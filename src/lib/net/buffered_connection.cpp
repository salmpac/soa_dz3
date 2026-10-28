#include "buffered_connection.h"

#include <soa/lib/common/error_macros.h>

#include <yt/yt/core/concurrency/scheduler_api.h>

#include <yt/yt/core/crypto/config.h>
#include <yt/yt/core/crypto/tls.h>

#include <yt/yt/core/logging/log.h>

#include <yt/yt/core/net/address.h>
#include <yt/yt/core/net/config.h>
#include <yt/yt/core/net/connection.h>
#include <yt/yt/core/net/dialer.h>

#include <algorithm>
#include <cstring>

namespace NSoa::NNet {

using namespace NYT;
using namespace NYT::NConcurrency;
using namespace NYT::NNet;

////////////////////////////////////////////////////////////////////////////////

YT_DEFINE_LEAKY_GLOBAL(const NLogging::TLogger, NetLogger, "Net");
constinit const auto Logger = NetLogger;

static constexpr size_t ReadBufferSize = 64 * 1024;
static constexpr size_t MaxLineLength = 64 * 1024 * 1024;

////////////////////////////////////////////////////////////////////////////////

namespace {

//! Помечает соединение сломанным на время операции и снимает пометку только при
//! успешном завершении. Если fiber отменят посреди WaitFor, пометка останется.
class TPessimisticGuard
{
public:
    explicit TPessimisticGuard(bool* broken)
        : Broken_(broken)
    {
        *Broken_ = true;
    }

