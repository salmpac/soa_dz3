#include "api_key_channel.h"

#include <yt/yt/core/rpc/channel_detail.h>
#include <yt/yt/core/rpc/client.h>

#include <yt/yt_proto/yt/core/rpc/proto/rpc.pb.h>

namespace NSoa::NRpc {

using namespace NYT;
using namespace NYT::NRpc;

////////////////////////////////////////////////////////////////////////////////

namespace {

class TApiKeyChannel
    : public TChannelWrapper
{
public:
    TApiKeyChannel(IChannelPtr underlying, std::string apiKey)
        : TChannelWrapper(std::move(underlying))
        , ApiKey_(std::move(apiKey))
    { }

    IClientRequestControlPtr Send(
        IClientRequestPtr request,
        IClientResponseHandlerPtr responseHandler,
        const TSendOptions& options) override
    {
        // Custom metadata из заголовка YT gRPC-канал отправляет как обычные gRPC metadata.
        auto* ext = request->Header().MutableExtension(NYT::NRpc::NProto::TCustomMetadataExt::custom_metadata_ext);
        (*ext->mutable_entries())["x-api-key"] = ApiKey_;
        return UnderlyingChannel_->Send(std::move(request), std::move(responseHandler), options);
    }

private:
    const std::string ApiKey_;
};

} // namespace

////////////////////////////////////////////////////////////////////////////////

IChannelPtr CreateApiKeyChannel(IChannelPtr underlying, std::string apiKey)
{
    return New<TApiKeyChannel>(std::move(underlying), std::move(apiKey));
}

////////////////////////////////////////////////////////////////////////////////

} // namespace NSoa::NRpc
