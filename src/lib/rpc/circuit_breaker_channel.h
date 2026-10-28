#pragma once

#include <yt/yt/core/rpc/public.h>

#include <library/cpp/yt/misc/enum.h>

#include <util/datetime/base.h>

#include <string>

namespace NSoa::NRpc {

////////////////////////////////////////////////////////////////////////////////

DEFINE_ENUM(ECircuitBreakerState,
    //! Штатная работа: запросы идут к сервису.
    (Closed)
    //! Сервис считается недоступным: запросы сразу завершаются ошибкой CircuitOpen.
    (Open)
    //! Пробный режим: пропускается один запрос; успех -> Closed, сбой -> Open.
    (HalfOpen)
);

struct TCircuitBreakerConfig
{
    //! Сколько сбоев подряд (без успешных ответов между ними) размыкает цепь.
    int FailureThreshold = 5;
    //! Окно, в которое должны уложиться эти сбои: более старые забываются.
    TDuration FailureWindow = TDuration::Seconds(10);
    //! Сколько цепь остаётся разомкнутой до пробного запроса.
    TDuration OpenTimeout = TDuration::Seconds(15);
};

//! Interceptor: Circuit Breaker для вызовов через #underlying.
/*!
 *  Сбоем считается ошибка из NSoa::NRpc::IsServiceFailure (транспорт, таймаут,
 *  UNAVAILABLE/INTERNAL от сервиса); бизнес-ошибки цепь не размыкают.
 *  В состоянии Open запрос не отправляется, а клиент сразу получает ошибку
 *  с кодом NSoa::EErrorCode::CircuitOpen. Переходы состояний логируются.
 */
NYT::NRpc::IChannelPtr CreateCircuitBreakerChannel(
    NYT::NRpc::IChannelPtr underlying,
    TCircuitBreakerConfig config,
    std::string name);

////////////////////////////////////////////////////////////////////////////////

} // namespace NSoa::NRpc
