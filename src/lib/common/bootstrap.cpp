#include "bootstrap.h"
#include "env.h"

#include <yt/yt/core/concurrency/action_queue.h>

#include <yt/yt/core/logging/config.h>
#include <yt/yt/core/logging/log.h>
#include <yt/yt/core/logging/log_manager.h>

#include <yt/yt/core/net/address.h>
#include <yt/yt/core/net/config.h>

#include <yt/yt/core/misc/shutdown.h>

#include <iostream>

#include <signal.h>

namespace NSoa {

using namespace NYT;
using namespace NYT::NConcurrency;

////////////////////////////////////////////////////////////////////////////////

YT_DEFINE_LEAKY_GLOBAL(const NLogging::TLogger, BootstrapLogger, "Bootstrap");
constinit const auto Logger = BootstrapLogger;

TError ConfigureProcess()
{
    SOA_ASSIGN_OR_RETURN(auto logLevel, GetEnvValue("LOG_LEVEL", NLogging::ELogLevel::Info));
    SOA_ASSIGN_OR_RETURN(auto logYtInternals, GetEnvValue("LOG_YT_INTERNALS", false));
    SOA_ASSIGN_OR_RETURN(auto enableIPv6, GetEnvValue("ENABLE_IPV6", false));

    // Конфиги YT валидируются исключениями - это граница с бросающим API.
    return CatchErrors([&] {
        auto logConfig = NLogging::TLogManagerConfig::CreateStderrLogger(logLevel);
        // Внутренние debug-логи YT (poller, fiber'ы, соединения) слишком шумные.
        if (logLevel <= NLogging::ELogLevel::Debug && !logYtInternals) {
            const auto& mainRule = logConfig->Rules[0];
            mainRule->ExcludeCategories = {"Concurrency", "Net", "Dns", "Rpc", "RpcServer", "RpcClient", "Grpc", "Http", "Bus", "Tls"};

            auto internalsRule = New<NLogging::TRuleConfig>();
            internalsRule->MinLevel = NLogging::ELogLevel::Info;
            internalsRule->IncludeCategories = mainRule->ExcludeCategories;
            internalsRule->Writers = mainRule->Writers;
            logConfig->Rules.push_back(internalsRule);
        }
        NLogging::TLogManager::Get()->Configure(logConfig);

        auto resolverConfig = New<NNet::TAddressResolverConfig>();
        resolverConfig->EnableIPv4 = true;
        resolverConfig->EnableIPv6 = enableIPv6;
        NNet::TAddressResolver::Get()->Configure(resolverConfig);
    });
}

int RunInFiber(std::function<TError()> main)
{
    auto queue = New<TActionQueue>("Main");
    TError mainError;
    // Ошибка future - сбой доставки/отмена, mainError - результат самой main.
    auto deliveryError = BIND([&] {
            mainError = main();
        })
        .AsyncVia(queue->GetInvoker())
        .Run()
        .BlockingGet();
    auto error = deliveryError.IsOK() ? mainError : deliveryError;
    if (!error.IsOK()) {
        YT_LOG_ERROR(error, "Fatal error");
        NLogging::TLogManager::Get()->Shutdown();
        return 1;
    }
    return 0;
}

int RunProgram(std::function<TError()> main)
{
    if (auto error = ConfigureProcess(); !error.IsOK()) {
        std::cerr << "Failed to configure process: " << ToString(error) << std::endl;
        return 1;
    }
    return RunInFiber(std::move(main));
}

int RunService(std::function<TError()> start, std::function<TError()> stop)
{
    sigset_t terminationSignals;
    sigemptyset(&terminationSignals);
    sigaddset(&terminationSignals, SIGTERM);
    sigaddset(&terminationSignals, SIGINT);
    // До ConfigureProcess: дальше YT начнёт создавать потоки, и они унаследуют маску.
    if (int error = pthread_sigmask(SIG_BLOCK, &terminationSignals, nullptr); error != 0) {
        std::cerr << "pthread_sigmask failed: " << error << std::endl;
        return 1;
    }

    if (auto exitCode = RunProgram(std::move(start)); exitCode != 0) {
        return exitCode;
    }

    int signal = 0;
    if (int error = sigwait(&terminationSignals, &signal); error != 0) {
        YT_LOG_ERROR("sigwait failed (Error: %v)", error);
        return 1;
    }
    YT_LOG_INFO("Received termination signal, shutting down (Signal: %v)", strsignal(signal));

    auto exitCode = RunInFiber(std::move(stop));
    if (exitCode == 0) {
        YT_LOG_INFO("Shutdown completed");
    }
    // Останавливает фоновые потоки YT (poller'ы, логирование и т.п.) и сбрасывает логи.
    Shutdown({.GraceTimeout = TDuration::Seconds(5), .AbortOnHang = false, .HungExitCode = exitCode});
    return exitCode;
}

////////////////////////////////////////////////////////////////////////////////

} // namespace NSoa
