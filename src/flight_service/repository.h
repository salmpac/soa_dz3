#pragma once

#include <soa/proto/flight_service.pb.h>

#include <library/cpp/yt/error/error.h>
#include <library/cpp/yt/memory/ref_counted.h>

#include <util/datetime/base.h>

#include <optional>
#include <string>
#include <vector>

namespace NSoa::NFlight {

////////////////////////////////////////////////////////////////////////////////

//! Календарная дата вылета (UTC) в формате YYYY-MM-DD.
using TDate = std::string;

//! Маршрут и дата рейса: по ним строятся ключи, которые затрагивает изменение рейса.
struct TFlightRoute
{
    i64 FlightId = 0;
    std::string Origin;
    std::string Destination;
    TDate DepartureDate;
};

struct TReserveResult
{
    flight::v1::SeatReservation Reservation;
    flight::v1::Money SeatPrice;
    TFlightRoute Route;
    //! true, если резервация с этим booking_id уже существовала (повторный вызов).
    bool Replayed = false;
};

struct TReleaseResult
{
    flight::v1::SeatReservation Reservation;
    TFlightRoute Route;
    //! true, если резервация уже была освобождена ранее.
    bool Replayed = false;
};

//! Резервации одного рейса, переведённые в EXPIRED фоновой задачей.
struct TExpiredReservations
{
    TFlightRoute Route;
    std::vector<std::string> BookingIds;
    //! Сколько мест возвращено рейсу.
    int SeatCount = 0;
};

struct TNewFlight
{
    std::string AirlineCode;
    int FlightNo = 0;
    std::string Origin;
    std::string Destination;
    TInstant DepartureTime;
    TInstant ArrivalTime;
    int TotalSeats = 0;
    flight::v1::Money Price;
};

struct TUpdateResult
{
    flight::v1::Flight Flight;
    TFlightRoute Route;
};

////////////////////////////////////////////////////////////////////////////////

DECLARE_REFCOUNTED_STRUCT(IFlightRepository)

//! Доступ к рейсам и резервациям. Все методы вызываются из fiber'а.
/*!
 *  Ошибки возвращаются с кодами из soa/lib/common/errors.h
 *  (NotFound, ResourceExhausted, ...); недоступность хранилища - Unavailable.
 *
 *  Реализации:
 *    - CreatePgFlightRepository: PostgreSQL;
 *    - CreateFlightRepositoryWithRedis: Cache-Aside в Redis поверх другого репозитория.
 */
struct IFlightRepository
    : public NYT::TRefCounted
{
    //! Рейсы в продаже (SCHEDULED, ещё не вылетевшие) по маршруту и (опционально) дате вылета.
    virtual NYT::TErrorOr<std::vector<flight::v1::Flight>> SearchFlights(
        const std::string& origin,
        const std::string& destination,
        const std::optional<TDate>& departureDate) = 0;

    virtual NYT::TErrorOr<flight::v1::Flight> GetFlight(i64 flightId) = 0;

    //! Атомарно: проверяет места, уменьшает available_seats и создаёт временную
    //! резервацию (со сроком жизни). Идемпотентен по #bookingId.
    virtual NYT::TErrorOr<TReserveResult> ReserveSeats(i64 flightId, int seatCount, const std::string& bookingId) = 0;

    //! Делает резервацию постоянной. Идемпотентен.
    //! FailedPrecondition, если резервация истекла или освобождена.
    virtual NYT::TErrorOr<flight::v1::SeatReservation> ConfirmReservation(const std::string& bookingId) = 0;

    //! Атомарно: возвращает места и переводит резервацию в RELEASED. Идемпотентен.
    virtual NYT::TErrorOr<TReleaseResult> ReleaseReservation(const std::string& bookingId) = 0;

    //! Переводит неподтверждённые просроченные резервации в EXPIRED и возвращает
    //! места; обрабатывает не больше #maxFlights рейсов за вызов. Рейсы, которые
    //! сейчас заблокированы другой транзакцией, пропускает до следующего вызова.
    virtual NYT::TErrorOr<std::vector<TExpiredReservations>> ExpireReservations(int maxFlights) = 0;

    //! Новый рейс в статусе SCHEDULED. Неизвестные авиакомпания/аэропорт и
    //! нарушения ограничений - InvalidArgument; дубль номера на дату - AlreadyExists.
    virtual NYT::TErrorOr<TUpdateResult> CreateFlight(const TNewFlight& flight) = 0;

    virtual NYT::TErrorOr<std::vector<flight::v1::Airport>> ListAirports() = 0;
    virtual NYT::TErrorOr<std::vector<flight::v1::Airline>> ListAirlines() = 0;

    virtual NYT::TErrorOr<TUpdateResult> UpdateFlight(
        i64 flightId,
        std::optional<flight::v1::FlightStatus> status,
        std::optional<flight::v1::Money> price) = 0;
};

DEFINE_REFCOUNTED_TYPE(IFlightRepository)

////////////////////////////////////////////////////////////////////////////////

} // namespace NSoa::NFlight
