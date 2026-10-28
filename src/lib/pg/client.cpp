#include "client.h"

#include <soa/lib/common/error_macros.h>

#include <yt/yt/core/concurrency/delayed_executor.h>
#include <yt/yt/core/concurrency/scheduler_api.h>

#include <yt/yt/core/crypto/crypto.h>

#include <yt/yt/core/logging/log.h>

#include <library/cpp/string_utils/base64/base64.h>

#include <util/string/split.h>

namespace NSoa::NPg {

using namespace NYT;
using namespace NYT::NConcurrency;
using namespace NYT::NCrypto;

////////////////////////////////////////////////////////////////////////////////

YT_DEFINE_LEAKY_GLOBAL(const NLogging::TLogger, PgLogger, "Pg");
constinit const auto Logger = PgLogger;

static constexpr i32 ProtocolVersion3 = 196608;  // 3.0
static constexpr i32 SslRequestCode = 80877103;
static constexpr i32 MaxMessageSize = 1 << 30;

static constexpr int ScramNonceBytes = 18;

////////////////////////////////////////////////////////////////////////////////

namespace {

void AppendInt32(std::string* out, i32 value)
{
    auto unsignedValue = static_cast<ui32>(value);
    out->push_back(static_cast<char>((unsignedValue >> 24) & 0xff));
    out->push_back(static_cast<char>((unsignedValue >> 16) & 0xff));
    out->push_back(static_cast<char>((unsignedValue >> 8) & 0xff));
    out->push_back(static_cast<char>(unsignedValue & 0xff));
}

void AppendInt16(std::string* out, i16 value)
{
    auto unsignedValue = static_cast<ui16>(value);
    out->push_back(static_cast<char>((unsignedValue >> 8) & 0xff));
    out->push_back(static_cast<char>(unsignedValue & 0xff));
}

void AppendCString(std::string* out, TStringBuf value)
{
    out->append(value.data(), value.size());
    out->push_back('\0');
}

i32 ParseInt32(TStringBuf data)
{
    YT_VERIFY(data.size() >= 4);
    auto byte = [&] (int index) {
        return static_cast<ui32>(static_cast<unsigned char>(data[index]));
    };
    return static_cast<i32>((byte(0) << 24) | (byte(1) << 16) | (byte(2) << 8) | byte(3));
}

//! Сообщение фронтенда: тип + длина (включая саму длину) + тело.
std::string MakeMessage(char type, TStringBuf body)
{
    std::string message;
    message.reserve(body.size() + 5);
    message.push_back(type);
    AppendInt32(&message, static_cast<i32>(body.size() + 4));
    message.append(body.data(), body.size());
    return message;
}

TError MakeConnectionError(TError error)
{
    return error.Wrap(EErrorCode::ConnectionError, "Postgres protocol error");
}

//! Последовательное чтение полей из тела сообщения бэкенда с проверкой границ.
//! Ошибка "липкая": после выхода за границы все чтения возвращают пустые значения,
//! а GetError() сообщает о первой проблеме.
class TMessageParser
{
public:
    explicit TMessageParser(TStringBuf data)
        : Data_(data)
    { }

    i32 ReadInt32()
    {
        auto bytes = ReadBytes(4);
        return bytes.size() == 4 ? ParseInt32(bytes) : 0;
    }

    i16 ReadInt16()
    {
        auto bytes = ReadBytes(2);
        if (bytes.size() != 2) {
            return 0;
        }
        return static_cast<i16>(
            (static_cast<ui16>(static_cast<unsigned char>(bytes[0])) << 8) |
            static_cast<ui16>(static_cast<unsigned char>(bytes[1])));
    }

    char ReadByte()
    {
        auto bytes = ReadBytes(1);
        return bytes.empty() ? '\0' : bytes[0];
    }

    TStringBuf ReadCString()
    {
        if (!Error_.IsOK()) {
            return {};
        }
        auto end = Data_.find('\0', Position_);
        if (end == TStringBuf::npos) {
            Error_ = TError("Malformed message: unterminated string");
            return {};
        }
        auto result = Data_.SubStr(Position_, end - Position_);
        Position_ = end + 1;
        return result;
    }

