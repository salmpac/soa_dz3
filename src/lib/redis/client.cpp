#include "client.h"

#include <soa/lib/common/error_macros.h>

#include <yt/yt/core/concurrency/scheduler_api.h>

#include <yt/yt/core/logging/log.h>

#include <util/string/cast.h>
#include <util/system/guard.h>

namespace NSoa::NRedis {

using namespace NYT;
using namespace NYT::NConcurrency;
using namespace NYT::NCrypto;

////////////////////////////////////////////////////////////////////////////////

YT_DEFINE_LEAKY_GLOBAL(const NLogging::TLogger, RedisLogger, "Redis");
constinit const auto Logger = RedisLogger;

static constexpr i64 MaxBulkLength = 512 * 1024 * 1024;
static constexpr int MaxReplyDepth = 16;

////////////////////////////////////////////////////////////////////////////////

TErrorOr<TRedisEndpoint> ParseEndpoint(TStringBuf address, int defaultPort)
{
    TRedisEndpoint endpoint;
    auto colon = address.rfind(':');
    if (colon == TStringBuf::npos) {
        endpoint.Host = std::string(address);
        endpoint.Port = defaultPort;
    } else {
        endpoint.Host = std::string(address.SubStr(0, colon));
        if (!TryFromString(address.SubStr(colon + 1), endpoint.Port)) {
            return TError("Invalid Redis endpoint %Qv", address);
        }
    }
    if (endpoint.Host.empty()) {
        return TError("Invalid Redis endpoint %Qv", address);
    }
    return endpoint;
}

////////////////////////////////////////////////////////////////////////////////

bool TRedisReply::IsNil() const
{
    return Type == ERedisReplyType::Nil;
}

TError TRedisReply::ToError() const
{
    if (Type == ERedisReplyType::Error) {
        return TError(EErrorCode::ServerError, "Redis error: %v", String);
    }
    return {};
}

////////////////////////////////////////////////////////////////////////////////

TRedisConnection::TRedisConnection(NSoa::NNet::TBufferedConnectionPtr connection)
    : Connection_(std::move(connection))
{ }

TErrorOr<TRedisConnectionPtr> TRedisConnection::Connect(
    const TRedisEndpoint& endpoint,
    const std::string& password,
    ERedisTlsMode tlsMode,
    const TSslContextPtr& sslContext,
    const IPollerPtr& poller,
    TDuration connectTimeout,
    TDuration ioTimeout)
{
    auto connect = [&] () -> TErrorOr<TRedisConnectionPtr> {
        SOA_ASSIGN_OR_RETURN(auto bufferedConnection, NSoa::NNet::DialTcp(
            endpoint.Host,
            endpoint.Port,
            poller,
            connectTimeout,
            ioTimeout));

        if (tlsMode != ERedisTlsMode::Disable) {
            // В Redis TLS начинается сразу, без согласования открытым текстом.
            std::optional<std::string> host;
            if (tlsMode == ERedisTlsMode::VerifyFull) {
                host = endpoint.Host;
            }
            SOA_RETURN_IF_ERROR(bufferedConnection->StartTls(sslContext, poller, host));
        }

        auto connection = New<TRedisConnection>(std::move(bufferedConnection));
        if (!password.empty()) {
            SOA_ASSIGN_OR_RETURN(auto reply, connection->Command({"AUTH", password}));
            SOA_RETURN_IF_ERROR(reply.ToError());
        }
        return connection;
    };

    auto connectionOrError = connect();
    if (!connectionOrError.IsOK()) {
        return connectionOrError.Wrap(
            EErrorCode::ConnectionError,
            "Failed to connect to Redis at %v:%v",
            endpoint.Host,
            endpoint.Port);
    }
    return connectionOrError;
}

TErrorOr<TRedisReply> TRedisConnection::Command(const std::vector<std::string>& args)
{
    if (Broken_) {
        return TError(EErrorCode::ConnectionError, "Connection is broken");
    }

    // Команда - массив bulk string'ов: *N\r\n$len\r\narg\r\n...
    std::string request;
    request += "*" + std::to_string(args.size()) + "\r\n";
    for (const auto& arg : args) {
        request += "$" + std::to_string(arg.size()) + "\r\n";
        request += arg;
        request += "\r\n";
    }

    // Пессимистично: если fiber отменят посреди ответа, дальше по соединению читать нельзя.
    Broken_ = true;
    auto reply = [&] () -> TErrorOr<TRedisReply> {
        SOA_RETURN_IF_ERROR(Connection_->Write(std::move(request)));
        return ReadReply();
    }();
    if (!reply.IsOK()) {
        Abort();
        return reply.Wrap(EErrorCode::ConnectionError, "Redis connection error");
    }
    Broken_ = false;
    return reply;
}

TErrorOr<TRedisReply> TRedisConnection::ReadReply(int depth)
{
    if (depth > MaxReplyDepth) {
        return TError("Redis reply is nested too deeply");
    }

    SOA_ASSIGN_OR_RETURN(auto line, Connection_->ReadLine());
    if (line.empty()) {
        return TError("Empty Redis reply line");
    }

    TRedisReply reply;
    auto payload = TStringBuf(line).SubStr(1);
    auto parseInteger = [&] () -> TErrorOr<i64> {
        i64 value;
        if (!TryFromString(payload, value)) {
            return TError("Malformed Redis integer %Qv", payload);
        }
        return value;
    };

    switch (line[0]) {
        case '+':
            reply.Type = ERedisReplyType::SimpleString;
            reply.String = std::string(payload);
            break;

        case '-':
            reply.Type = ERedisReplyType::Error;
            reply.String = std::string(payload);
            break;

        case ':': {
            reply.Type = ERedisReplyType::Integer;
            SOA_ASSIGN_OR_RETURN(reply.Integer, parseInteger());
            break;
        }

        case '$': {
            SOA_ASSIGN_OR_RETURN(auto length, parseInteger());
            if (length < 0) {
                reply.Type = ERedisReplyType::Nil;
                break;
            }
            if (length > MaxBulkLength) {
                return TError("Redis bulk string is too long: %v", length);
            }
            reply.Type = ERedisReplyType::BulkString;
            SOA_ASSIGN_OR_RETURN(reply.String, Connection_->ReadExact(length));
            SOA_ASSIGN_OR_RETURN(auto terminator, Connection_->ReadLine());
            if (!terminator.empty()) {
                return TError("Malformed Redis bulk string");
            }
            break;
        }

        case '*': {
            SOA_ASSIGN_OR_RETURN(auto count, parseInteger());
            if (count < 0) {
                reply.Type = ERedisReplyType::Nil;
                break;
            }
            reply.Type = ERedisReplyType::Array;
            reply.Elements.reserve(std::min<i64>(count, 1024));
            for (i64 index = 0; index < count; ++index) {
                SOA_ASSIGN_OR_RETURN(auto element, ReadReply(depth + 1));
                reply.Elements.push_back(std::move(element));
            }
            break;
        }

        default:
            return TError("Unknown Redis reply type %Qv", line[0]);
    }
    return reply;
}

bool TRedisConnection::IsReusable() const
{
    return !Broken_ && !Connection_->IsBroken();
}

void TRedisConnection::Abort()
{
    Broken_ = true;
    Connection_->Abort();
}

////////////////////////////////////////////////////////////////////////////////

namespace {

//! Ошибки, после которых текущий "мастер" больше не мастер (failover).
bool IsFailoverError(TStringBuf message)
{
    return message.StartsWith("READONLY") ||
        message.StartsWith("LOADING") ||
        message.StartsWith("MASTERDOWN");
}

} // namespace

TErrorOr<TRedisClientPtr> TRedisClient::Create(TRedisConfig config, IPollerPtr poller)
{
    TSslContextPtr sslContext;
    if (config.TlsMode != ERedisTlsMode::Disable) {
        SOA_ASSIGN_OR_RETURN(sslContext, NSoa::NNet::CreateClientSslContext(
            config.TlsMode == ERedisTlsMode::VerifyFull,
            config.TlsCaCertPath));
    }
    return New<TRedisClient>(std::move(config), std::move(poller), std::move(sslContext));
}

TRedisClient::TRedisClient(TRedisConfig config, IPollerPtr poller, TSslContextPtr sslContext)
    : Config_(std::move(config))
    , Poller_(std::move(poller))
    , SslContext_(std::move(sslContext))
    , Pool_(New<TPool>(Config_.PoolSize, [this] {
        return ConnectToMaster();
    }))
{ }

TErrorOr<TRedisConnectionPtr> TRedisClient::Connect(const TRedisEndpoint& endpoint, const std::string& password)
{
    return TRedisConnection::Connect(
        endpoint,
        password,
        Config_.TlsMode,
        SslContext_,
        Poller_,
        Config_.ConnectTimeout,
        Config_.IOTimeout);
}

TErrorOr<TRedisConnectionPtr> TRedisClient::ConnectToMaster()
{
    std::optional<TRedisEndpoint> master;
    {
        auto guard = Guard(MasterLock_);
        master = Master_;
    }

    if (!master) {
        SOA_ASSIGN_OR_RETURN(master, DiscoverMaster());
        auto guard = Guard(MasterLock_);
        Master_ = master;
    }

    auto connectionOrError = Connect(*master, Config_.Password);
    if (!connectionOrError.IsOK()) {
        ResetMaster();
    }
    return connectionOrError;
}

TErrorOr<TRedisEndpoint> TRedisClient::DiscoverMasterVia(const TRedisEndpoint& sentinel)
{
    SOA_ASSIGN_OR_RETURN(auto sentinelConnection, Connect(sentinel, Config_.SentinelPassword));
    SOA_ASSIGN_OR_RETURN(auto reply, sentinelConnection->Command({"SENTINEL", "get-master-addr-by-name", Config_.MasterName}));
    SOA_RETURN_IF_ERROR(reply.ToError());
    if (reply.Type != ERedisReplyType::Array || reply.Elements.size() != 2) {
        return TError("Sentinel does not know master %Qv", Config_.MasterName);
    }

    TRedisEndpoint master{.Host = reply.Elements[0].String};
    if (!TryFromString(reply.Elements[1].String, master.Port)) {
        return TError("Sentinel returned invalid master port %Qv", reply.Elements[1].String);
    }

    // Sentinel мог ещё не заметить failover: убеждаемся, что узел - мастер.
    SOA_ASSIGN_OR_RETURN(auto masterConnection, Connect(master, Config_.Password));
    SOA_ASSIGN_OR_RETURN(auto role, masterConnection->Command({"ROLE"}));
    SOA_RETURN_IF_ERROR(role.ToError());
    if (role.Type != ERedisReplyType::Array || role.Elements.empty() || role.Elements[0].String != "master") {
        return TError("Node %v:%v reported by sentinel is not a master", master.Host, master.Port);
    }
    return master;
}

TErrorOr<TRedisEndpoint> TRedisClient::DiscoverMaster()
{
    if (Config_.Sentinels.empty()) {
        return Config_.StandaloneEndpoint;
    }

    std::vector<TError> errors;
    for (const auto& sentinel : Config_.Sentinels) {
        auto masterOrError = DiscoverMasterVia(sentinel);
        if (masterOrError.IsOK()) {
            const auto& master = masterOrError.Value();
            YT_LOG_INFO("Discovered Redis master via Sentinel (Sentinel: %v:%v, Master: %v:%v)",
                sentinel.Host,
                sentinel.Port,
                master.Host,
                master.Port);
            return masterOrError;
        }
        YT_LOG_WARNING(masterOrError, "Failed to discover Redis master via sentinel (Sentinel: %v:%v)",
            sentinel.Host,
            sentinel.Port);
        errors.push_back(masterOrError);
    }

    return TError(EErrorCode::ConnectionError, "Failed to discover Redis master %Qv via any sentinel", Config_.MasterName)
        .With(errors);
}

void TRedisClient::ResetMaster()
{
    std::optional<TRedisEndpoint> previous;
    {
        auto guard = Guard(MasterLock_);
        previous.swap(Master_);
    }
    if (!previous) {
        return;
    }
    YT_LOG_INFO("Resetting Redis master (Master: %v:%v)", previous->Host, previous->Port);
    Pool_->Reset();
}

TErrorOr<TRedisReply> TRedisClient::Command(const std::vector<std::string>& args)
{
    SOA_ASSIGN_OR_RETURN(auto guard, Pool_->Acquire());

    auto replyOrError = guard->Command(args);
    if (!replyOrError.IsOK()) {
        ResetMaster();
        return replyOrError;
    }

    const auto& reply = replyOrError.Value();
    if (reply.Type == ERedisReplyType::Error && IsFailoverError(reply.String)) {
        guard.Discard();
        ResetMaster();
    }
    SOA_RETURN_IF_ERROR(reply.ToError());
    return replyOrError;
}

TErrorOr<std::optional<std::string>> TRedisClient::Get(const std::string& key)
{
    SOA_ASSIGN_OR_RETURN(auto reply, Command({"GET", key}));
    if (reply.IsNil()) {
        return std::optional<std::string>();
    }
    return std::optional<std::string>(std::move(reply.String));
}

TError TRedisClient::SetEx(const std::string& key, const std::string& value, TDuration ttl)
{
    SOA_RETURN_IF_ERROR(Command({"SET", key, value, "PX", std::to_string(ttl.MilliSeconds())}));
    return {};
}

TErrorOr<i64> TRedisClient::Del(const std::vector<std::string>& keys)
{
    std::vector<std::string> args;
    args.reserve(keys.size() + 1);
    args.push_back("DEL");
    args.insert(args.end(), keys.begin(), keys.end());
    SOA_ASSIGN_OR_RETURN(auto reply, Command(args));
    return reply.Integer;
}

////////////////////////////////////////////////////////////////////////////////

} // namespace NSoa::NRedis
