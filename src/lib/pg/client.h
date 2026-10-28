#pragma once

#include <soa/lib/net/buffered_connection.h>
#include <soa/lib/net/connection_pool.h>

#include <yt/yt/core/concurrency/public.h>
#include <yt/yt/core/crypto/public.h>

#include <library/cpp/yt/error/error.h>
#include <library/cpp/yt/misc/enum.h>
#include <library/cpp/yt/string/format.h>

#include <optional>
#include <string>
#include <vector>

namespace NSoa::NPg {

////////////////////////////////////////////////////////////////////////////////

YT_DEFINE_ERROR_ENUM(
    //! Сервер вернул ErrorResponse; SQLSTATE лежит в атрибуте "sqlstate".
    ((ServerError)      (30000))
    //! Ошибка соединения/протокола; соединение больше не используется.
    ((ConnectionError)  (30001))
);

//! Некоторые коды SQLSTATE.
inline constexpr TStringBuf UniqueViolation = "23505";
inline constexpr TStringBuf CheckViolation = "23514";
inline constexpr TStringBuf ForeignKeyViolation = "23503";
inline constexpr TStringBuf SerializationFailure = "40001";
inline constexpr TStringBuf DeadlockDetected = "40P01";

//! Возвращает SQLSTATE ошибки Postgres или пустую строку.
std::string GetSqlState(const NYT::TError& error);

////////////////////////////////////////////////////////////////////////////////

//! Аналог sslmode из libpq.
DEFINE_ENUM(EPgSslMode,
    //! Без TLS.
    (Disable)
    //! TLS обязателен, сертификат сервера не проверяется.
    (Require)
    //! TLS обязателен, сертификат проверяется по CA и имени хоста.
    (VerifyFull)
);

struct TPgConfig
{
    std::string Host = "localhost";
    int Port = 5432;
    std::string User;
    std::string Password;
    std::string Database;

    EPgSslMode SslMode = EPgSslMode::Disable;
    //! PEM-файл с CA для VerifyFull.
    std::string SslRootCertPath;

    int PoolSize = 8;
    TDuration ConnectTimeout = TDuration::Seconds(5);
    TDuration IOTimeout = TDuration::Seconds(10);
};

////////////////////////////////////////////////////////////////////////////////

//! Параметр запроса в текстовом формате; std::nullopt - SQL NULL.
using TPgParam = std::optional<std::string>;
using TPgParams = std::vector<TPgParam>;

template <class T>
TPgParam PgParam(const T& value)
{
    return std::string(NYT::ToString(value));
}

inline TPgParam PgParam(std::string value)
{
    return value;
}

inline TPgParam PgParam(const char* value)
{
    return std::string(value);
}

template <class T>
TPgParam PgParam(const std::optional<T>& value)
{
    return value ? PgParam(*value) : std::nullopt;
}

////////////////////////////////////////////////////////////////////////////////

class TPgResult;

//! Чтение колонок одной строки результата с "липкой" ошибкой
//! (как bufio.Scanner в Go): при первой ошибке (нет колонки, NULL, не парсится)
//! она запоминается, а Get возвращает значение по умолчанию. После чтения всех
//! полей достаточно один раз проверить GetError().
class TPgRowReader
{
public:
    TPgRowReader(const TPgResult* result, int row);

    //! Значение колонки; NULL - ошибка.
    template <class T>
    T Get(TStringBuf column);

    template <class T>
    std::optional<T> GetOptional(TStringBuf column);

    const NYT::TError& GetError() const;

private:
    const TPgResult* const Result_;
    const int Row_;
    NYT::TError Error_;

    const std::optional<std::string>* FindRaw(TStringBuf column);
};

//! Результат выполнения запроса (все значения в текстовом формате).
class TPgResult
{
public:
    int GetRowCount() const;
    std::optional<int> FindColumnIndex(TStringBuf name) const;
    const std::optional<std::string>& GetRaw(int row, int column) const;

    TPgRowReader Row(int row) const;

    //! Одиночное значение (для запросов вида SELECT x ...).
    template <class T>
    NYT::TErrorOr<T> Get(int row, TStringBuf column) const;

    //! Число строк, затронутых INSERT/UPDATE/DELETE (из CommandComplete).
    i64 GetAffectedRows() const;

private:
    friend class TPgConnection;

