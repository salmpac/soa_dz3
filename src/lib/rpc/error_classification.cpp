#include "error_classification.h"

#include <soa/lib/common/errors.h>

#include <yt/yt/core/rpc/public.h>

namespace NSoa::NRpc {

using namespace NYT;

////////////////////////////////////////////////////////////////////////////////

bool IsTransientError(const TError& error)
{
    if (error.IsOK()) {
        return false;
    }
    // Бизнес-ошибка от сервиса решает сама: повторяем только его Unavailable.
    if (auto businessCode = FindBusinessErrorCode(error)) {
        return *businessCode == NSoa::EErrorCode::Unavailable;
    }
    return static_cast<bool>(error.FindMatching(THashSet<TErrorCode>{
        NYT::EErrorCode::Timeout,                   // DEADLINE_EXCEEDED
        NYT::NRpc::EErrorCode::TransportError,      // UNAVAILABLE (соединение)
        NYT::NRpc::EErrorCode::Unavailable,
    }));
}

bool IsServiceFailure(const TError& error)
{
    if (error.IsOK()) {
        return false;
    }
    if (auto businessCode = FindBusinessErrorCode(error)) {
        return
            *businessCode == NSoa::EErrorCode::Unavailable ||
            *businessCode == NSoa::EErrorCode::Internal;
    }
    // Не бизнес-ошибка: транспорт, таймаут или неизвестный сбой сервиса.
    return true;
}

////////////////////////////////////////////////////////////////////////////////

} // namespace NSoa::NRpc
