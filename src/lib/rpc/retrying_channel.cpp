#include "retrying_channel.h"
#include "error_classification.h"

#include <soa/lib/common/error_macros.h>

#include <yt/yt/core/logging/log.h>

#include <yt/yt/core/rpc/config.h>
#include <yt/yt/core/rpc/retrying_channel.h>

namespace NSoa::NRpc {

using namespace NYT;
using namespace NYT::NRpc;

////////////////////////////////////////////////////////////////////////////////

YT_DEFINE_LEAKY_GLOBAL(const NLogging::TLogger, RetryLogger, "Retry");

////////////////////////////////////////////////////////////////////////////////

TErrorOr<IChannelPtr> CreateRetryingChannel(
    IChannelPtr underlying,
    const TRetryConfig& config,
    std::string name)
{
    if (config.MaxAttempts < 1) {
        return TError("Retry attempt count must be positive, got %v", config.MaxAttempts);
    }

    // Конфиги YT валидируются исключениями.
    SOA_ASSIGN_OR_RETURN(auto retryingConfig, CatchErrors([&] {
        auto retryingConfig = New<TRetryingChannelConfig>();
        retryingConfig->EnableExponentialRetryBackoffs = true;
        // Первая попытка не считается: InvocationCount - число повторов.
        retryingConfig->RetryBackoff.InvocationCount = config.MaxAttempts - 1;
        retryingConfig->RetryBackoff.MinBackoff = config.InitialBackoff;
        retryingConfig->RetryBackoff.MaxBackoff = config.MaxBackoff;
        retryingConfig->RetryBackoff.BackoffMultiplier = 2.0;
        retryingConfig->RetryBackoff.BackoffJitter = 0.0;
        retryingConfig->RetryTimeout = std::nullopt;
        return retryingConfig;
    }));

    auto Logger = RetryLogger().WithTag("Name", name);
    YT_LOG_INFO("Retrying channel created (MaxAttempts: %v, InitialBackoff: %v, MaxBackoff: %v)",
        config.MaxAttempts,
        config.InitialBackoff,
        config.MaxBackoff);

    auto retryChecker = BIND([Logger] (const TError& error) {
        bool retriable = IsTransientError(error);
        if (retriable) {
            YT_LOG_INFO(error, "Request attempt failed with retriable error, will retry if attempts remain");
        }
        return retriable;
    });

    return NYT::NRpc::CreateRetryingChannel(retryingConfig, std::move(underlying), std::move(retryChecker));
}

////////////////////////////////////////////////////////////////////////////////

} // namespace NSoa::NRpc