    std::vector<std::string> Columns_;
    std::vector<std::vector<std::optional<std::string>>> Rows_;
    std::string CommandTag_;
};

////////////////////////////////////////////////////////////////////////////////

DECLARE_REFCOUNTED_CLASS(TPgConnection)

//! Одно соединение с Postgres поверх YT-шного poller'а (wire protocol v3).
/*!
 *  Методы вызываются только из fiber'а: все ожидания сети идут через WaitFor.
 *  Поддерживаемая аутентификация: trust, cleartext, md5, SCRAM-SHA-256.
 *
 *  Ошибка сервера (ErrorResponse, код ServerError) оставляет соединение
 *  пригодным; ошибка протокола/сети или отмена fiber'а посреди запроса - нет.
 */
class TPgConnection
    : public NYT::TRefCounted
{
public:
    //! #sslContext обязателен, если config.SslMode != Disable.
    static NYT::TErrorOr<TPgConnectionPtr> Connect(
        const TPgConfig& config,
        const NYT::NConcurrency::IPollerPtr& poller,
        const NYT::NCrypto::TSslContextPtr& sslContext);

    //! Выполняет запрос через extended query protocol (Parse/Bind/Describe/Execute/Sync).
    NYT::TErrorOr<TPgResult> Execute(TStringBuf sql, const TPgParams& params = {});

    //! Соединение можно вернуть в пул: оно живо и не находится внутри транзакции.
    bool IsReusable() const;

    //! Статус транзакции из последнего ReadyForQuery: 'I', 'T' или 'E'.
    char GetTransactionStatus() const;

    //! Разрывает соединение.
    void Abort();

private:
    const NSoa::NNet::TBufferedConnectionPtr Connection_;
    char TransactionStatus_ = 'I';
    bool Broken_ = false;

    explicit TPgConnection(NSoa::NNet::TBufferedConnectionPtr connection);
    DECLARE_NEW_FRIEND()

    struct TMessage
    {
        char Type;
        std::string Body;
    };

    NYT::TErrorOr<TMessage> ReadMessage();
    NYT::TError NegotiateTls(
        const TPgConfig& config,
        const NYT::NConcurrency::IPollerPtr& poller,
        const NYT::NCrypto::TSslContextPtr& sslContext);
    NYT::TError Startup(const TPgConfig& config);
    NYT::TError AuthenticateScram(const TPgConfig& config, TStringBuf mechanisms);
    NYT::TError SendPassword(std::string payload);
    NYT::TError WaitReadyForQuery();
    NYT::TErrorOr<TPgResult> DoExecute(TStringBuf sql, const TPgParams& params);
};

DEFINE_REFCOUNTED_TYPE(TPgConnection)

////////////////////////////////////////////////////////////////////////////////

DECLARE_REFCOUNTED_CLASS(TPgClient)

//! Пул соединений + удобные обёртки для запросов и транзакций.
class TPgClient
    : public NYT::TRefCounted
{
public:
    using TPool = NSoa::NNet::TConnectionPool<TPgConnection>;
    using TConnectionGuard = TPool::TGuard;

    static NYT::TErrorOr<TPgClientPtr> Create(TPgConfig config, NYT::NConcurrency::IPollerPtr poller);

    NYT::TErrorOr<TConnectionGuard> Acquire();

    //! Выполняет одиночный запрос на любом свободном соединении (autocommit).
    NYT::TErrorOr<TPgResult> Execute(TStringBuf sql, const TPgParams& params = {});

    //! Выполняет #callback внутри BEGIN/COMMIT; если callback вернул ошибку - ROLLBACK.
    /*!
     *  #callback: (TPgConnection&) -> TError или TErrorOr<T>; результат
     *  WithTransaction - того же типа.
     */
    template <class TCallback>
    auto WithTransaction(TCallback&& callback) -> std::invoke_result_t<TCallback, TPgConnection&>;

    //! Ждёт, пока база станет доступна (при старте сервиса в docker-compose).
    NYT::TError WaitUntilAvailable(TDuration timeout);

private:
    const TPgConfig Config_;
    const NYT::NConcurrency::IPollerPtr Poller_;
    const NYT::NCrypto::TSslContextPtr SslContext_;
    const NYT::TIntrusivePtr<TPool> Pool_;

    TPgClient(TPgConfig config, NYT::NConcurrency::IPollerPtr poller, NYT::NCrypto::TSslContextPtr sslContext);
    DECLARE_NEW_FRIEND()
};

DEFINE_REFCOUNTED_TYPE(TPgClient)

////////////////////////////////////////////////////////////////////////////////

} // namespace NSoa::NPg

#define CLIENT_INL_H_
#include "client-inl.h"
#undef CLIENT_INL_H_
