// Flight Service: gRPC API рейсов и резервирования мест (PostgreSQL + Redis).

#include "flight_repository_with_redis.h"
#include "pg_repository.h"
#include "reservation_sweeper.h"

#include <yt/yt/core/concurrency/periodic_executor.h>
#include "service.h"

#include <soa/lib/common/bootstrap.h>
#include <soa/lib/common/env.h>
#include <soa/lib/common/error_macros.h>
#include <soa/lib/pg/env_config.h>

#include <yt/yt/core/concurrency/scheduler_api.h>
#include <yt/yt/core/concurrency/thread_pool.h>
#include <yt/yt/core/concurrency/thread_pool_poller.h>

#include <yt/yt/core/crypto/config.h>

#include <yt/yt/core/logging/log.h>

#include <yt/yt/core/rpc/grpc/config.h>
#include <yt/yt/core/rpc/grpc/server.h>
#include <yt/yt/core/rpc/server.h>

#include <util/string/split.h>

using namespace NYT;
using namespace NYT::NConcurrency;
using namespace NSoa;
using namespace NSoa::NFlight;

////////////////////////////////////////////////////////////////////////////////

YT_DEFINE_LEAKY_GLOBAL(const NLogging::TLogger, MainLogger, "Main");
constinit const auto Logger = MainLogger;

////////////////////////////////////////////////////////////////////////////////

