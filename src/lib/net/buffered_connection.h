#pragma once

#include <yt/yt/core/concurrency/public.h>
#include <yt/yt/core/crypto/public.h>
#include <yt/yt/core/net/public.h>

#include <library/cpp/yt/error/error.h>
#include <library/cpp/yt/memory/ref.h>

#include <util/datetime/base.h>

#include <optional>
#include <string>

namespace NSoa::NNet {

////////////////////////////////////////////////////////////////////////////////

DECLARE_REFCOUNTED_CLASS(TBufferedConnection)

//! TCP-соединение с буферизованным чтением.
/*!
 *  Все методы "блокирующие" с точки зрения вызывающего fiber'а: внутри они
 *  ждут TFuture от NYT::NNet::IConnection через WaitFor, т.е. fiber
 *  засыпает, а поток в это время обслуживает другие fiber'ы (epoll внутри
 *  YT-шного poller'а).
 *
 *  Любая ошибка ввода-вывода (и отмена fiber'а посреди операции) помечает
 *  соединение сломанным: дальше по нему читать/писать нельзя.
 *
 *  Не потокобезопасно: одновременно соединением пользуется один fiber.
 */
class TBufferedConnection
    : public NYT::TRefCounted
{
public:
    TBufferedConnection(NYT::NNet::IConnectionPtr connection, TDuration ioTimeout);
    ~TBufferedConnection();

    //! Читает ровно #size байт.
    NYT::TErrorOr<std::string> ReadExact(size_t size);

    //! Читает строку до "\r\n" (терминатор не включается).
    NYT::TErrorOr<std::string> ReadLine();

    //! Отправляет #data целиком.
    NYT::TError Write(std::string data);

    //! Переводит соединение в TLS после согласования на уровне протокола
    //! (Postgres SSLRequest и т.п.).
    /*!
     *  В буфере не должно быть непрочитанных данных: всё, что пришло до
     *  рукопожатия, не защищено TLS и может быть подмешано атакующим
     *  (ср. CVE-2021-23222).
     *  Если задан #host, он используется для SNI и проверки сертификата.
     */
    NYT::TError StartTls(
        const NYT::NCrypto::TSslContextPtr& sslContext,
        const NYT::NConcurrency::IPollerPtr& poller,
        const std::optional<std::string>& host);

    //! Закрывает соединение без ожидания.
    void Abort();

    bool IsBroken() const;

private:
    NYT::NNet::IConnectionPtr Connection_;
    const TDuration IOTimeout_;

    NYT::TSharedMutableRef ReadBuffer_;
    size_t ReadBegin_ = 0;
    size_t ReadEnd_ = 0;
    bool Broken_ = false;

    NYT::TError FillBuffer();
    NYT::TError MarkBroken(NYT::TError error);
};

DEFINE_REFCOUNTED_TYPE(TBufferedConnection)

////////////////////////////////////////////////////////////////////////////////

//! Клиентский TLS-контекст: с проверкой сертификата по CA из #caCertPath
//! или, если #verify == false, без проверки.
NYT::TErrorOr<NYT::NCrypto::TSslContextPtr> CreateClientSslContext(bool verify, const std::string& caCertPath);

//! Резолвит #host, устанавливает TCP-соединение и возвращает его.
NYT::TErrorOr<TBufferedConnectionPtr> DialTcp(
    const std::string& host,
    int port,
    const NYT::NConcurrency::IPollerPtr& poller,
    TDuration connectTimeout,
    TDuration ioTimeout);

////////////////////////////////////////////////////////////////////////////////

} // namespace NSoa::NNet
