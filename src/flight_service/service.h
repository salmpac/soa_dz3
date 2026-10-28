#pragma once

#include "repository.h"

#include <yt/yt/core/rpc/public.h>

namespace NSoa::NFlight {

////////////////////////////////////////////////////////////////////////////////

//! Аутентификация межсервисных вызовов по API key из gRPC metadata "x-api-key".
NYT::NRpc::IAuthenticatorPtr CreateApiKeyAuthenticator(std::string apiKey);

//! gRPC-сервис flight.v1.FlightService.
NYT::NRpc::IServicePtr CreateFlightService(
    NYT::IInvokerPtr invoker,
    IFlightRepositoryPtr repository,
    NYT::NRpc::IAuthenticatorPtr authenticator);

////////////////////////////////////////////////////////////////////////////////

} // namespace NSoa::NFlight
