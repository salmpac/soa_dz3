#pragma once

#include <library/cpp/yt/error/error.h>

namespace NSoa {

////////////////////////////////////////////////////////////////////////////////

//! Бизнес-ошибки сервисов. Каждой соответствует стандартный gRPC status code
//! (см. ToGrpcStatusCode), который Flight Service отдаёт клиентам.
YT_DEFINE_ERROR_ENUM(
    ((InvalidArgument)      (31000))
    ((NotFound)             (31001))
    ((ResourceExhausted)    (31002))
    ((FailedPrecondition)   (31003))
    ((AlreadyExists)        (31004))
    ((Unauthenticated)      (31005))
    ((Unavailable)          (31006))
    ((Internal)             (31007))
    //! Circuit breaker разомкнут: запрос не отправлялся.
    ((CircuitOpen)          (31008))
);

//! Атрибут ошибки, по которому (пропатченный) YT gRPC-сервер выбирает status code.
//! Совпадает с NYT::NRpc::NGrpc::GrpcStatusCodeAttributeKey из patches/ytsaurus.patch.
inline constexpr TStringBuf GrpcStatusCodeAttributeKey = "grpc_status_code";

//! Числовое значение grpc_status_code для бизнес-ошибки.
int ToGrpcStatusCode(EErrorCode code);

//! Ошибка с кодом #code и атрибутом grpc_status_code.
template <class... TArgs>
NYT::TError MakeError(EErrorCode code, NYT::TFormatString<TArgs...> format, TArgs&&... args)
{
    return NYT::TError(code, format, std::forward<TArgs>(args)...)
        .With(std::string(GrpcStatusCodeAttributeKey), ToGrpcStatusCode(code));
}

//! Ищет в цепочке ошибок первую бизнес-ошибку (код из EErrorCode).
std::optional<NYT::TError> FindBusinessError(const NYT::TError& error);

//! Код первой бизнес-ошибки в цепочке.
std::optional<EErrorCode> FindBusinessErrorCode(const NYT::TError& error);

////////////////////////////////////////////////////////////////////////////////

} // namespace NSoa