namespace {

//! std::nullopt - кеш выключен (CACHE_ENABLED=false).
TErrorOr<std::optional<NRedis::TRedisConfig>> LoadRedisConfig()
{
    SOA_ASSIGN_OR_RETURN(auto enabled, GetEnvValue("CACHE_ENABLED", true));
    if (!enabled) {
        return std::optional<NRedis::TRedisConfig>();
    }

    NRedis::TRedisConfig config;
    for (const auto& part : StringSplitter(GetEnvString("REDIS_SENTINELS")).Split(',').SkipEmpty()) {
        SOA_ASSIGN_OR_RETURN(auto sentinel, NRedis::ParseEndpoint(part.Token(), 26379));
        config.Sentinels.push_back(std::move(sentinel));
    }
    config.MasterName = GetEnvString("REDIS_MASTER_NAME", "mymaster");
    SOA_ASSIGN_OR_RETURN(config.StandaloneEndpoint, NRedis::ParseEndpoint(GetEnvString("REDIS_ADDRESS", "localhost:6379"), 6379));
    config.Password = GetEnvString("REDIS_PASSWORD");
    config.SentinelPassword = GetEnvString("REDIS_SENTINEL_PASSWORD");
    SOA_ASSIGN_OR_RETURN(config.TlsMode, GetEnvValue("REDIS_TLS_MODE", NRedis::ERedisTlsMode::Disable));
    config.TlsCaCertPath = GetEnvString("REDIS_TLS_CA");
    SOA_ASSIGN_OR_RETURN(config.PoolSize, GetEnvValue("REDIS_POOL_SIZE", 16));
    return std::optional<NRedis::TRedisConfig>(std::move(config));
}

NRpc::NGrpc::TServerConfigPtr LoadGrpcServerConfig()
{
    auto addressConfig = New<NRpc::NGrpc::TServerAddressConfig>();
    addressConfig->Address = GetEnvString("GRPC_ADDRESS", "0.0.0.0:50051");

    auto certPath = GetEnvString("GRPC_TLS_CERT");
    auto keyPath = GetEnvString("GRPC_TLS_KEY");
    if (!certPath.empty() || !keyPath.empty()) {
        auto keyCertPair = New<NRpc::NGrpc::TSslPemKeyCertPairConfig>();
        keyCertPair->CertChain = NCrypto::TPemBlobConfig::CreateFileReference(certPath);
        keyCertPair->PrivateKey = NCrypto::TPemBlobConfig::CreateFileReference(keyPath);

        auto credentials = New<NRpc::NGrpc::TServerCredentialsConfig>();
        credentials->PemKeyCertPairs.push_back(keyCertPair);

        // mTLS: с CA для клиентов сервер принимает только клиентов с сертификатом,
        // подписанным этим CA (поверх этого каждый запрос проверяется по API key).
        // Без него - только серверный TLS (по умолчанию YT требовал бы сертификат).
        auto clientCaPath = GetEnvString("GRPC_TLS_CLIENT_CA");
        if (!clientCaPath.empty()) {
            credentials->PemRootCerts = NCrypto::TPemBlobConfig::CreateFileReference(clientCaPath);
            credentials->ClientCertificateRequest = NRpc::NGrpc::EClientCertificateRequest::RequestAndRequireClientCertificateAndVerify;
        } else {
            credentials->ClientCertificateRequest = NRpc::NGrpc::EClientCertificateRequest::DontRequestClientCertificate;
        }
        addressConfig->Credentials = credentials;
    }

    auto config = New<NRpc::NGrpc::TServerConfig>();
    config->Addresses.push_back(addressConfig);
    return config;
}

//! Всё, что должно жить до конца процесса.
struct TFlightServiceState
{
    IPollerPtr Poller;
    IThreadPoolPtr WorkerPool;
    TPeriodicExecutorPtr ReservationSweeper;
    NRpc::IServerPtr Server;
};

TError Run(TFlightServiceState* state)
{
    SOA_ASSIGN_OR_RETURN(auto pollerThreads, GetEnvValue("POLLER_THREADS", 2));
    SOA_ASSIGN_OR_RETURN(auto workerThreads, GetEnvValue("WORKER_THREADS", 4));
    state->Poller = CreateThreadPoolPoller(pollerThreads, "Poller");
    state->WorkerPool = CreateThreadPool(workerThreads, "Worker");
    const auto& poller = state->Poller;

    SOA_ASSIGN_OR_RETURN(auto apiKey, GetRequiredEnvString("FLIGHT_SERVICE_API_KEY"));
    SOA_ASSIGN_OR_RETURN(auto pgConfig, NPg::LoadPgConfigFromEnv());
    SOA_ASSIGN_OR_RETURN(auto redisConfig, LoadRedisConfig());
    SOA_ASSIGN_OR_RETURN(auto cacheTtl, GetEnvValue("CACHE_TTL_MS", TDuration::Minutes(5)));
    SOA_ASSIGN_OR_RETURN(auto dbWaitSeconds, GetEnvValue("DB_WAIT_SECONDS", 60));
    SOA_ASSIGN_OR_RETURN(auto reservationTtl, GetEnvValue("RESERVATION_TTL_MS", TDuration::Minutes(10)));
    SOA_ASSIGN_OR_RETURN(auto sweepPeriod, GetEnvValue("RESERVATION_SWEEP_PERIOD_MS", TDuration::Seconds(5)));

    SOA_ASSIGN_OR_RETURN(auto pgClient, NPg::TPgClient::Create(std::move(pgConfig), poller));
    SOA_RETURN_IF_ERROR(pgClient->WaitUntilAvailable(TDuration::Seconds(dbWaitSeconds)));
    auto repository = CreatePgFlightRepository(pgClient, reservationTtl);
    if (redisConfig) {
        SOA_ASSIGN_OR_RETURN(auto redisClient, NRedis::TRedisClient::Create(std::move(*redisConfig), poller));
        repository = CreateFlightRepositoryWithRedis(std::move(repository), std::move(redisClient), cacheTtl);
    }

    auto serverConfig = LoadGrpcServerConfig();
    // Создание сервера и чтение TLS-сертификатов сообщают об ошибках исключениями.
    SOA_ASSIGN_OR_RETURN(state->Server, CatchErrors([&] {
        auto server = NRpc::NGrpc::CreateServer(serverConfig);
        server->RegisterService(CreateFlightService(
            state->WorkerPool->GetInvoker(),
            repository,
            CreateApiKeyAuthenticator(apiKey)));
        server->Start();
        return server;
    }));

    state->ReservationSweeper = StartReservationSweeper(repository, state->WorkerPool->GetInvoker(), sweepPeriod);

    const auto& credentials = serverConfig->Addresses[0]->Credentials;
    YT_LOG_INFO("Flight Service started (Address: %v, Tls: %v, MutualTls: %v, CacheEnabled: %v, CacheTtl: %v, ReservationTtl: %v)",
        serverConfig->Addresses[0]->Address,
        static_cast<bool>(credentials),
        credentials && credentials->PemRootCerts,
        redisConfig.has_value(),
        cacheTtl,
        reservationTtl);
    return {};
}

//! Graceful остановка: сервер перестаёт принимать запросы и ждёт текущие.
TError Stop(TFlightServiceState* state)
{
    if (state->ReservationSweeper) {
        SOA_RETURN_IF_ERROR(WaitFor(state->ReservationSweeper->Stop()));
    }
    if (!state->Server) {
        return {};
    }
    SOA_ASSIGN_OR_RETURN(auto timeout, GetEnvValue("STOP_TIMEOUT_MS", TDuration::Seconds(8)));
    YT_LOG_INFO("Stopping gRPC server (Timeout: %v)", timeout);
    SOA_RETURN_IF_ERROR_WRAP(
        WaitFor(state->Server->Stop(/*graceful*/ true).WithTimeout(timeout)),
        "Failed to stop gRPC server gracefully");
    YT_LOG_INFO("gRPC server stopped");
    return {};
}

} // namespace

////////////////////////////////////////////////////////////////////////////////

int main()
{
    TFlightServiceState state;
    return RunService(
        [&] { return Run(&state); },
        [&] { return Stop(&state); });
}
