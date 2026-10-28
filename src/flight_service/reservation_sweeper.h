#pragma once

#include "repository.h"

#include <yt/yt/core/actions/public.h>
#include <yt/yt/core/concurrency/public.h>

namespace NSoa::NFlight {

////////////////////////////////////////////////////////////////////////////////

//! Фоновая задача: раз в #period переводит неподтверждённые просроченные
//! резервации в EXPIRED и возвращает места (IFlightRepository::ExpireReservations).
//! Работает fiber'ом на #invoker; возвращается уже запущенной.
NYT::NConcurrency::TPeriodicExecutorPtr StartReservationSweeper(
    IFlightRepositoryPtr repository,
    NYT::IInvokerPtr invoker,
    TDuration period);

////////////////////////////////////////////////////////////////////////////////

} // namespace NSoa::NFlight
