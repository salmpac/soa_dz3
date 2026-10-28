#pragma once

#include <soa/lib/common/error_macros.h>

#include <yt/yt/core/concurrency/async_semaphore.h>
#include <yt/yt/core/concurrency/scheduler_api.h>

#include <library/cpp/yt/system/spin_lock.h>

#include <util/system/guard.h>

#include <atomic>
#include <functional>
#include <vector>

namespace NSoa::NNet {

////////////////////////////////////////////////////////////////////////////////

//! Пул соединений с асинхронным ожиданием свободного слота.
/*!
 *  TConnection - ref-counted тип с методом `bool IsReusable() const`.
 *  Фабрика вызывается внутри fiber'а, может "блокироваться" через WaitFor
 *  и возвращает новое соединение или ошибку.
 */
template <class TConnection>
class TConnectionPool
    : public NYT::TRefCounted
{
public:
    using TConnectionPtr = NYT::TIntrusivePtr<TConnection>;
    using TFactory = std::function<NYT::TErrorOr<TConnectionPtr>()>;

    //! Владеет соединением, пока fiber с ним работает; в деструкторе возвращает его в пул.
    class TGuard
    {
    public:
        TGuard() = default;
        TGuard(TGuard&&) = default;
        TGuard& operator=(TGuard&&) = default;

        ~TGuard()
        {
            if (Connection_) {
                Pool_->Return(std::move(Connection_), Epoch_);
            }
        }

        TConnection* operator->() const
        {
            return Connection_.Get();
        }

        TConnection& operator*() const
        {
            return *Connection_;
        }

        //! Не возвращать соединение в пул (например, состояние протокола неизвестно).
        void Discard()
        {
            Connection_.Reset();
        }

    private:
        friend class TConnectionPool;

        NYT::TIntrusivePtr<TConnectionPool> Pool_;
        NYT::NConcurrency::TAsyncSemaphoreGuard SemaphoreGuard_;
        TConnectionPtr Connection_;
        i64 Epoch_ = 0;
    };

    TConnectionPool(int maxSize, TFactory factory)
        : Semaphore_(NYT::New<NYT::NConcurrency::TAsyncSemaphore>(maxSize))
        , Factory_(std::move(factory))
    { }

    //! Ждёт свободный слот (без таймаута: дедлайн задаёт вызывающий запрос,
    //! а слоты гарантированно освобождаются RAII-guard'ами) и выдаёт соединение:
    //! простаивающее или новое из фабрики.
    NYT::TErrorOr<TGuard> Acquire()
    {
        auto semaphoreGuardOrError = NYT::NConcurrency::WaitFor(Semaphore_->AsyncAcquire().AsUnique());
        SOA_RETURN_IF_ERROR_WRAP(semaphoreGuardOrError, "Failed to acquire connection from pool");

        TGuard guard;
        guard.Pool_ = this;
        guard.SemaphoreGuard_ = std::move(semaphoreGuardOrError).Value();
        guard.Epoch_ = Epoch_.load();

        {
            auto lockGuard = Guard(Lock_);
            if (!Idle_.empty()) {
                guard.Connection_ = std::move(Idle_.back());
                Idle_.pop_back();
            }
        }

        if (!guard.Connection_) {
            SOA_ASSIGN_OR_RETURN(guard.Connection_, Factory_());
        }
        return guard;
    }

    //! Закрывает все простаивающие соединения; занятые не вернутся в пул.
    void Reset()
    {
        std::vector<TConnectionPtr> idle;
        {
            auto lockGuard = Guard(Lock_);
            ++Epoch_;
            idle.swap(Idle_);
        }
        // Соединения закрываются уже вне спинлока.
    }

private:
    const NYT::NConcurrency::TAsyncSemaphorePtr Semaphore_;
    const TFactory Factory_;

    YT_DECLARE_SPIN_LOCK(NYT::TSpinLock, Lock_);
    std::vector<TConnectionPtr> Idle_;
    std::atomic<i64> Epoch_ = 0;

    void Return(TConnectionPtr connection, i64 epoch)
    {
        if (!connection->IsReusable()) {
            return;
        }
        auto lockGuard = Guard(Lock_);
        if (epoch == Epoch_.load()) {
            Idle_.push_back(std::move(connection));
        }
    }
};

////////////////////////////////////////////////////////////////////////////////

} // namespace NSoa::NNet
