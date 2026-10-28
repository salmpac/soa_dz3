#pragma once

#include <yt/yt/core/rpc/public.h>

#include <library/cpp/yt/error/error.h>

#include <util/datetime/base.h>

#include <string>

namespace NSoa::NRpc {

////////////////////////////////////////////////////////////////////////////////

struct TRetryConfig
{
    //! Всего попыток, включая первую.
    int MaxAttempts = 3;
    //! Пауза перед первым повтором; дальше удваивается (100ms, 200ms, 400ms, ...).
    TDuration InitialBackoff = TDuration::MilliSeconds(100);
    TDuration MaxBackoff = TDuration::Seconds(2);
};

//! Interceptor: повторяет запрос при временной недоступности сервиса
//! (UNAVAILABLE, DEADLINE_EXCEEDED - см. IsTransientError) с экспоненциальным
//! backoff. Бизнес-ошибки (INVALID_ARGUMENT, NOT_FOUND, RESOURCE_EXHAUSTED, ...)
//! и разомкнутый circuit breaker не повторяются.
//!
//! Построен на YT-шном retrying channel; мутирующие методы должны быть
//! идемпотентными (ReserveSeats - по booking_id).
NYT::TErrorOr<NYT::NRpc::IChannelPtr> CreateRetryingChannel(
    NYT::NRpc::IChannelPtr underlying,
    const TRetryConfig& config,
    std::string name);

////////////////////////////////////////////////////////////////////////////////

} // namespace NSoa::NRpc
