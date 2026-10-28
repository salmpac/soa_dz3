#pragma once

#include <library/cpp/yt/error/error.h>

namespace NSoa::NRpc {

////////////////////////////////////////////////////////////////////////////////

//! Временная недоступность вызываемого сервиса - эквивалент gRPC UNAVAILABLE
//! и DEADLINE_EXCEEDED: транспортная ошибка, таймаут, Unavailable от сервиса.
//! Только такие ошибки имеет смысл повторять.
bool IsTransientError(const NYT::TError& error);

//! Сбой вызываемого сервиса (для circuit breaker): временная недоступность
//! или внутренняя ошибка. Бизнес-ошибки (NOT_FOUND, RESOURCE_EXHAUSTED, ...)
//! означают, что сервис работает, и сбоем не считаются.
bool IsServiceFailure(const NYT::TError& error);

////////////////////////////////////////////////////////////////////////////////

} // namespace NSoa::NRpc