    void Complete()
    {
        *Broken_ = false;
    }

private:
    bool* const Broken_;
};

} // namespace

////////////////////////////////////////////////////////////////////////////////

TBufferedConnection::TBufferedConnection(IConnectionPtr connection, TDuration ioTimeout)
    : Connection_(std::move(connection))
    , IOTimeout_(ioTimeout)
    , ReadBuffer_(TSharedMutableRef::Allocate(ReadBufferSize, {.InitializeStorage = false}))
{ }

TBufferedConnection::~TBufferedConnection()
{
    Abort();
}

TError TBufferedConnection::MarkBroken(TError error)
{
    Broken_ = true;
    return error;
}

TError TBufferedConnection::FillBuffer()
{
    if (ReadBegin_ == ReadEnd_) {
        ReadBegin_ = ReadEnd_ = 0;
    } else if (ReadEnd_ == ReadBuffer_.Size()) {
        std::memmove(ReadBuffer_.Begin(), ReadBuffer_.Begin() + ReadBegin_, ReadEnd_ - ReadBegin_);
        ReadEnd_ -= ReadBegin_;
        ReadBegin_ = 0;
    }

    auto bytesReadOrError = WaitFor(Connection_
        ->Read(ReadBuffer_.Slice(ReadEnd_, ReadBuffer_.Size()))
        .WithTimeout(IOTimeout_));
    if (!bytesReadOrError.IsOK()) {
        return MarkBroken(bytesReadOrError.Wrap("Error reading from %v", Connection_->GetRemoteAddress()));
    }

    auto bytesRead = bytesReadOrError.Value();
    if (bytesRead == 0) {
        return MarkBroken(TError("Connection to %v closed by peer", Connection_->GetRemoteAddress()));
    }
    ReadEnd_ += bytesRead;
    return {};
}

TErrorOr<std::string> TBufferedConnection::ReadExact(size_t size)
{
    if (Broken_) {
        return TError("Connection is broken");
    }
    TPessimisticGuard guard(&Broken_);

    std::string result;
    result.reserve(size);
    while (result.size() < size) {
        if (ReadBegin_ == ReadEnd_) {
            SOA_RETURN_IF_ERROR(FillBuffer());
        }
        auto chunkSize = std::min(size - result.size(), ReadEnd_ - ReadBegin_);
        result.append(ReadBuffer_.Begin() + ReadBegin_, chunkSize);
        ReadBegin_ += chunkSize;
    }

    guard.Complete();
    return result;
}

TErrorOr<std::string> TBufferedConnection::ReadLine()
{
    if (Broken_) {
        return TError("Connection is broken");
    }
    TPessimisticGuard guard(&Broken_);

    std::string line;
    while (true) {
        if (ReadBegin_ == ReadEnd_) {
            SOA_RETURN_IF_ERROR(FillBuffer());
        }

        const char* begin = ReadBuffer_.Begin() + ReadBegin_;
        const char* end = ReadBuffer_.Begin() + ReadEnd_;
        const char* newline = std::find(begin, end, '\n');
        line.append(begin, newline);

        if (newline == end) {
            ReadBegin_ = ReadEnd_;
            if (line.size() > MaxLineLength) {
                return TError("Line is too long");
            }
            continue;
        }

        ReadBegin_ += newline - begin + 1;
        if (line.empty() || line.back() != '\r') {
            return TError("Malformed line: missing CR before LF");
        }
        line.pop_back();

        guard.Complete();
        return line;
    }
}

TError TBufferedConnection::Write(std::string data)
{
    if (Broken_) {
        return TError("Connection is broken");
    }
    TPessimisticGuard guard(&Broken_);

    auto error = WaitFor(Connection_
        ->Write(TSharedRef::FromString(std::move(data)))
        .WithTimeout(IOTimeout_));
    if (!error.IsOK()) {
        return error.Wrap("Error writing to %v", Connection_->GetRemoteAddress());
    }

    guard.Complete();
    return {};
}

TError TBufferedConnection::StartTls(
    const NCrypto::TSslContextPtr& sslContext,
    const IPollerPtr& poller,
    const std::optional<std::string>& host)
{
    if (Broken_) {
        return TError("Connection is broken");
    }
    if (ReadBegin_ != ReadEnd_) {
        Abort();
        return TError("Received unencrypted data before TLS handshake");
    }

    SOA_ASSIGN_OR_RETURN(Connection_, CatchErrors([&] {
        return sslContext->StartClientTls(Connection_, poller, host);
    }));
    return {};
}

void TBufferedConnection::Abort()
{
    Broken_ = true;
    YT_UNUSED_FUTURE(Connection_->Abort());
}

bool TBufferedConnection::IsBroken() const
{
    return Broken_;
}

////////////////////////////////////////////////////////////////////////////////

TErrorOr<NCrypto::TSslContextPtr> CreateClientSslContext(bool verify, const std::string& caCertPath)
{
    if (verify && caCertPath.empty()) {
        return TError("CA certificate path is required for TLS with certificate verification");
    }

    // TSslContext сообщает об ошибках (нет файла, битый PEM) исключениями.
    return CatchErrors([&] {
        auto sslContext = New<NCrypto::TSslContext>();
        if (verify) {
            sslContext->AddCertificateAuthorityFromFile(caCertPath);
        } else {
            auto sslConfig = New<NCrypto::TSslContextConfig>();
            sslConfig->InsecureSkipVerify = true;
            sslContext->ApplyConfig(sslConfig);
        }
        sslContext->Commit();
        return sslContext;
    });
}

TErrorOr<TBufferedConnectionPtr> DialTcp(
    const std::string& host,
    int port,
    const IPollerPtr& poller,
    TDuration connectTimeout,
    TDuration ioTimeout)
{
    auto addressOrError = WaitFor(TAddressResolver::Get()->Resolve(host));
    SOA_RETURN_IF_ERROR_WRAP(addressOrError, "Failed to resolve %Qv", host);
    auto address = TNetworkAddress(addressOrError.Value(), port);

    auto dialerConfig = New<TDialerConfig>();
    dialerConfig->EnableNoDelay = true;
    auto dialer = CreateDialer(dialerConfig, poller, Logger());

    auto connectionOrError = WaitFor(dialer->Dial(address).WithTimeout(connectTimeout));
    SOA_RETURN_IF_ERROR_WRAP(connectionOrError, "Failed to connect to %v:%v", host, port);

    YT_LOG_DEBUG("Connection established (Host: %v, Address: %v)", host, address);
    return New<TBufferedConnection>(connectionOrError.Value(), ioTimeout);
}

////////////////////////////////////////////////////////////////////////////////

} // namespace NSoa::NNet
