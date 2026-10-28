#ifndef CLIENT_INL_H_
#error "Direct inclusion of this file is not allowed, include client.h"
// For the sake of sane code completion.
#include "client.h"
#endif

#include <soa/lib/common/error_macros.h>

#include <util/string/cast.h>

namespace NSoa::NPg {

////////////////////////////////////////////////////////////////////////////////

namespace NDetail {

template <class T>
std::optional<T> ParseValue(const std::string& raw)
{
    if constexpr (std::is_same_v<T, std::string>) {
        return raw;
    } else if constexpr (std::is_same_v<T, bool>) {
        if (raw == "t" || raw == "f") {
            return raw == "t";
        }
        return std::nullopt;
    } else {
        T value;
        if (!TryFromString(raw, value)) {
            return std::nullopt;
        }
        return value;
    }
}

} // namespace NDetail

////////////////////////////////////////////////////////////////////////////////

template <class T>
std::optional<T> TPgRowReader::GetOptional(TStringBuf column)
{
    const auto* raw = FindRaw(column);
    if (!raw || !*raw) {
        return std::nullopt;
    }
    auto value = NDetail::ParseValue<T>(**raw);
    if (!value && Error_.IsOK()) {
        Error_ = NYT::TError("Cannot parse value %Qv of column %Qv", **raw, column);
    }
    return value;
}

template <class T>
T TPgRowReader::Get(TStringBuf column)
{
    const auto* raw = FindRaw(column);
    if (raw && !*raw && Error_.IsOK()) {
        Error_ = NYT::TError("Column %Qv is NULL", column);
    }
    return GetOptional<T>(column).value_or(T{});
}

template <class T>
NYT::TErrorOr<T> TPgResult::Get(int row, TStringBuf column) const
{
    auto reader = Row(row);
    auto value = reader.Get<T>(column);
    SOA_RETURN_IF_ERROR(reader.GetError());
    return value;
}

////////////////////////////////////////////////////////////////////////////////

template <class TCallback>
auto TPgClient::WithTransaction(TCallback&& callback) -> std::invoke_result_t<TCallback, TPgConnection&>
{
    SOA_ASSIGN_OR_RETURN(auto guard, Acquire());
    SOA_RETURN_IF_ERROR(guard->Execute("BEGIN"));

    auto result = callback(*guard);
    if (!result.IsOK()) {
        if (auto rollbackError = guard->Execute("ROLLBACK"); !rollbackError.IsOK()) {
            // Состояние соединения неизвестно - не возвращаем его в пул.
            guard->Abort();
        }
        return result;
    }

    SOA_RETURN_IF_ERROR(guard->Execute("COMMIT"));
    return result;
}

////////////////////////////////////////////////////////////////////////////////

} // namespace NSoa::NPg
