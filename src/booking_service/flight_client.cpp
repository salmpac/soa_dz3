#include "flight_client.h"

#include <soa/lib/common/error_macros.h>
#include <soa/lib/rpc/api_key_channel.h>

#include <soa/proto/flight_service_proxy.h>

#include <yt/yt/core/concurrency/scheduler_api.h>

#include <yt/yt/core/crypto/config.h>

#include <yt/yt/core/rpc/grpc/channel.h>
#include <yt/yt/core/rpc/grpc/config.h>

namespace NSoa::NBooking {

using namespace NYT;
using namespace NYT::NConcurrency;
using namespace flight::v1;

using NSoa::NFlight::TFlightServiceProxy;

////////////////////////////////////////////////////////////////////////////////

namespace {

TErrorOr<NYT::NRpc::IChannelPtr> CreateGrpcChannel(const TFlightClientConfig& config)
{
    // Конфиги и создание gRPC-канала сообщают об ошибках исключениями.
    return CatchErrors([&] () -> NYT::NRpc::IChannelPtr {
        auto channelConfig = New<NYT::NRpc::NGrpc::TChannelConfig>();
        channelConfig->Address = config.Address;
        if (!config.TlsCaCertPath.empty()) {
            auto credentials = New<NYT::NRpc::NGrpc::TChannelCredentialsConfig>();
            credentials->PemRootCerts = NCrypto::TPemBlobConfig::CreateFileReference(config.TlsCaCertPath);
            credentials->VerifyServerCert = true;
            if (!config.TlsCertPath.empty()) {
                auto keyCertPair = New<NYT::NRpc::NGrpc::TSslPemKeyCertPairConfig>();
                keyCertPair->CertChain = NCrypto::TPemBlobConfig::CreateFileReference(config.TlsCertPath);
                keyCertPair->PrivateKey = NCrypto::TPemBlobConfig::CreateFileReference(config.TlsKeyPath);
                credentials->PemKeyCertPair = keyCertPair;
            }
            channelConfig->Credentials = credentials;
        }
        return NYT::NRpc::NGrpc::CreateGrpcChannel(channelConfig);
    });
}

} // namespace

////////////////////////////////////////////////////////////////////////////////

TErrorOr<TFlightClientPtr> TFlightClient::Create(const TFlightClientConfig& config)
{
    if (config.ApiKey.empty()) {
        return TError("Flight Service API key is required");
    }

    SOA_ASSIGN_OR_RETURN(auto channel, CreateGrpcChannel(config));
    channel = NSoa::NRpc::CreateApiKeyChannel(std::move(channel), config.ApiKey);
    channel = NSoa::NRpc::CreateCircuitBreakerChannel(std::move(channel), config.CircuitBreaker, "FlightService");
    SOA_ASSIGN_OR_RETURN(channel, NSoa::NRpc::CreateRetryingChannel(std::move(channel), config.Retry, "FlightService"));

    return New<TFlightClient>(std::move(channel), config.RequestTimeout);
}

TFlightClient::TFlightClient(NYT::NRpc::IChannelPtr channel, TDuration requestTimeout)
    : Channel_(std::move(channel))
    , RequestTimeout_(requestTimeout)
{ }

TErrorOr<std::vector<Flight>> TFlightClient::SearchFlights(
    const std::string& origin,
    const std::string& destination,
    const std::optional<Date>& departureDate)
{
    TFlightServiceProxy proxy(Channel_);
    proxy.SetDefaultTimeout(RequestTimeout_);

    auto request = proxy.SearchFlights();
    request->set_origin(origin);
    request->set_destination(destination);
    if (departureDate) {
        *request->mutable_departure_date() = *departureDate;
    }

    SOA_ASSIGN_OR_RETURN(auto response, WaitFor(request->Invoke()));
    return std::vector<Flight>(response->flights().begin(), response->flights().end());
}

TErrorOr<Flight> TFlightClient::GetFlight(i64 flightId)
{
    TFlightServiceProxy proxy(Channel_);
    proxy.SetDefaultTimeout(RequestTimeout_);

    auto request = proxy.GetFlight();
    request->set_flight_id(flightId);

    SOA_ASSIGN_OR_RETURN(auto response, WaitFor(request->Invoke()));
    return response->flight();
}

TErrorOr<ReserveSeatsResponse> TFlightClient::ReserveSeats(i64 flightId, int seatCount, const std::string& bookingId)
{
    TFlightServiceProxy proxy(Channel_);
    proxy.SetDefaultTimeout(RequestTimeout_);

    auto request = proxy.ReserveSeats();
    request->set_flight_id(flightId);
    request->set_seat_count(seatCount);
    request->set_booking_id(bookingId);

    SOA_ASSIGN_OR_RETURN(auto response, WaitFor(request->Invoke()));
    ReserveSeatsResponse result;
    result.CopyFrom(*response);
    return result;
}

TErrorOr<SeatReservation> TFlightClient::ConfirmReservation(const std::string& bookingId)
{
    TFlightServiceProxy proxy(Channel_);
    proxy.SetDefaultTimeout(RequestTimeout_);

    auto request = proxy.ConfirmReservation();
    request->set_booking_id(bookingId);

    SOA_ASSIGN_OR_RETURN(auto response, WaitFor(request->Invoke()));
    return response->reservation();
}

TErrorOr<SeatReservation> TFlightClient::ReleaseReservation(const std::string& bookingId)
{
    TFlightServiceProxy proxy(Channel_);
    proxy.SetDefaultTimeout(RequestTimeout_);

    auto request = proxy.ReleaseReservation();
    request->set_booking_id(bookingId);

    SOA_ASSIGN_OR_RETURN(auto response, WaitFor(request->Invoke()));
    return response->reservation();
}

TErrorOr<Flight> TFlightClient::CreateFlight(const CreateFlightRequest& flight)
{
    TFlightServiceProxy proxy(Channel_);
    proxy.SetDefaultTimeout(RequestTimeout_);

    auto request = proxy.CreateFlight();
    request->CopyFrom(flight);

    SOA_ASSIGN_OR_RETURN(auto response, WaitFor(request->Invoke()));
    return response->flight();
}

TErrorOr<Flight> TFlightClient::UpdateFlight(
    i64 flightId,
    std::optional<FlightStatus> status,
    std::optional<Money> price)
{
    TFlightServiceProxy proxy(Channel_);
    proxy.SetDefaultTimeout(RequestTimeout_);

    auto request = proxy.UpdateFlight();
    request->set_flight_id(flightId);
    if (status) {
        request->set_status(*status);
    }
    if (price) {
        *request->mutable_price() = *price;
    }

    SOA_ASSIGN_OR_RETURN(auto response, WaitFor(request->Invoke()));
    return response->flight();
}

TErrorOr<std::vector<Airport>> TFlightClient::ListAirports()
{
    TFlightServiceProxy proxy(Channel_);
    proxy.SetDefaultTimeout(RequestTimeout_);

    SOA_ASSIGN_OR_RETURN(auto response, WaitFor(proxy.ListAirports()->Invoke()));
    return std::vector<Airport>(response->airports().begin(), response->airports().end());
}

TErrorOr<std::vector<Airline>> TFlightClient::ListAirlines()
{
    TFlightServiceProxy proxy(Channel_);
    proxy.SetDefaultTimeout(RequestTimeout_);

    SOA_ASSIGN_OR_RETURN(auto response, WaitFor(proxy.ListAirlines()->Invoke()));
    return std::vector<Airline>(response->airlines().begin(), response->airlines().end());
}

////////////////////////////////////////////////////////////////////////////////

} // namespace NSoa::NBooking
