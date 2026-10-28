#include "errors.h"

namespace NSoa {

using namespace NYT;

////////////////////////////////////////////////////////////////////////////////

int ToGrpcStatusCode(EErrorCode code)
{
    // Значения из grpc/status.h.
    switch (code) {
        case EErrorCode::InvalidArgument:    return 3;
        case EErrorCode::NotFound:           return 5;
        case EErrorCode::AlreadyExists:      return 6;
        case EErrorCode::ResourceExhausted:  return 8;
        case EErrorCode::FailedPrecondition: return 9;
        case EErrorCode::Internal:           return 13;
        case EErrorCode::Unavailable:        return 14;
        case EErrorCode::CircuitOpen:        return 14;
        case EErrorCode::Unauthenticated:    return 16;
    }
    return 2;  // UNKNOWN
}

namespace {

bool IsBusinessErrorCode(TErrorCode code)
{
    for (auto value : TEnumTraits<EErrorCode>::GetDomainValues()) {
        if (static_cast<int>(value) == static_cast<int>(code)) {
            return true;
        }
    }
    return false;
}

} // namespace

std::optional<TError> FindBusinessError(const TError& error)
{
    return error.FindMatching([] (const TError& candidate) {
        return IsBusinessErrorCode(candidate.GetCode());
    });
}

std::optional<EErrorCode> FindBusinessErrorCode(const TError& error)
{
    if (auto businessError = FindBusinessError(error)) {
        return static_cast<EErrorCode>(static_cast<int>(businessError->GetCode()));
    }
    return std::nullopt;
}

////////////////////////////////////////////////////////////////////////////////

} // namespace NSoa
