#pragma once

#include <library/cpp/yt/error/error.h>

#include <type_traits>

//! Ошибки в проекте - значения: функции возвращают NYT::TError / NYT::TErrorOr<T>,
//! а эти макросы пробрасывают их наверх (как `if err != nil { return err }` в Go).
//!
//! Исключения в нашем коде не используются. Единственное, что по-прежнему
//! раскручивает стек, - отмена fiber'а (TFiberCanceledException из WaitFor);
//! её не ловим, ресурсы освобождаются через RAII.

////////////////////////////////////////////////////////////////////////////////

#define SOA_CONCAT_IMPL(a, b) a##b
#define SOA_CONCAT(a, b) SOA_CONCAT_IMPL(a, b)

//! Если #expr (TError или TErrorOr<T>) - ошибка, возвращает её из текущей функции.
#define SOA_RETURN_IF_ERROR(...) \
    do { \
        if (const ::NYT::TError& soaError__ = (__VA_ARGS__); !soaError__.IsOK()) { \
            return soaError__; \
        } \
    } while (false)

//! То же, но оборачивает ошибку в новую с сообщением: SOA_RETURN_IF_ERROR_WRAP(expr, "fmt %v", arg).
#define SOA_RETURN_IF_ERROR_WRAP(expr, ...) \
    do { \
        if (const ::NYT::TError& soaError__ = (expr); !soaError__.IsOK()) { \
            return soaError__.Wrap(__VA_ARGS__); \
        } \
    } while (false)

//! `SOA_ASSIGN_OR_RETURN(auto value, Compute());` - достаёт значение из TErrorOr<T>
//! или возвращает ошибку из текущей функции.
#define SOA_ASSIGN_OR_RETURN(lhs, ...) \
    SOA_ASSIGN_OR_RETURN_IMPL(SOA_CONCAT(soaResult__, __LINE__), lhs, __VA_ARGS__)

#define SOA_ASSIGN_OR_RETURN_IMPL(result, lhs, ...) \
    auto result = (__VA_ARGS__); \
    if (!result.IsOK()) { \
        return static_cast<::NYT::TError&&>(result); \
    } \
    lhs = std::move(result).Value()

////////////////////////////////////////////////////////////////////////////////

namespace NSoa {

//! Граница с бросающим API (библиотеки YT/util): вызывает #callback и
//! превращает исключение в TError. Отмена fiber'а не перехватывается -
//! TFiberCanceledException не наследуется от std::exception.
template <class TCallback>
auto CatchErrors(TCallback&& callback) -> NYT::TErrorOr<std::invoke_result_t<TCallback>>
{
    try {
        if constexpr (std::is_void_v<std::invoke_result_t<TCallback>>) {
            callback();
            return {};
        } else {
            return callback();
        }
    } catch (const std::exception& ex) {
        return NYT::TError(ex);
    }
}

} // namespace NSoa
