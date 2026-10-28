#include "circuit_breaker_channel.h"
#include "error_classification.h"

#include <soa/lib/common/errors.h>

#include <yt/yt/core/logging/log.h>

#include <yt/yt/core/rpc/channel_detail.h>
#include <yt/yt/core/rpc/client.h>

#include <library/cpp/yt/system/spin_lock.h>

#include <util/system/guard.h>

#include <deque>

namespace NSoa::NRpc {

using namespace NYT;
using namespace NYT::NRpc;

////////////////////////////////////////////////////////////////////////////////

YT_DEFINE_LEAKY_GLOBAL(const NLogging::TLogger, CircuitBreakerLogger, "CircuitBreaker");

////////////////////////////////////////////////////////////////////////////////

namespace {

//! Конечный автомат. Все переходы - под спинлоком; логирование - вне его.
class TCircuitBreaker
    : public NYT::TRefCounted
{
public:
    TCircuitBreaker(TCircuitBreakerConfig config, std::string name)
        : Config_(config)
        , Logger(CircuitBreakerLogger().WithTag("Name", name))
    {
        YT_LOG_INFO("Circuit breaker created (State: %v, FailureThreshold: %v, FailureWindow: %v, OpenTimeout: %v)",
            ECircuitBreakerState::Closed,
            Config_.FailureThreshold,
            Config_.FailureWindow,
            Config_.OpenTimeout);
    }

    //! Можно ли отправить запрос. Если да и это пробный запрос, *probe = true.
    bool TryAcquire(bool* probe)
    {
        *probe = false;
        std::optional<TTransition> transition;
        bool allowed;
        {
            auto guard = Guard(Lock_);
            switch (State_) {
                case ECircuitBreakerState::Closed:
                    allowed = true;
                    break;

                case ECircuitBreakerState::Open:
                    if (TInstant::Now() < OpenedAt_ + Config_.OpenTimeout) {
                        allowed = false;
                        break;
                    }
                    transition = SetState(ECircuitBreakerState::HalfOpen, "open timeout elapsed");
                    [[fallthrough]];

                case ECircuitBreakerState::HalfOpen:
                    // В HalfOpen пропускаем ровно один пробный запрос.
                    allowed = !ProbeInFlight_;
                    if (allowed) {
                        ProbeInFlight_ = true;
                        *probe = true;
                    }
                    break;
            }
        }
        LogTransition(transition);
        return allowed;
    }

    void OnResult(const TError& error, bool probe)
    {
        bool failure = IsServiceFailure(error);
        auto now = TInstant::Now();
        std::optional<TTransition> transition;
        {
            auto guard = Guard(Lock_);
            if (probe) {
                ProbeInFlight_ = false;
            }

            if (!failure) {
                Failures_.clear();
                if (State_ == ECircuitBreakerState::HalfOpen && probe) {
                    transition = SetState(ECircuitBreakerState::Closed, "probe request succeeded");
                }
            } else if (State_ == ECircuitBreakerState::HalfOpen && probe) {
                OpenedAt_ = now;
                transition = SetState(ECircuitBreakerState::Open, "probe request failed");
            } else if (State_ == ECircuitBreakerState::Closed) {
                Failures_.push_back(now);
                while (!Failures_.empty() && Failures_.front() + Config_.FailureWindow < now) {
                    Failures_.pop_front();
                }
                if (std::ssize(Failures_) >= Config_.FailureThreshold) {
                    Failures_.clear();
                    OpenedAt_ = now;
                    transition = SetState(
                        ECircuitBreakerState::Open,
                        Format("%v consecutive failures within %v", Config_.FailureThreshold, Config_.FailureWindow));
                }
            }
        }
        LogTransition(transition, failure ? error : TError());
    }

    TError MakeOpenError() const
    {
        return MakeError(
            NSoa::EErrorCode::CircuitOpen,
            "Service is unavailable: circuit breaker is open, retry later");
    }

private:
    struct TTransition
    {
        ECircuitBreakerState From;
        ECircuitBreakerState To;
        std::string Reason;
    };

    const TCircuitBreakerConfig Config_;
    const NLogging::TLogger Logger;

    YT_DECLARE_SPIN_LOCK(NYT::TSpinLock, Lock_);
    ECircuitBreakerState State_ = ECircuitBreakerState::Closed;
    std::deque<TInstant> Failures_;
    TInstant OpenedAt_;
    bool ProbeInFlight_ = false;

    TTransition SetState(ECircuitBreakerState state, std::string reason)
    {
        TTransition transition{State_, state, std::move(reason)};
        State_ = state;
        return transition;
    }

    void LogTransition(const std::optional<TTransition>& transition, const TError& lastError = {})
    {
        if (!transition) {
            return;
        }
        if (lastError.IsOK()) {
            YT_LOG_WARNING("Circuit breaker state changed: %v -> %v (Reason: %v)",
                transition->From,
                transition->To,
                transition->Reason);
        } else {
            YT_LOG_WARNING(lastError, "Circuit breaker state changed: %v -> %v (Reason: %v)",
                transition->From,
                transition->To,
                transition->Reason);
        }
    }
};

using TCircuitBreakerPtr = NYT::TIntrusivePtr<TCircuitBreaker>;

////////////////////////////////////////////////////////////////////////////////

//! Пропускает ответ к исходному обработчику, сообщив результат автомату.
class TCircuitBreakerResponseHandler
    : public IClientResponseHandler
{
public:
    TCircuitBreakerResponseHandler(IClientResponseHandlerPtr underlying, TCircuitBreakerPtr breaker, bool probe)
        : Underlying_(std::move(underlying))
        , Breaker_(std::move(breaker))
        , Probe_(probe)
    { }

    void HandleAcknowledgement() override
    {
        Underlying_->HandleAcknowledgement();
    }

    void HandleResponse(
        TSharedRefArray message,
        const std::string& address,
        NBus::IDirectPlacementTransferPtr attachmentsTransfer) override
    {
        Breaker_->OnResult(TError(), Probe_);
        Underlying_->HandleResponse(std::move(message), address, std::move(attachmentsTransfer));
    }

    void HandleError(TError error, const std::string& address) override
    {
        Breaker_->OnResult(error, Probe_);
        Underlying_->HandleError(std::move(error), address);
    }

    void HandleStreamingPayload(const TStreamingPayload& payload) override
    {
        Underlying_->HandleStreamingPayload(payload);
    }

    void HandleStreamingFeedback(const TStreamingFeedback& feedback) override
    {
        Underlying_->HandleStreamingFeedback(feedback);
    }

private:
    const IClientResponseHandlerPtr Underlying_;
    const TCircuitBreakerPtr Breaker_;
    const bool Probe_;
};

////////////////////////////////////////////////////////////////////////////////

class TCircuitBreakerChannel
    : public TChannelWrapper
{
public:
    TCircuitBreakerChannel(IChannelPtr underlying, TCircuitBreakerConfig config, std::string name)
        : TChannelWrapper(std::move(underlying))
        , Breaker_(New<TCircuitBreaker>(config, std::move(name)))
    { }

    IClientRequestControlPtr Send(
        IClientRequestPtr request,
        IClientResponseHandlerPtr responseHandler,
        const TSendOptions& options) override
    {
        bool probe = false;
        if (!Breaker_->TryAcquire(&probe)) {
            // Реальный вызов не делаем: клиент сразу получает осмысленную ошибку.
            responseHandler->HandleError(Breaker_->MakeOpenError()
                .With("service", request->GetService())
                .With("method", request->GetMethod()));
            return New<TClientRequestControlThunk>();
        }

        return UnderlyingChannel_->Send(
            std::move(request),
            New<TCircuitBreakerResponseHandler>(std::move(responseHandler), Breaker_, probe),
            options);
    }

private:
    const TCircuitBreakerPtr Breaker_;
};

} // namespace

////////////////////////////////////////////////////////////////////////////////

IChannelPtr CreateCircuitBreakerChannel(
    IChannelPtr underlying,
    TCircuitBreakerConfig config,
    std::string name)
{
    return New<TCircuitBreakerChannel>(std::move(underlying), config, std::move(name));
}

////////////////////////////////////////////////////////////////////////////////

} // namespace NSoa::NRpc
