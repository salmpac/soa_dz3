#pragma once

#include <soa/proto/flight_service.pb.h>

#include <yt/yt/core/rpc/client.h>

namespace NSoa::NFlight {

////////////////////////////////////////////////////////////////////////////////

//! Дескриптор сервиса: полное имя совпадает с `package.service` из flight_service.proto,
//! поэтому gRPC-путь - "/flight.v1.FlightService/<Method>", как у обычного gRPC-сервера.
inline const NYT::NRpc::TServiceDescriptor& GetFlightServiceDescriptor()
{
    static const auto descriptor = NYT::NRpc::TServiceDescriptor("FlightService")
        .SetNamespace("flight.v1");
    return descriptor;
}

//! Типизированный клиент Flight Service поверх YT RPC.
class TFlightServiceProxy
    : public NYT::NRpc::TProxyBase
{
public:
    explicit TFlightServiceProxy(NYT::NRpc::IChannelPtr channel)
        : TProxyBase(std::move(channel), GetFlightServiceDescriptor())
    { }

    DEFINE_RPC_PROXY_METHOD_GENERIC(SearchFlights, ::flight::v1::SearchFlightsRequest, ::flight::v1::SearchFlightsResponse);
    DEFINE_RPC_PROXY_METHOD_GENERIC(GetFlight, ::flight::v1::GetFlightRequest, ::flight::v1::GetFlightResponse);
    DEFINE_RPC_PROXY_METHOD_GENERIC(ReserveSeats, ::flight::v1::ReserveSeatsRequest, ::flight::v1::ReserveSeatsResponse);
    DEFINE_RPC_PROXY_METHOD_GENERIC(ConfirmReservation, ::flight::v1::ConfirmReservationRequest, ::flight::v1::ConfirmReservationResponse);
    DEFINE_RPC_PROXY_METHOD_GENERIC(ReleaseReservation, ::flight::v1::ReleaseReservationRequest, ::flight::v1::ReleaseReservationResponse);
    DEFINE_RPC_PROXY_METHOD_GENERIC(CreateFlight, ::flight::v1::CreateFlightRequest, ::flight::v1::CreateFlightResponse);
    DEFINE_RPC_PROXY_METHOD_GENERIC(UpdateFlight, ::flight::v1::UpdateFlightRequest, ::flight::v1::UpdateFlightResponse);
    DEFINE_RPC_PROXY_METHOD_GENERIC(ListAirports, ::flight::v1::ListAirportsRequest, ::flight::v1::ListAirportsResponse);
    DEFINE_RPC_PROXY_METHOD_GENERIC(ListAirlines, ::flight::v1::ListAirlinesRequest, ::flight::v1::ListAirlinesResponse);
};

////////////////////////////////////////////////////////////////////////////////

} // namespace NSoa::NFlight
