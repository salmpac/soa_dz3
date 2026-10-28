#pragma once

#include <soa/lib/net/buffered_connection.h>
#include <soa/lib/net/connection_pool.h>

#include <yt/yt/core/concurrency/public.h>
#include <yt/yt/core/crypto/public.h>

#include <library/cpp/yt/error/error.h>
#include <library/cpp/yt/misc/enum.h>
#include <library/cpp/yt/system/spin_lock.h>

#include <optional>
#include <string>
#include <vector>

namespace NSoa::NRedis {

////////////////////////////////////////////////////////////////////////////////

YT_DEFINE_ERROR_ENUM(
    //! Сервер ответил ошибкой (-ERR ...).
    ((ServerError)      (30100))
    //! Ошибка соединения/протокола или недоступность мастера.
    ((ConnectionError)  (30101))
);

DEFINE_ENUM(ERedisTlsMode,
    (Disable)
    //! TLS без проверки сертификата.
    (Require)
    //! TLS с проверкой сертификата по CA и имени хоста.
    (VerifyFull)
);

struct TRedisEndpoint
{
    std::string Host;
    int Port = 6379;
};

//! Разбирает "host:port".
NYT::TErrorOr<TRedisEndpoint> ParseEndpoint(TStringBuf address, int defaultPort);

struct TRedisConfig
{
    //! Адреса sentinel'ов. Если пусто - используется StandaloneEndpoint.
    std::vector<TRedisEndpoint> Sentinels;
    std::string MasterName = "mymaster";
    //! Используется без Sentinel.
    TRedisEndpoint StandaloneEndpoint;

    std::string Password;
    //! Пароль для sentinel'ов (если включён requirepass на них).
    std::string SentinelPassword;

    ERedisTlsMode TlsMode = ERedisTlsMode::Disable;
    std::string TlsCaCertPath;

    int PoolSize = 8;
    TDuration ConnectTimeout = TDuration::Seconds(2);
    TDuration IOTimeout = TDuration::Seconds(2);
};

////////////////////////////////////////////////////////////////////////////////

DEFINE_ENUM(ERedisReplyType,
    (SimpleString)
    (Error)
    (Integer)
    (BulkString)
    (Nil)
    (Array)
);

struct TRedisReply
{
    ERedisReplyType Type = ERedisReplyType::Nil;
    std::string String;
    i64 Integer = 0;
    std::vector<TRedisReply> Elements;

    bool IsNil() const;
    //! Ошибка, если ответ сервера - ошибка (-ERR ...).
    NYT::TError ToError() const;
};

////////////////////////////////////////////////////////////////////////////////

DECLARE_REFCOUNTED_CLASS(TRedisConnection)

//! Соединение с одним узлом Redis (RESP2). Используется одним fiber'ом за раз.
class TRedisConnection
    : public NYT::TRefCounted
{
public:
    static NYT::TErrorOr<TRedisConnectionPtr> Connect(
        const TRedisEndpoint& endpoint,
        const std::string& password,
        ERedisTlsMode tlsMode,
        const NYT::NCrypto::TSslContextPtr& sslContext,
        const NYT::NConcurrency::IPollerPtr& poller,
        TDuration connectTimeout,
        TDuration ioTimeout);

    //! Ответ сервера; ответ-ошибка (-ERR) тоже возвращается как TRedisReply.
    NYT::TErrorOr<TRedisReply> Command(const std::vector<std::string>& args);

    bool IsReusable() const;
    void Abort();

private:
    const NSoa::NNet::TBufferedConnectionPtr Connection_;
    bool Broken_ = false;

    explicit TRedisConnection(NSoa::NNet::TBufferedConnectionPtr connection);
    DECLARE_NEW_FRIEND()

    NYT::TErrorOr<TRedisReply> ReadReply(int depth = 0);
};

DEFINE_REFCOUNTED_TYPE(TRedisConnection)

////////////////////////////////////////////////////////////////////////////////

DECLARE_REFCOUNTED_CLASS(TRedisClient)

//! Клиент к мастеру Redis с обнаружением мастера через Sentinel.
/*!
 *  Адрес мастера запрашивается у sentinel'ов (SENTINEL get-master-addr-by-name)
 *  и проверяется командой ROLE. При ошибке соединения или ответе
 *  READONLY/LOADING (после failover бывший мастер становится репликой)
 *  пул сбрасывается, и при следующем запросе мастер ищется заново.
 */
class TRedisClient
    : public NYT::TRefCounted
{
public:
    static NYT::TErrorOr<TRedisClientPtr> Create(TRedisConfig config, NYT::NConcurrency::IPollerPtr poller);

    //! Ответ-ошибка сервера превращается в TError (код ServerError).
    NYT::TErrorOr<TRedisReply> Command(const std::vector<std::string>& args);

    NYT::TErrorOr<std::optional<std::string>> Get(const std::string& key);
    NYT::TError SetEx(const std::string& key, const std::string& value, TDuration ttl);
    NYT::TErrorOr<i64> Del(const std::vector<std::string>& keys);

private:
    using TPool = NSoa::NNet::TConnectionPool<TRedisConnection>;

    const TRedisConfig Config_;
    const NYT::NConcurrency::IPollerPtr Poller_;
    const NYT::NCrypto::TSslContextPtr SslContext_;
    const NYT::TIntrusivePtr<TPool> Pool_;

    YT_DECLARE_SPIN_LOCK(NYT::TSpinLock, MasterLock_);
    std::optional<TRedisEndpoint> Master_;

    TRedisClient(TRedisConfig config, NYT::NConcurrency::IPollerPtr poller, NYT::NCrypto::TSslContextPtr sslContext);
    DECLARE_NEW_FRIEND()

    NYT::TErrorOr<TRedisConnectionPtr> ConnectToMaster();
    NYT::TErrorOr<TRedisEndpoint> DiscoverMaster();
    NYT::TErrorOr<TRedisEndpoint> DiscoverMasterVia(const TRedisEndpoint& sentinel);
    NYT::TErrorOr<TRedisConnectionPtr> Connect(const TRedisEndpoint& endpoint, const std::string& password);
    void ResetMaster();
};

DEFINE_REFCOUNTED_TYPE(TRedisClient)

////////////////////////////////////////////////////////////////////////////////

} // namespace NSoa::NRedis
