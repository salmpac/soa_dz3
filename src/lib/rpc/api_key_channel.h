#pragma once

#include <yt/yt/core/rpc/public.h>

#include <string>

namespace NSoa::NRpc {

////////////////////////////////////////////////////////////////////////////////

//! Interceptor: добавляет API key в gRPC metadata "x-api-key" каждого запроса.
NYT::NRpc::IChannelPtr CreateApiKeyChannel(NYT::NRpc::IChannelPtr underlying, std::string apiKey);

////////////////////////////////////////////////////////////////////////////////

} // namespace NSoa::NRpc
