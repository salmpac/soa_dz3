#pragma once

#include <soa/lib/rpc/circuit_breaker_channel.h>
#include <soa/lib/rpc/retrying_channel.h>

#include <soa/proto/flight_service.pb.h>

#include <yt/yt/core/rpc/public.h>

#include <library/cpp/yt/error/error.h>

#include <optional>
#include <string>
#include <vector>

namespace NSoa::NBooking {

////////////////////////////////////////////////////////////////////////////////

struct TFlightClientConfig
{
    //! host:port Flight Service.
    std::string Address;
    std::string ApiKey;

    //! PEM с CA для проверки сертификата Flight Service; пусто - без TLS.
    std::string TlsCaCertPath;
    //! Клиентский сертификат и ключ для mTLS; пусто - без клиентского сертификата.
    std::string TlsCertPath;
    std::string TlsKeyPath;

    //! Таймаут одной попытки.
    TDuration RequestTimeout = TDuration::Seconds(2);

    NSoa::NRpc::TRetryConfig Retry;
    NSoa::NRpc::TCircuitBreakerConfig CircuitBreaker;
};

DECLARE_REFCOUNTED_CLASS(TFlightClient)

//! Типизированный клиент Flight Service.
/*!
 *  Канал собирается из interceptor'ов (снаружи внутрь):
 *    retry -> circuit breaker -> API key -> gRPC.
 *  Каждая попытка retry проходит через circuit breaker: когда цепь размыкается,
 *  оставшиеся попытки сразу получают CircuitOpen и не повторяются.
 */
class TFlightClient
    : public NYT::TRefCounted
{
public:
    static NYT::TErrorOr<TFlightClientPtr> Create(const TFlightClientConfig& config);

    NYT::TErrorOr<std::vector<flight::v1::Flight>> SearchFlights(
        const std::string& origin,
        const std::string& destination,
        const std::optional<flight::v1::Date>& departureDate);

    NYT::TErrorOr<flight::v1::Flight> GetFlight(i64 flightId);

    //! Идемпотентно по #bookingId.
    NYT::TErrorOr<flight::v1::ReserveSeatsResponse> ReserveSeats(i64 flightId, int seatCount, const std::string& bookingId);

    //! Делает резервацию постоянной. Идемпотентно.
    NYT::TErrorOr<flight::v1::SeatReservation> ConfirmReservation(const std::string& bookingId);

    NYT::TErrorOr<flight::v1::SeatReservation> ReleaseReservation(const std::string& bookingId);

    // Административные операции.
    NYT::TErrorOr<flight::v1::Flight> CreateFlight(const flight::v1::CreateFlightRequest& request);
    NYT::TErrorOr<flight::v1::Flight> UpdateFlight(
        i64 flightId,
        std::optional<flight::v1::FlightStatus> status,
        std::optional<flight::v1::Money> price);
    NYT::TErrorOr<std::vector<flight::v1::Airport>> ListAirports();
    NYT::TErrorOr<std::vector<flight::v1::Airline>> ListAirlines();

private:
    const NYT::NRpc::IChannelPtr Channel_;
    const TDuration RequestTimeout_;

    TFlightClient(NYT::NRpc::IChannelPtr channel, TDuration requestTimeout);
    DECLARE_NEW_FRIEND()
};

DEFINE_REFCOUNTED_TYPE(TFlightClient)

////////////////////////////////////////////////////////////////////////////////

} // namespace NSoa::NBooking
