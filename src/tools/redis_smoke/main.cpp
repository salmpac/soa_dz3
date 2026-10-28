// Ручная проверка клиента Redis: TLS, Sentinel, GET/SET/DEL, работа при failover.
#include <soa/lib/common/bootstrap.h>
#include <soa/lib/common/env.h>
#include <soa/lib/common/error_macros.h>
#include <soa/lib/redis/client.h>

#include <yt/yt/core/concurrency/delayed_executor.h>
#include <yt/yt/core/concurrency/thread_pool_poller.h>

#include <util/string/split.h>

#include <iostream>

using namespace NYT;
using namespace NYT::NConcurrency;
using namespace NSoa;
using namespace NSoa::NRedis;

namespace {

TError Run(const IPollerPtr& poller)
{
    TRedisConfig config;
    for (const auto& part : StringSplitter(GetEnvString("REDIS_SENTINELS")).Split(',').SkipEmpty()) {
        SOA_ASSIGN_OR_RETURN(auto sentinel, ParseEndpoint(part.Token(), 26379));
        config.Sentinels.push_back(std::move(sentinel));
    }
    SOA_ASSIGN_OR_RETURN(config.StandaloneEndpoint, ParseEndpoint(GetEnvString("REDIS_ADDRESS", "localhost:6379"), 6379));
    config.Password = GetEnvString("REDIS_PASSWORD");
    SOA_ASSIGN_OR_RETURN(config.TlsMode, GetEnvValue("REDIS_TLS_MODE", ERedisTlsMode::Disable));
    config.TlsCaCertPath = GetEnvString("REDIS_TLS_CA");
    SOA_ASSIGN_OR_RETURN(auto loopSeconds, GetEnvValue("LOOP_SECONDS", 0));

    SOA_ASSIGN_OR_RETURN(auto client, TRedisClient::Create(config, poller));
    SOA_RETURN_IF_ERROR(client->SetEx("smoke:key", "value with \r\n inside", TDuration::Seconds(10)));
    SOA_ASSIGN_OR_RETURN(auto value, client->Get("smoke:key"));
    SOA_ASSIGN_OR_RETURN(auto ttl, client->Command({"PTTL", "smoke:key"}));
    std::cerr << "get=" << (value ? *value : "<nil>") << " ttl=" << ttl.Integer << "\n";
    SOA_ASSIGN_OR_RETURN(auto missing, client->Get("smoke:missing"));
    std::cerr << "missing=" << (missing ? "?" : "<nil>") << "\n";
    SOA_ASSIGN_OR_RETURN(auto deleted, client->Del({"smoke:key", "smoke:missing"}));
    std::cerr << "del=" << deleted << "\n";

    auto unknown = client->Command({"NOSUCHCOMMAND"});
    if (unknown.IsOK()) {
        return TError("Expected error for unknown command");
    }
    std::cerr << "expected error: " << unknown.GetMessage() << "\n";

    // Для проверки failover: пишем раз в 200 мс и печатаем ошибки.
    for (int i = 0; i < loopSeconds * 5; ++i) {
        if (auto error = client->SetEx("smoke:loop", std::to_string(i), TDuration::Seconds(10)); error.IsOK()) {
            std::cerr << "." << std::flush;
        } else {
            std::cerr << "\nwrite failed: " << error.GetMessage() << "\n";
        }
        TDelayedExecutor::WaitForDuration(TDuration::MilliSeconds(200));
    }
    std::cerr << "\nOK\n";
    return {};
}

} // namespace

int main()
{
    auto poller = CreateThreadPoolPoller(2, "Poller");
    return RunProgram([&] {
        return Run(poller);
    });
}
