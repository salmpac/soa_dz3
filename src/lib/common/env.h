#pragma once

#include "error_macros.h"

#include <library/cpp/yt/error/error.h>
#include <library/cpp/yt/misc/enum.h>
#include <library/cpp/yt/string/enum.h>

#include <util/datetime/base.h>
#include <util/string/cast.h>
#include <util/system/env.h>

#include <string>

namespace NSoa {

////////////////////////////////////////////////////////////////////////////////

//! Значение переменной окружения или #defaultValue.
inline std::string GetEnvString(const char* name, const std::string& defaultValue = {})
{
    return std::string(GetEnv(name, TString(defaultValue)));
}

//! Обязательная переменная окружения.
inline NYT::TErrorOr<std::string> GetRequiredEnvString(const char* name)
{
    auto value = GetEnv(name);
    if (value.empty()) {
        return NYT::TError("Environment variable %v is required", name);
    }
    return std::string(value);
}

//! Типизированное значение переменной окружения или #defaultValue.
//! Длительности (TDuration) задаются в миллисекундах.
template <class T>
NYT::TErrorOr<T> GetEnvValue(const char* name, T defaultValue)
{
    auto value = GetEnv(name);
    if (value.empty()) {
        return defaultValue;
    }

    auto invalid = [&] {
        return NYT::TError("Invalid value %Qv of environment variable %v", value, name);
    };

    if constexpr (std::is_same_v<T, TDuration>) {
        ui64 milliseconds;
        if (!TryFromString(value, milliseconds)) {
            return invalid();
        }
        return TDuration::MilliSeconds(milliseconds);
    } else if constexpr (NYT::TEnumTraits<T>::IsEnum) {
        if (auto parsed = NYT::TryParseEnum<T>(value)) {
            return *parsed;
        }
        return invalid();
    } else {
        T result;
        if (!TryFromString(value, result)) {
            return invalid();
        }
        return result;
    }
}

////////////////////////////////////////////////////////////////////////////////

} // namespace NSoa