    TStringBuf ReadBytes(size_t size)
    {
        if (!Error_.IsOK()) {
            return {};
        }
        if (Data_.size() - Position_ < size) {
            Error_ = TError("Malformed message: unexpected end of data");
            return {};
        }
        auto result = Data_.SubStr(Position_, size);
        Position_ += size;
        return result;
    }

    TStringBuf ReadRest()
    {
        return ReadBytes(Data_.size() - Position_);
    }

    const TError& GetError() const
    {
        return Error_;
    }

private:
    const TStringBuf Data_;
    size_t Position_ = 0;
    TError Error_;
};

//! Разбирает ErrorResponse в ошибку с кодом ServerError. Если само сообщение
//! повреждено, возвращает ошибку протокола (другой код).
TError ParseErrorResponse(TStringBuf body)
{
    TMessageParser parser(body);
    THashMap<char, std::string> fields;
    while (true) {
        auto type = parser.ReadByte();
        if (type == '\0') {
            break;
        }
        fields[type] = parser.ReadCString();
    }
    if (!parser.GetError().IsOK()) {
        return parser.GetError();
    }

    auto get = [&] (char type) {
        auto it = fields.find(type);
        return it == fields.end() ? std::string() : it->second;
    };

    auto error = TError(EErrorCode::ServerError, "Postgres error: %v", get('M'))
        .With("sqlstate", get('C'))
        .With("severity", get('V').empty() ? get('S') : get('V'));
    if (auto detail = get('D'); !detail.empty()) {
        error = std::move(error).With("detail", detail);
    }
    if (auto constraint = get('n'); !constraint.empty()) {
        error = std::move(error).With("constraint", constraint);
    }
    return error;
}

std::string XorStrings(TStringBuf lhs, TStringBuf rhs)
{
    YT_VERIFY(lhs.size() == rhs.size());
    std::string result(lhs.size(), '\0');
    for (size_t index = 0; index < lhs.size(); ++index) {
        result[index] = lhs[index] ^ rhs[index];
    }
    return result;
}

std::string Sha256(TStringBuf data)
{
    auto digest = TSha256Hasher().Append(data).GetDigest();
    return std::string(digest.data(), digest.size());
}

//! Hi() из RFC 5802, он же PBKDF2-HMAC-SHA256 с одним блоком.
std::string ScramHi(TStringBuf password, TStringBuf salt, int iterations)
{
    std::string saltWithIndex(salt);
    AppendInt32(&saltWithIndex, 1);

    auto u = CreateSha256HmacRaw(password, saltWithIndex);
    auto result = u;
    for (int iteration = 1; iteration < iterations; ++iteration) {
        u = CreateSha256HmacRaw(password, u);
        result = XorStrings(result, u);
    }
    return result;
}

//! Разбирает "k1=v1,k2=v2,..." из сообщений SCRAM.
TErrorOr<THashMap<char, std::string>> ParseScramAttributes(TStringBuf message)
{
    THashMap<char, std::string> result;
    for (const auto& part : StringSplitter(message).Split(',')) {
        auto token = part.Token();
        if (token.size() < 2 || token[1] != '=') {
            return TError("Malformed SCRAM message %Qv", message);
        }
        result[token[0]] = std::string(token.SubStr(2));
    }
    return result;
}

TErrorOr<std::string> GetScramAttribute(const THashMap<char, std::string>& attributes, char key)
{
    auto it = attributes.find(key);
    if (it == attributes.end()) {
        return TError("Missing SCRAM attribute %Qv", key);
    }
    return it->second;
}

TErrorOr<std::string> DecodeBase64(TStringBuf data)
{
    // Base64StrictDecode сообщает о некорректном вводе исключением.
    return CatchErrors([&] {
        return std::string(Base64StrictDecode(data));
    });
}

} // namespace

////////////////////////////////////////////////////////////////////////////////

std::string GetSqlState(const TError& error)
{
    if (auto pgError = error.FindMatching(EErrorCode::ServerError)) {
        return pgError->Attributes().Get<std::string>("sqlstate", "");
    }
    return {};
}

////////////////////////////////////////////////////////////////////////////////

TPgRowReader::TPgRowReader(const TPgResult* result, int row)
    : Result_(result)
    , Row_(row)
{ }

const std::optional<std::string>* TPgRowReader::FindRaw(TStringBuf column)
{
    auto index = Result_->FindColumnIndex(column);
    if (!index) {
        if (Error_.IsOK()) {
            Error_ = TError("No such column %Qv in result", column);
        }
        return nullptr;
    }
    return &Result_->GetRaw(Row_, *index);
}

const TError& TPgRowReader::GetError() const
{
    return Error_;
}

////////////////////////////////////////////////////////////////////////////////

int TPgResult::GetRowCount() const
{
    return std::ssize(Rows_);
}

std::optional<int> TPgResult::FindColumnIndex(TStringBuf name) const
{
    for (int index = 0; index < std::ssize(Columns_); ++index) {
        if (Columns_[index] == name) {
            return index;
        }
    }
    return std::nullopt;
}

const std::optional<std::string>& TPgResult::GetRaw(int row, int column) const
{
    YT_VERIFY(row >= 0 && row < std::ssize(Rows_));
    YT_VERIFY(column >= 0 && column < std::ssize(Rows_[row]));
    return Rows_[row][column];
}

TPgRowReader TPgResult::Row(int row) const
{
    YT_VERIFY(row >= 0 && row < std::ssize(Rows_));
    return TPgRowReader(this, row);
}

i64 TPgResult::GetAffectedRows() const
{
    // Тег вида "INSERT 0 1", "UPDATE 3", "SELECT 2": число строк - последнее слово.
    auto position = CommandTag_.rfind(' ');
    if (position == std::string::npos) {
        return 0;
    }
    i64 result = 0;
    if (!TryFromString(TStringBuf(CommandTag_).SubStr(position + 1), result)) {
        return 0;
    }
    return result;
}

////////////////////////////////////////////////////////////////////////////////

TPgConnection::TPgConnection(NSoa::NNet::TBufferedConnectionPtr connection)
    : Connection_(std::move(connection))
{ }

TErrorOr<TPgConnectionPtr> TPgConnection::Connect(
    const TPgConfig& config,
    const IPollerPtr& poller,
    const TSslContextPtr& sslContext)
{
    auto wrap = [&] (const TError& error) {
        return error.Wrap(EErrorCode::ConnectionError, "Failed to connect to Postgres at %v:%v", config.Host, config.Port);
    };

    auto bufferedConnectionOrError = NSoa::NNet::DialTcp(
        config.Host,
        config.Port,
        poller,
        config.ConnectTimeout,
        config.IOTimeout);
    if (!bufferedConnectionOrError.IsOK()) {
        return wrap(bufferedConnectionOrError);
    }

    auto connection = New<TPgConnection>(std::move(bufferedConnectionOrError).Value());
    if (config.SslMode != EPgSslMode::Disable) {
        if (auto error = connection->NegotiateTls(config, poller, sslContext); !error.IsOK()) {
            connection->Abort();
            return wrap(error);
        }
    }
    if (auto error = connection->Startup(config); !error.IsOK()) {
        connection->Abort();
        return wrap(error);
    }

    YT_LOG_DEBUG("Connected to Postgres (Host: %v, Port: %v, Database: %v, SslMode: %v)",
        config.Host,
        config.Port,
        config.Database,
        config.SslMode);
    return connection;
}

TError TPgConnection::NegotiateTls(
    const TPgConfig& config,
    const IPollerPtr& poller,
    const TSslContextPtr& sslContext)
{
    YT_VERIFY(sslContext);

    std::string request;
    AppendInt32(&request, 8);
    AppendInt32(&request, SslRequestCode);
    SOA_RETURN_IF_ERROR(Connection_->Write(std::move(request)));

    SOA_ASSIGN_OR_RETURN(auto response, Connection_->ReadExact(1));
    if (response[0] != 'S') {
        return TError("Postgres server refused TLS connection")
            .With("response", response);
    }

    std::optional<std::string> host;
    if (config.SslMode == EPgSslMode::VerifyFull) {
        host = config.Host;
    }
    return Connection_->StartTls(sslContext, poller, host);
}

TError TPgConnection::Startup(const TPgConfig& config)
{
    std::string body;
    AppendInt32(&body, ProtocolVersion3);
    AppendCString(&body, "user");
    AppendCString(&body, config.User);
    AppendCString(&body, "database");
    AppendCString(&body, config.Database);
    AppendCString(&body, "client_encoding");
    AppendCString(&body, "UTF8");
    AppendCString(&body, "application_name");
    AppendCString(&body, "soa");
    body.push_back('\0');

    // У StartupMessage нет байта типа.
    std::string startupMessage;
    AppendInt32(&startupMessage, static_cast<i32>(body.size() + 4));
    startupMessage += body;
    SOA_RETURN_IF_ERROR(Connection_->Write(std::move(startupMessage)));

    while (true) {
        SOA_ASSIGN_OR_RETURN(auto message, ReadMessage());
        if (message.Type == 'E') {
            return ParseErrorResponse(message.Body);
        }
        if (message.Type != 'R') {
            return TError("Unexpected message %Qv during authentication", message.Type);
        }

        TMessageParser parser(message.Body);
        auto authType = parser.ReadInt32();
        SOA_RETURN_IF_ERROR(parser.GetError());
        switch (authType) {
            case 0:  // AuthenticationOk
                return WaitReadyForQuery();
            case 3:  // AuthenticationCleartextPassword
                SOA_RETURN_IF_ERROR(SendPassword(config.Password));
                break;
            case 5: {  // AuthenticationMD5Password
                auto salt = parser.ReadBytes(4);
                SOA_RETURN_IF_ERROR(parser.GetError());
                auto inner = GetMD5HexDigestLowerCase(config.Password + config.User);
                auto outer = GetMD5HexDigestLowerCase(inner + std::string(salt));
                SOA_RETURN_IF_ERROR(SendPassword("md5" + outer));
                break;
            }
            case 10: {  // AuthenticationSASL
                auto mechanisms = parser.ReadRest();
                SOA_RETURN_IF_ERROR(parser.GetError());
                SOA_RETURN_IF_ERROR(AuthenticateScram(config, mechanisms));
                break;
            }
            default:
                return TError("Unsupported authentication method %v", authType);
        }
    }
}

TError TPgConnection::SendPassword(std::string payload)
{
    std::string body;
    AppendCString(&body, payload);
    return Connection_->Write(MakeMessage('p', body));
}

TError TPgConnection::AuthenticateScram(const TPgConfig& config, TStringBuf mechanisms)
{
    bool hasScram = false;
    TMessageParser mechanismParser(mechanisms);
    while (true) {
        auto mechanism = mechanismParser.ReadCString();
        if (mechanism.empty()) {
            break;
        }
        hasScram |= (mechanism == "SCRAM-SHA-256");
    }
    SOA_RETURN_IF_ERROR(mechanismParser.GetError());
    if (!hasScram) {
        return TError("Server does not support SCRAM-SHA-256");
    }

    // GenerateCryptoStrongRandomString сообщает о сбое RAND_bytes исключением.
    SOA_ASSIGN_OR_RETURN(auto nonceBytes, CatchErrors([] {
        return GenerateCryptoStrongRandomString(ScramNonceBytes);
    }));

    // Имя пользователя в SCRAM Postgres игнорирует (берёт из StartupMessage).
    auto clientNonce = std::string(Base64Encode(nonceBytes));
    auto clientFirstBare = "n=,r=" + clientNonce;
    auto clientFirst = "n,," + clientFirstBare;

    {
        std::string body;
        AppendCString(&body, "SCRAM-SHA-256");
        AppendInt32(&body, static_cast<i32>(clientFirst.size()));
        body += clientFirst;
        SOA_RETURN_IF_ERROR(Connection_->Write(MakeMessage('p', body)));
    }

    auto readSaslMessage = [&] (i32 expectedType) -> TErrorOr<std::string> {
        SOA_ASSIGN_OR_RETURN(auto message, ReadMessage());
        if (message.Type == 'E') {
            return ParseErrorResponse(message.Body);
        }
        TMessageParser parser(message.Body);
        auto type = parser.ReadInt32();
        auto data = parser.ReadRest();
        SOA_RETURN_IF_ERROR(parser.GetError());
        if (message.Type != 'R' || type != expectedType) {
            return TError("Unexpected message during SCRAM authentication");
        }
        return std::string(data);
    };

    // AuthenticationSASLContinue: server-first-message.
    SOA_ASSIGN_OR_RETURN(auto serverFirst, readSaslMessage(11));
    SOA_ASSIGN_OR_RETURN(auto serverFirstAttributes, ParseScramAttributes(serverFirst));
    SOA_ASSIGN_OR_RETURN(auto serverNonce, GetScramAttribute(serverFirstAttributes, 'r'));
    SOA_ASSIGN_OR_RETURN(auto saltBase64, GetScramAttribute(serverFirstAttributes, 's'));
    SOA_ASSIGN_OR_RETURN(auto salt, DecodeBase64(saltBase64));
    SOA_ASSIGN_OR_RETURN(auto iterationsString, GetScramAttribute(serverFirstAttributes, 'i'));
    int iterations = 0;
    if (!TryFromString(iterationsString, iterations) || iterations <= 0 || !serverNonce.starts_with(clientNonce)) {
        return TError("Invalid SCRAM server-first-message");
    }

    auto saltedPassword = ScramHi(config.Password, salt, iterations);
    auto clientKey = CreateSha256HmacRaw(saltedPassword, "Client Key");
    auto storedKey = Sha256(clientKey);

    // "biws" = base64("n,,") - channel binding не используется.
    auto clientFinalWithoutProof = "c=biws,r=" + serverNonce;
    auto authMessage = clientFirstBare + "," + serverFirst + "," + clientFinalWithoutProof;
    auto clientSignature = CreateSha256HmacRaw(storedKey, authMessage);
    auto clientProof = XorStrings(clientKey, clientSignature);
    auto clientFinal = clientFinalWithoutProof + ",p=" + std::string(Base64Encode(clientProof));
    SOA_RETURN_IF_ERROR(Connection_->Write(MakeMessage('p', clientFinal)));

    // AuthenticationSASLFinal: проверяем, что сервер тоже знает пароль.
    SOA_ASSIGN_OR_RETURN(auto serverFinal, readSaslMessage(12));
    SOA_ASSIGN_OR_RETURN(auto serverFinalAttributes, ParseScramAttributes(serverFinal));
    if (auto it = serverFinalAttributes.find('e'); it != serverFinalAttributes.end()) {
        return TError("SCRAM authentication failed: %v", it->second);
    }
    SOA_ASSIGN_OR_RETURN(auto serverSignatureBase64, GetScramAttribute(serverFinalAttributes, 'v'));
    SOA_ASSIGN_OR_RETURN(auto serverSignature, DecodeBase64(serverSignatureBase64));
    auto serverKey = CreateSha256HmacRaw(saltedPassword, "Server Key");
    auto expectedServerSignature = CreateSha256HmacRaw(serverKey, authMessage);
    if (!ConstantTimeCompare(expectedServerSignature, serverSignature)) {
        return TError("SCRAM server signature mismatch");
    }
    return {};
}

TError TPgConnection::WaitReadyForQuery()
{
    while (true) {
        SOA_ASSIGN_OR_RETURN(auto message, ReadMessage());
        switch (message.Type) {
            case 'Z': {
                TMessageParser parser(message.Body);
                TransactionStatus_ = parser.ReadByte();
                return parser.GetError();
            }
            case 'E':
                return ParseErrorResponse(message.Body);
            case 'S':  // ParameterStatus
            case 'K':  // BackendKeyData
            case 'N':  // NoticeResponse
                break;
            default:
                return TError("Unexpected message %Qv during startup", message.Type);
        }
    }
}

TErrorOr<TPgConnection::TMessage> TPgConnection::ReadMessage()
{
    SOA_ASSIGN_OR_RETURN(auto header, Connection_->ReadExact(5));
    auto length = ParseInt32(TStringBuf(header).SubStr(1));
    if (length < 4 || length > MaxMessageSize) {
        return TError("Invalid message length %v", length);
    }
    SOA_ASSIGN_OR_RETURN(auto body, Connection_->ReadExact(length - 4));
    return TMessage{header[0], std::move(body)};
}

TErrorOr<TPgResult> TPgConnection::Execute(TStringBuf sql, const TPgParams& params)
{
    if (Broken_) {
        return TError(EErrorCode::ConnectionError, "Connection is broken");
    }

    // Пессимистично помечаем соединение сломанным: если fiber отменят посреди
    // протокола, состояние соединения неизвестно и в пул оно не вернётся.
    Broken_ = true;
    auto result = DoExecute(sql, params);
    if (result.IsOK() || result.GetCode() == EErrorCode::ServerError) {
        // После ErrorResponse сервер дочитывает до Sync и присылает ReadyForQuery:
        // протокол в согласованном состоянии.
        Broken_ = false;
        return result;
    }

    Abort();
    return MakeConnectionError(result);
}

TErrorOr<TPgResult> TPgConnection::DoExecute(TStringBuf sql, const TPgParams& params)
{
    // Parse + Bind + Describe + Execute + Sync одним пакетом.
    std::string packet;
    {
        std::string body;
        AppendCString(&body, "");  // безымянный statement
        AppendCString(&body, sql);
        AppendInt16(&body, 0);  // типы параметров выводит сервер
        packet += MakeMessage('P', body);
    }
    {
        std::string body;
        AppendCString(&body, "");  // безымянный portal
        AppendCString(&body, "");
        AppendInt16(&body, 0);  // все параметры в текстовом формате
        AppendInt16(&body, static_cast<i16>(params.size()));
        for (const auto& param : params) {
            if (param) {
                AppendInt32(&body, static_cast<i32>(param->size()));
                body += *param;
            } else {
                AppendInt32(&body, -1);
            }
        }
        AppendInt16(&body, 0);  // все результаты в текстовом формате
        packet += MakeMessage('B', body);
    }
    {
        std::string body;
        body.push_back('P');
        AppendCString(&body, "");
        packet += MakeMessage('D', body);
    }
    {
        std::string body;
        AppendCString(&body, "");
        AppendInt32(&body, 0);  // без ограничения на число строк
        packet += MakeMessage('E', body);
    }
    packet += MakeMessage('S', {});

    SOA_RETURN_IF_ERROR(Connection_->Write(std::move(packet)));

    TPgResult result;
    std::optional<TError> serverError;

    // Читаем до ReadyForQuery: после ErrorResponse сервер пропускает всё до Sync,
    // так что соединение остаётся в согласованном состоянии.
    while (true) {
        SOA_ASSIGN_OR_RETURN(auto message, ReadMessage());
        TMessageParser parser(message.Body);
        switch (message.Type) {
            case '1':  // ParseComplete
            case '2':  // BindComplete
            case 'n':  // NoData
            case 'N':  // NoticeResponse
            case 'S':  // ParameterStatus
            case 'I':  // EmptyQueryResponse
                break;

            case 'T': {  // RowDescription
                auto columnCount = parser.ReadInt16();
                result.Columns_.reserve(std::max<int>(columnCount, 0));
                for (int index = 0; index < columnCount; ++index) {
                    result.Columns_.emplace_back(parser.ReadCString());
                    // table oid, attnum, type oid, typlen, typmod, format
                    parser.ReadBytes(4 + 2 + 4 + 2 + 4 + 2);
                }
                break;
            }

            case 'D': {  // DataRow
                auto columnCount = parser.ReadInt16();
                auto& row = result.Rows_.emplace_back();
                row.reserve(std::max<int>(columnCount, 0));
                for (int index = 0; index < columnCount; ++index) {
                    auto length = parser.ReadInt32();
                    if (length < 0) {
                        row.emplace_back(std::nullopt);
                    } else {
                        row.emplace_back(std::string(parser.ReadBytes(length)));
                    }
                }
                if (std::ssize(row) != std::ssize(result.Columns_)) {
                    return TError("DataRow has %v columns, expected %v", row.size(), result.Columns_.size());
                }
                break;
            }

            case 'C':  // CommandComplete
                result.CommandTag_ = parser.ReadCString();
                break;

            case 'E':
                serverError = ParseErrorResponse(message.Body);
                if (serverError->GetCode() != EErrorCode::ServerError) {
                    return *serverError;
                }
                break;

            case 'Z':
                TransactionStatus_ = parser.ReadByte();
                SOA_RETURN_IF_ERROR(parser.GetError());
                if (serverError) {
                    return *serverError;
                }
                return result;

            default:
                return TError("Unexpected message %Qv", message.Type);
        }
        SOA_RETURN_IF_ERROR(parser.GetError());
    }
}

bool TPgConnection::IsReusable() const
{
    return !Broken_ && !Connection_->IsBroken() && TransactionStatus_ == 'I';
}

char TPgConnection::GetTransactionStatus() const
{
    return TransactionStatus_;
}

void TPgConnection::Abort()
{
    Broken_ = true;
    Connection_->Abort();
}

////////////////////////////////////////////////////////////////////////////////

TErrorOr<TPgClientPtr> TPgClient::Create(TPgConfig config, IPollerPtr poller)
{
    TSslContextPtr sslContext;
    if (config.SslMode != EPgSslMode::Disable) {
        SOA_ASSIGN_OR_RETURN(sslContext, NSoa::NNet::CreateClientSslContext(
            config.SslMode == EPgSslMode::VerifyFull,
            config.SslRootCertPath));
    }
    return New<TPgClient>(std::move(config), std::move(poller), std::move(sslContext));
}

TPgClient::TPgClient(TPgConfig config, IPollerPtr poller, TSslContextPtr sslContext)
    : Config_(std::move(config))
    , Poller_(std::move(poller))
    , SslContext_(std::move(sslContext))
    , Pool_(New<TPool>(Config_.PoolSize, [this] {
        return TPgConnection::Connect(Config_, Poller_, SslContext_);
    }))
{ }

TErrorOr<TPgClient::TConnectionGuard> TPgClient::Acquire()
{
    return Pool_->Acquire();
}

TErrorOr<TPgResult> TPgClient::Execute(TStringBuf sql, const TPgParams& params)
{
    SOA_ASSIGN_OR_RETURN(auto guard, Acquire());
    return guard->Execute(sql, params);
}

TError TPgClient::WaitUntilAvailable(TDuration timeout)
{
    auto deadline = TInstant::Now() + timeout;
    while (true) {
        auto result = Execute("SELECT 1");
        if (result.IsOK()) {
            YT_LOG_INFO("Postgres is available (Host: %v, Database: %v)", Config_.Host, Config_.Database);
            return {};
        }
        if (TInstant::Now() > deadline) {
            return TError(result).Wrap("Postgres is not available after %v", timeout);
        }
        YT_LOG_WARNING(result, "Postgres is not available yet, retrying (Host: %v)", Config_.Host);
        TDelayedExecutor::WaitForDuration(TDuration::Seconds(1));
    }
}

////////////////////////////////////////////////////////////////////////////////

} // namespace NSoa::NPg
