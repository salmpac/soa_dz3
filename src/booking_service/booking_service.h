#pragma once

#include "booking_repository.h"
#include "flight_client.h"

namespace NSoa::NBooking {

////////////////////////////////////////////////////////////////////////////////

struct TCreateBookingRequest
{
    std::string UserId;
    i64 FlightId = 0;
    std::string PassengerName;
    std::string PassengerEmail;
    int SeatCount = 0;
};

//! Бизнес-логика Booking Service (без транспорта: HTTP - в http_api).
class TBookingService
{
public:
    TBookingService(TFlightClientPtr flightClient, TBookingRepository* repository);

    NYT::TErrorOr<std::vector<flight::v1::Flight>> SearchFlights(
        const std::string& origin,
        const std::string& destination,
        const std::optional<flight::v1::Date>& departureDate);

    NYT::TErrorOr<flight::v1::Flight> GetFlight(i64 flightId);

    //! GetFlight -> ReserveSeats (временная резервация) -> фиксация цены ->
    //! транзакция { INSERT CONFIRMED; ConfirmReservation } .
    //! Если резервирование не удалось, бронирование не создаётся. Если не удалось
    //! записать бронирование или подтвердить резервацию, транзакция откатывается,
    //! а резервация освобождается сразу (или истечёт сама по TTL).
    NYT::TErrorOr<TBooking> CreateBooking(const TCreateBookingRequest& request);

    NYT::TErrorOr<TBooking> GetBooking(const std::string& bookingId);
    NYT::TErrorOr<std::vector<TBooking>> ListBookings(const std::string& userId);

    //! CONFIRMED -> ReleaseReservation во Flight Service -> CANCELLED.
    NYT::TErrorOr<TBooking> CancelBooking(const std::string& bookingId);

private:
    const TFlightClientPtr FlightClient_;
    TBookingRepository* const Repository_;

    void CompensateReservation(const std::string& bookingId);
};

////////////////////////////////////////////////////////////////////////////////

} // namespace NSoa::NBooking
