// Booking Service: REST API бронирований (PostgreSQL), рейсы - через Flight Service по gRPC.

#include "booking_repository.h"
#include "booking_service.h"
#include "flight_client.h"
#include "http_api.h"

#include <soa/lib/common/bootstrap.h>
#include <soa/lib/common/env.h>
#include <soa/lib/common/error_macros.h>
#include <soa/lib/pg/env_config.h>

#include <yt/yt/core/concurrency/scheduler_api.h>
#include <yt/yt/core/concurrency/thread_pool.h>
#include <yt/yt/core/concurrency/thread_pool_poller.h>

#include <yt/yt/core/http/config.h>
#include <yt/yt/core/http/server.h>

#include <yt/yt/core/logging/log.h>

using namespace NYT;
using namespace NYT::NConcurrency;
using namespace NSoa;
using namespace NSoa::NBooking;

////////////////////////////////////////////////////////////////////////////////

YT_DEFINE_LEAKY_GLOBAL(const NLogging::TLogger, MainLogger, "Main");
constinit const auto Logger = MainLogger;

////////////////////////////////////////////////////////////////////////////////

namespace {

TErrorOr<TFlightClientConfig> LoadFlightClientConfig()
{
    TFlightClientConfig config;
    config.Address = GetEnvString("FLIGHT_SERVICE_ADDRESS", "localhost:50051");
    SOA_ASSIGN_OR_RETURN(config.ApiKey, GetRequiredEnvString("FLIGHT_SERVICE_API_KEY"));
    config.TlsCaCertPath = GetEnvString("FLIGHT_SERVICE_TLS_CA");
    config.TlsCertPath = GetEnvString("FLIGHT_SERVICE_TLS_CERT");
    config.TlsKeyPath = GetEnvString("FLIGHT_SERVICE_TLS_KEY");
    if (config.TlsCertPath.empty() != config.TlsKeyPath.empty()) {
        return TError("FLIGHT_SERVICE_TLS_CERT and FLIGHT_SERVICE_TLS_KEY must be set together");
    }
    SOA_ASSIGN_OR_RETURN(config.RequestTimeout, GetEnvValue("FLIGHT_RPC_TIMEOUT_MS", TDuration::Seconds(2)));

    SOA_ASSIGN_OR_RETURN(config.Retry.MaxAttempts, GetEnvValue("FLIGHT_RETRY_MAX_ATTEMPTS", 3));
    SOA_ASSIGN_OR_RETURN(config.Retry.InitialBackoff, GetEnvValue("FLIGHT_RETRY_INITIAL_BACKOFF_MS", TDuration::MilliSeconds(100)));
    SOA_ASSIGN_OR_RETURN(config.Retry.MaxBackoff, GetEnvValue("FLIGHT_RETRY_MAX_BACKOFF_MS", TDuration::Seconds(2)));

    SOA_ASSIGN_OR_RETURN(config.CircuitBreaker.FailureThreshold, GetEnvValue("CB_FAILURE_THRESHOLD", 5));
    SOA_ASSIGN_OR_RETURN(config.CircuitBreaker.FailureWindow, GetEnvValue("CB_FAILURE_WINDOW_MS", TDuration::Seconds(10)));
    SOA_ASSIGN_OR_RETURN(config.CircuitBreaker.OpenTimeout, GetEnvValue("CB_OPEN_TIMEOUT_MS", TDuration::Seconds(15)));
    if (config.CircuitBreaker.FailureThreshold < 1) {
        return TError("CB_FAILURE_THRESHOLD must be positive");
    }
    return config;
}

//! Всё, что должно жить до конца процесса.
struct TBookingServiceState
{
    IPollerPtr Poller;
    IThreadPoolPtr WorkerPool;
    std::unique_ptr<TBookingRepository> Repository;
    std::unique_ptr<TBookingService> Service;
    NHttp::IServerPtr HttpServer;
};

TError Run(TBookingServiceState* state)
{
    SOA_ASSIGN_OR_RETURN(auto pollerThreads, GetEnvValue("POLLER_THREADS", 2));
    SOA_ASSIGN_OR_RETURN(auto workerThreads, GetEnvValue("WORKER_THREADS", 4));
    SOA_ASSIGN_OR_RETURN(auto httpPort, GetEnvValue("HTTP_PORT", 8080));
    SOA_ASSIGN_OR_RETURN(auto dbWaitSeconds, GetEnvValue("DB_WAIT_SECONDS", 60));
    SOA_ASSIGN_OR_RETURN(auto pgConfig, NPg::LoadPgConfigFromEnv());
    SOA_ASSIGN_OR_RETURN(auto flightClientConfig, LoadFlightClientConfig());
    // Пустой ADMIN_TOKEN - административные ручки выключены.
    auto adminToken = GetEnvString("ADMIN_TOKEN");

    state->Poller = CreateThreadPoolPoller(pollerThreads, "Poller");
    state->WorkerPool = CreateThreadPool(workerThreads, "Worker");

    SOA_ASSIGN_OR_RETURN(auto pgClient, NPg::TPgClient::Create(std::move(pgConfig), state->Poller));
    SOA_RETURN_IF_ERROR(pgClient->WaitUntilAvailable(TDuration::Seconds(dbWaitSeconds)));
    state->Repository = std::make_unique<TBookingRepository>(pgClient);

    SOA_ASSIGN_OR_RETURN(auto flightClient, TFlightClient::Create(flightClientConfig));
    state->Service = std::make_unique<TBookingService>(flightClient, state->Repository.get());

    // Создание сервера и bind порта сообщают об ошибках исключениями.
    SOA_ASSIGN_OR_RETURN(state->HttpServer, CatchErrors([&] {
        auto serverConfig = New<NHttp::TServerConfig>();
        serverConfig->Port = httpPort;
        // Обработчики выполняются fiber'ами в пуле Worker: в них можно WaitFor.
        auto server = NHttp::CreateServer(serverConfig, state->Poller, state->WorkerPool->GetInvoker());
        RegisterHttpHandlers(server, state->Service.get(), flightClient, adminToken);
        server->Start();
        return server;
    }));

    YT_LOG_INFO("Booking Service started (HttpPort: %v, FlightService: %v, FlightServiceTls: %v, ClientCertificate: %v, AdminApi: %v)",
        httpPort,
        flightClientConfig.Address,
        !flightClientConfig.TlsCaCertPath.empty(),
        !flightClientConfig.TlsCertPath.empty(),
        !adminToken.empty());
    return {};
}

TError Stop(TBookingServiceState* state)
{
    if (state->HttpServer) {
        YT_LOG_INFO("Stopping HTTP server");
        // Stop() перестаёт принимать соединения; текущие запросы дорабатывают в пуле.
        SOA_RETURN_IF_ERROR(CatchErrors([&] {
            state->HttpServer->Stop();
        }));
    }
    return {};
}

} // namespace

////////////////////////////////////////////////////////////////////////////////

int main()
{
    TBookingServiceState state;
    return RunService(
        [&] { return Run(&state); },
        [&] { return Stop(&state); });
}
