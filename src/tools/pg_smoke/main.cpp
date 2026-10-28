// Ручная проверка клиента Postgres: подключение (TLS/SCRAM), запросы, транзакции, ошибки.
#include <soa/lib/common/bootstrap.h>
#include <soa/lib/common/env.h>
#include <soa/lib/common/error_macros.h>
#include <soa/lib/pg/client.h>

#include <yt/yt/core/concurrency/thread_pool_poller.h>

#include <iostream>

using namespace NYT;
using namespace NYT::NConcurrency;
using namespace NSoa;
using namespace NSoa::NPg;

namespace {

//! Проверка ожидаемой ошибки: возвращает ошибку, если операция неожиданно удалась.
TError ExpectSqlState(const TError& error, TStringBuf expected)
{
    if (error.IsOK()) {
        return TError("Expected SQLSTATE %v, got success", expected);
    }
    auto sqlState = GetSqlState(error);
    std::cerr << "expected error, sqlstate=" << sqlState << "\n";
    if (sqlState != expected) {
        return TError("Expected SQLSTATE %v, got %v", expected, sqlState).With(error);
    }
    return {};
}

TError Run(const IPollerPtr& poller)
{
    TPgConfig config;
    config.Host = GetEnvString("PG_HOST", "localhost");
    SOA_ASSIGN_OR_RETURN(config.Port, GetEnvValue("PG_PORT", 5432));
    config.User = GetEnvString("PG_USER", "postgres");
    config.Password = GetEnvString("PG_PASSWORD", "postgres");
    config.Database = GetEnvString("PG_DATABASE", "postgres");
    SOA_ASSIGN_OR_RETURN(config.SslMode, GetEnvValue("PG_SSLMODE", EPgSslMode::Disable));
    config.SslRootCertPath = GetEnvString("PG_SSLROOTCERT");

    SOA_ASSIGN_OR_RETURN(auto client, TPgClient::Create(config, poller));
    SOA_RETURN_IF_ERROR(client->WaitUntilAvailable(TDuration::Seconds(10)));

    SOA_ASSIGN_OR_RETURN(auto r, client->Execute(
        "SELECT $1::int + 1 AS x, $2::text AS s, NULL::text AS n, "
        "(SELECT ssl FROM pg_stat_ssl WHERE pid = pg_backend_pid()) AS ssl",
        {PgParam(41), PgParam("hello")}));
    auto row = r.Row(0);
    auto x = row.Get<int>("x");
    auto s = row.Get<std::string>("s");
    auto n = row.GetOptional<std::string>("n");
    auto ssl = row.Get<bool>("ssl");
    SOA_RETURN_IF_ERROR(row.GetError());
    std::cerr << "x=" << x << " s=" << s << " n=" << (n ? "?" : "NULL") << " ssl=" << ssl << "\n";

    SOA_RETURN_IF_ERROR(client->Execute("DROP TABLE IF EXISTS smoke"));
    SOA_RETURN_IF_ERROR(client->Execute("CREATE TABLE smoke (id int primary key, v int CHECK (v >= 0))"));
    SOA_ASSIGN_OR_RETURN(auto inserted, client->Execute("INSERT INTO smoke VALUES (1, 10), (2, 20)"));
    std::cerr << "inserted=" << inserted.GetAffectedRows() << "\n";

    SOA_RETURN_IF_ERROR(ExpectSqlState(client->Execute("INSERT INTO smoke VALUES (1, 5)"), UniqueViolation));

    auto txError = client->WithTransaction([] (TPgConnection& connection) -> TError {
        SOA_RETURN_IF_ERROR(connection.Execute("UPDATE smoke SET v = v - 5 WHERE id = 1"));
        // Нарушение CHECK -> вся транзакция откатывается.
        SOA_RETURN_IF_ERROR(connection.Execute("UPDATE smoke SET v = v - 100 WHERE id = 2"));
        return {};
    });
    SOA_RETURN_IF_ERROR(ExpectSqlState(txError, CheckViolation));

    SOA_ASSIGN_OR_RETURN(auto sumResult, client->Execute("SELECT sum(v)::bigint AS s FROM smoke"));
    SOA_ASSIGN_OR_RETURN(auto sum, sumResult.Get<i64>(0, "s"));
    std::cerr << "sum after rollback (expect 30)=" << sum << "\n";

    SOA_ASSIGN_OR_RETURN(auto before, client->WithTransaction([] (TPgConnection& connection) -> TErrorOr<int> {
        SOA_ASSIGN_OR_RETURN(auto locked, connection.Execute("SELECT v FROM smoke WHERE id = $1 FOR UPDATE", {PgParam(1)}));
        SOA_RETURN_IF_ERROR(connection.Execute("UPDATE smoke SET v = v - 1 WHERE id = $1", {PgParam(1)}));
        return locked.Get<int>(0, "v");
    }));
    std::cerr << "tx returned " << before << "\n";

    // Параллельные запросы: пул на 8 соединений, 32 fiber'а.
    std::vector<TErrorOr<int>> results(32);
    std::vector<TFuture<void>> futures;
    for (int i = 0; i < std::ssize(results); ++i) {
        futures.push_back(BIND([client, i, result = &results[i]] {
            *result = [&] () -> TErrorOr<int> {
                SOA_ASSIGN_OR_RETURN(auto rows, client->Execute("SELECT pg_sleep(0.2), $1::int AS i", {PgParam(i)}));
                return rows.Get<int>(0, "i");
            }();
        }).AsyncVia(GetCurrentInvoker()).Run());
    }
    auto start = TInstant::Now();
    SOA_RETURN_IF_ERROR(WaitFor(AllSucceeded(futures)));
    for (const auto& result : results) {
        SOA_RETURN_IF_ERROR(result);
    }
    std::cerr << "32 parallel queries in " << (TInstant::Now() - start).MilliSeconds() << "ms (expect ~800ms)\n";

    SOA_RETURN_IF_ERROR(client->Execute("DROP TABLE smoke"));
    std::cerr << "OK\n";
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
