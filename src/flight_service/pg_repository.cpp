#include "pg_repository.h"

#include <soa/lib/common/error_macros.h>
#include <soa/lib/common/errors.h>

#include <yt/yt/core/logging/log.h>

namespace NSoa::NFlight {

using namespace NYT;
using namespace flight::v1;

using NPg::CheckViolation;
using NPg::DeadlockDetected;
using NPg::ForeignKeyViolation;
using NPg::GetSqlState;
using NPg::PgParam;
using NPg::SerializationFailure;
using NPg::TPgConnection;
using NPg::TPgResult;
using NPg::UniqueViolation;
using NSoa::EErrorCode;

////////////////////////////////////////////////////////////////////////////////

YT_DEFINE_LEAKY_GLOBAL(const NLogging::TLogger, PgRepositoryLogger, "PgRepository");
constinit const auto Logger = PgRepositoryLogger;

////////////////////////////////////////////////////////////////////////////////

namespace {

// Время отдаём в микросекундах от эпохи, чтобы не разбирать текстовый timestamptz.
constexpr TStringBuf FlightColumns = R"sql(
    f.id,
    f.airline_code,
    a.name AS airline_name,
    f.flight_no,
    f.origin,
    f.destination,
    (extract(epoch FROM f.departure_time) * 1000000)::bigint AS departure_us,
    (extract(epoch FROM f.arrival_time) * 1000000)::bigint AS arrival_us,
    to_char(f.departure_time AT TIME ZONE 'UTC', 'YYYY-MM-DD') AS departure_date,
    f.total_seats,
    f.available_seats,
    f.price_amount,
    f.currency,
    f.status::text AS status
)sql";

constexpr TStringBuf ReservationColumns = R"sql(
    r.id,
    r.flight_id,
    r.booking_id::text AS booking_id,
    r.seat_count,
    r.status::text AS status,
    (extract(epoch FROM r.created_at) * 1000000)::bigint AS created_us,
    (extract(epoch FROM r.updated_at) * 1000000)::bigint AS updated_us,
    (extract(epoch FROM r.expires_at) * 1000000)::bigint AS expires_us,
    (extract(epoch FROM r.confirmed_at) * 1000000)::bigint AS confirmed_us
)sql";

//! Неподтверждённая активная резервация, срок которой истёк.
constexpr TStringBuf ExpiredReservationCondition =
    "r.status = 'ACTIVE' AND r.confirmed_at IS NULL AND r.expires_at <= now()";

void ToTimestamp(google::protobuf::Timestamp* timestamp, i64 microseconds)
{
    timestamp->set_seconds(microseconds / 1'000'000);
    timestamp->set_nanos(static_cast<int>(microseconds % 1'000'000) * 1000);
}

TErrorOr<FlightStatus> ParseFlightStatus(const std::string& status)
{
    FlightStatus result;
    if (!FlightStatus_Parse("FLIGHT_STATUS_" + status, &result)) {
        return TError("Unknown flight status %Qv", status);
    }
    return result;
}

std::string FormatFlightStatus(FlightStatus status)
{
    // FLIGHT_STATUS_SCHEDULED -> SCHEDULED
    return FlightStatus_Name(status).substr(std::string_view("FLIGHT_STATUS_").size());
}

TErrorOr<ReservationStatus> ParseReservationStatus(const std::string& status)
{
    ReservationStatus result;
    if (!ReservationStatus_Parse("RESERVATION_STATUS_" + status, &result)) {
        return TError("Unknown reservation status %Qv", status);
    }
    return result;
}

TErrorOr<Flight> FlightFromRow(const TPgResult& result, int row)
{
    auto reader = result.Row(row);
    Flight flight;
    flight.set_id(reader.Get<i64>("id"));
    auto airlineCode = reader.Get<std::string>("airline_code");
    flight.set_airline_code(airlineCode);
    flight.set_airline_name(reader.Get<std::string>("airline_name"));
    flight.set_flight_number(airlineCode + ToString(reader.Get<int>("flight_no")));
    flight.set_origin(reader.Get<std::string>("origin"));
    flight.set_destination(reader.Get<std::string>("destination"));
    ToTimestamp(flight.mutable_departure_time(), reader.Get<i64>("departure_us"));
    ToTimestamp(flight.mutable_arrival_time(), reader.Get<i64>("arrival_us"));
    flight.set_total_seats(reader.Get<int>("total_seats"));
    flight.set_available_seats(reader.Get<int>("available_seats"));
    flight.mutable_price()->set_amount_minor(reader.Get<i64>("price_amount"));
    flight.mutable_price()->set_currency(reader.Get<std::string>("currency"));
    auto status = reader.Get<std::string>("status");
    SOA_RETURN_IF_ERROR(reader.GetError());

    SOA_ASSIGN_OR_RETURN(auto parsedStatus, ParseFlightStatus(status));
    flight.set_status(parsedStatus);
    return flight;
}

TErrorOr<TFlightRoute> RouteFromRow(const TPgResult& result, int row)
{
    auto reader = result.Row(row);
    TFlightRoute route{
        .FlightId = reader.Get<i64>("id"),
        .Origin = reader.Get<std::string>("origin"),
        .Destination = reader.Get<std::string>("destination"),
        .DepartureDate = reader.Get<std::string>("departure_date"),
    };
    SOA_RETURN_IF_ERROR(reader.GetError());
    return route;
}

TErrorOr<SeatReservation> ReservationFromRow(const TPgResult& result, int row)
{
    auto reader = result.Row(row);
    SeatReservation reservation;
    reservation.set_id(reader.Get<i64>("id"));
    reservation.set_flight_id(reader.Get<i64>("flight_id"));
    reservation.set_booking_id(reader.Get<std::string>("booking_id"));
    reservation.set_seat_count(reader.Get<int>("seat_count"));
    auto status = reader.Get<std::string>("status");
    ToTimestamp(reservation.mutable_created_at(), reader.Get<i64>("created_us"));
    ToTimestamp(reservation.mutable_updated_at(), reader.Get<i64>("updated_us"));
    if (auto expiresUs = reader.GetOptional<i64>("expires_us")) {
        ToTimestamp(reservation.mutable_expires_at(), *expiresUs);
    }
    if (auto confirmedUs = reader.GetOptional<i64>("confirmed_us")) {
        ToTimestamp(reservation.mutable_confirmed_at(), *confirmedUs);
    }
    SOA_RETURN_IF_ERROR(reader.GetError());

    SOA_ASSIGN_OR_RETURN(auto parsedStatus, ParseReservationStatus(status));
    reservation.set_status(parsedStatus);
    return reservation;
}

std::string SelectFlightSql(TStringBuf whereClause, TStringBuf suffix = {})
{
    return Format(
        "SELECT %v FROM flights f JOIN airlines a ON a.code = f.airline_code WHERE %v %v",
        FlightColumns,
        whereClause,
        suffix);
}

//! Переводит ошибки БД (недоступность, нарушение ограничений) в бизнес-ошибки;
//! бизнес-ошибки и OK возвращает как есть.
TError TranslateStorageError(const TError& error)
{
    if (error.IsOK() || FindBusinessErrorCode(error)) {
        return error;
    }
    if (error.FindMatching(NPg::EErrorCode::ConnectionError)) {
        return MakeError(EErrorCode::Unavailable, "Flight database is unavailable").With(error);
    }
    auto sqlState = GetSqlState(error);
    if (sqlState == CheckViolation || sqlState == ForeignKeyViolation) {
        return MakeError(EErrorCode::FailedPrecondition, "Data integrity constraint violated").With(error);
    }
    if (sqlState == SerializationFailure || sqlState == DeadlockDetected) {
        return MakeError(EErrorCode::Unavailable, "Concurrent update conflict, retry the request").With(error);
    }
    return MakeError(EErrorCode::Internal, "Flight storage error").With(error);
}

//! Публичные методы отдают наружу только бизнес-ошибки.
template <class T>
TErrorOr<T> Translate(TErrorOr<T> result)
{
    if (!result.IsOK()) {
        return TranslateStorageError(result);
    }
    return result;
}

////////////////////////////////////////////////////////////////////////////////

class TPgFlightRepository
    : public IFlightRepository
{
public:
    TPgFlightRepository(NPg::TPgClientPtr client, TDuration reservationTtl);

    TErrorOr<std::vector<Flight>> SearchFlights(
        const std::string& origin,
        const std::string& destination,
        const std::optional<TDate>& departureDate) override;

    TErrorOr<Flight> GetFlight(i64 flightId) override;

    TErrorOr<TReserveResult> ReserveSeats(i64 flightId, int seatCount, const std::string& bookingId) override;

    TErrorOr<SeatReservation> ConfirmReservation(const std::string& bookingId) override;

    TErrorOr<TReleaseResult> ReleaseReservation(const std::string& bookingId) override;

    TErrorOr<std::vector<TExpiredReservations>> ExpireReservations(int maxFlights) override;

    TErrorOr<TUpdateResult> CreateFlight(const TNewFlight& flight) override;
    TErrorOr<std::vector<Airport>> ListAirports() override;
    TErrorOr<std::vector<Airline>> ListAirlines() override;

    TErrorOr<TUpdateResult> UpdateFlight(
        i64 flightId,
        std::optional<FlightStatus> status,
        std::optional<Money> price) override;

private:
    const NPg::TPgClientPtr Client_;
    const TDuration ReservationTtl_;

    //! Истечение резерваций одного рейса; std::nullopt - рейс занят другой транзакцией.
    TErrorOr<std::optional<TExpiredReservations>> ExpireFlightReservations(i64 flightId);

    TErrorOr<std::optional<TReserveResult>> FindExistingReservation(
        TPgConnection& connection,
        i64 flightId,
        int seatCount,
        const std::string& bookingId);
    TErrorOr<TReserveResult> DoReserveSeats(i64 flightId, int seatCount, const std::string& bookingId);
};

TPgFlightRepository::TPgFlightRepository(NPg::TPgClientPtr client, TDuration reservationTtl)
    : Client_(std::move(client))
    , ReservationTtl_(reservationTtl)
{ }

TErrorOr<std::vector<Flight>> TPgFlightRepository::SearchFlights(
    const std::string& origin,
    const std::string& destination,
    const std::optional<TDate>& departureDate)
{
    return Translate([&] () -> TErrorOr<std::vector<Flight>> {
        SOA_ASSIGN_OR_RETURN(auto result, Client_->Execute(
            SelectFlightSql(
                // Только рейсы в продаже: SCHEDULED и ещё не вылетевшие.
                "f.origin = $1 AND f.destination = $2 AND f.status = 'SCHEDULED' AND f.departure_time > now() "
                "AND ($3::date IS NULL OR (f.departure_time AT TIME ZONE 'UTC')::date = $3::date)",
                "ORDER BY f.departure_time, f.id"),
            {PgParam(origin), PgParam(destination), PgParam(departureDate)}));

        std::vector<Flight> flights;
        flights.reserve(result.GetRowCount());
        for (int row = 0; row < result.GetRowCount(); ++row) {
            SOA_ASSIGN_OR_RETURN(auto flight, FlightFromRow(result, row));
            flights.push_back(std::move(flight));
        }
        return flights;
    }());
}

TErrorOr<Flight> TPgFlightRepository::GetFlight(i64 flightId)
{
    return Translate([&] () -> TErrorOr<Flight> {
        SOA_ASSIGN_OR_RETURN(auto result, Client_->Execute(SelectFlightSql("f.id = $1"), {PgParam(flightId)}));
        if (result.GetRowCount() == 0) {
            return MakeError(EErrorCode::NotFound, "Flight %v not found", flightId);
        }
        return FlightFromRow(result, 0);
    }());
}

TErrorOr<std::optional<TReserveResult>> TPgFlightRepository::FindExistingReservation(
    TPgConnection& connection,
    i64 flightId,
    int seatCount,
    const std::string& bookingId)
{
    SOA_ASSIGN_OR_RETURN(auto existing, connection.Execute(
        Format("SELECT %v, f.price_amount, f.currency, f.origin, f.destination, "
            "to_char(f.departure_time AT TIME ZONE 'UTC', 'YYYY-MM-DD') AS departure_date "
            "FROM seat_reservations r JOIN flights f ON f.id = r.flight_id "
            "WHERE r.booking_id = $1",
            ReservationColumns),
        {PgParam(bookingId)}));
    if (existing.GetRowCount() == 0) {
        return std::optional<TReserveResult>();
    }

    SOA_ASSIGN_OR_RETURN(auto reservation, ReservationFromRow(existing, 0));
    if (reservation.flight_id() != flightId || reservation.seat_count() != seatCount) {
        return MakeError(
            EErrorCode::AlreadyExists,
            "Booking %v already has a reservation with different parameters",
            bookingId)
            .With("existing_flight_id", reservation.flight_id())
            .With("existing_seat_count", reservation.seat_count());
    }
    if (reservation.status() != RESERVATION_STATUS_ACTIVE) {
        return MakeError(
            EErrorCode::FailedPrecondition,
            "Reservation for booking %v is %v and cannot be reused",
            bookingId,
            ReservationStatus_Name(reservation.status()));
    }

    auto reader = existing.Row(0);
    TReserveResult result;
    result.Reservation = std::move(reservation);
    result.SeatPrice.set_amount_minor(reader.Get<i64>("price_amount"));
    result.SeatPrice.set_currency(reader.Get<std::string>("currency"));
    result.Route = TFlightRoute{
        .FlightId = flightId,
        .Origin = reader.Get<std::string>("origin"),
        .Destination = reader.Get<std::string>("destination"),
        .DepartureDate = reader.Get<std::string>("departure_date"),
    };
    result.Replayed = true;
    SOA_RETURN_IF_ERROR(reader.GetError());
    return std::optional<TReserveResult>(std::move(result));
}

TErrorOr<TReserveResult> TPgFlightRepository::DoReserveSeats(i64 flightId, int seatCount, const std::string& bookingId)
{
    return Client_->WithTransaction([&] (TPgConnection& connection) -> TErrorOr<TReserveResult> {
        // Блокируем строку рейса: параллельные резервации на этот рейс ждут здесь,
        // поэтому проверка мест и их списание не могут "разъехаться".
        SOA_ASSIGN_OR_RETURN(auto flightResult, connection.Execute(
            SelectFlightSql("f.id = $1", "FOR UPDATE OF f"),
            {PgParam(flightId)}));
        if (flightResult.GetRowCount() == 0) {
            return MakeError(EErrorCode::NotFound, "Flight %v not found", flightId);
        }
        SOA_ASSIGN_OR_RETURN(auto flight, FlightFromRow(flightResult, 0));

        // Идемпотентность: повторный вызов с тем же booking_id возвращает существующую резервацию.
        SOA_ASSIGN_OR_RETURN(auto existing, FindExistingReservation(connection, flightId, seatCount, bookingId));
        if (existing) {
            return std::move(*existing);
        }

        if (flight.status() != FLIGHT_STATUS_SCHEDULED) {
            return MakeError(
                EErrorCode::FailedPrecondition,
                "Flight %v is %v, seats cannot be reserved",
                flightId,
                FormatFlightStatus(flight.status()));
        }
        // Статус рейса в БД может отставать от реальности (SCHEDULED в прошлом).
        if (TInstant::Seconds(flight.departure_time().seconds()) <= TInstant::Now()) {
            return MakeError(
                EErrorCode::FailedPrecondition,
                "Flight %v has already departed, seats cannot be reserved",
                flightId);
        }
        if (flight.available_seats() < seatCount) {
            return MakeError(
                EErrorCode::ResourceExhausted,
                "Not enough seats on flight %v: requested %v, available %v",
                flightId,
                seatCount,
                flight.available_seats());
        }

        SOA_RETURN_IF_ERROR(connection.Execute(
            "UPDATE flights SET available_seats = available_seats - $2 WHERE id = $1",
            {PgParam(flightId), PgParam(seatCount)}));
        SOA_ASSIGN_OR_RETURN(auto reservationResult, connection.Execute(
            Format("INSERT INTO seat_reservations AS r (flight_id, booking_id, seat_count, expires_at) "
                "VALUES ($1, $2, $3, now() + make_interval(secs => $4::double precision / 1000)) RETURNING %v",
                ReservationColumns),
            {PgParam(flightId), PgParam(bookingId), PgParam(seatCount), PgParam(ReservationTtl_.MilliSeconds())}));

        TReserveResult result;
        SOA_ASSIGN_OR_RETURN(result.Reservation, ReservationFromRow(reservationResult, 0));
        SOA_ASSIGN_OR_RETURN(result.Route, RouteFromRow(flightResult, 0));
        result.SeatPrice = flight.price();
        return result;
    });
}

TErrorOr<TReserveResult> TPgFlightRepository::ReserveSeats(i64 flightId, int seatCount, const std::string& bookingId)
{
    auto result = DoReserveSeats(flightId, seatCount, bookingId);
    if (!result.IsOK() && GetSqlState(result) == UniqueViolation) {
        // Тот же booking_id параллельно резервируется на другом рейсе: вторая
        // транзакция падает на UNIQUE(booking_id). Повторяем - и попадаем в ветку
        // идемпотентности с корректной ошибкой.
        YT_LOG_DEBUG("Concurrent reservation with the same booking id, retrying (BookingId: %v)", bookingId);
        result = DoReserveSeats(flightId, seatCount, bookingId);
    }
    return Translate(std::move(result));
}

TErrorOr<TReleaseResult> TPgFlightRepository::ReleaseReservation(const std::string& bookingId)
{
    return Translate(Client_->WithTransaction([&] (TPgConnection& connection) -> TErrorOr<TReleaseResult> {
        SOA_ASSIGN_OR_RETURN(auto lookup, connection.Execute(
            "SELECT flight_id FROM seat_reservations WHERE booking_id = $1",
            {PgParam(bookingId)}));
        if (lookup.GetRowCount() == 0) {
            return MakeError(EErrorCode::NotFound, "Reservation for booking %v not found", bookingId);
        }
        SOA_ASSIGN_OR_RETURN(auto flightId, lookup.Get<i64>(0, "flight_id"));

        // Порядок блокировок тот же, что в ReserveSeats: сначала рейс, потом резервация.
        SOA_ASSIGN_OR_RETURN(auto flightResult, connection.Execute(
            SelectFlightSql("f.id = $1", "FOR UPDATE OF f"),
            {PgParam(flightId)}));
        SOA_ASSIGN_OR_RETURN(auto reservationResult, connection.Execute(
            Format("SELECT %v FROM seat_reservations r WHERE r.booking_id = $1 FOR UPDATE", ReservationColumns),
            {PgParam(bookingId)}));

        TReleaseResult result;
        SOA_ASSIGN_OR_RETURN(result.Route, RouteFromRow(flightResult, 0));
        SOA_ASSIGN_OR_RETURN(result.Reservation, ReservationFromRow(reservationResult, 0));
        if (result.Reservation.status() != RESERVATION_STATUS_ACTIVE) {
            result.Replayed = true;
            return result;
        }

        SOA_RETURN_IF_ERROR(connection.Execute(
            "UPDATE flights SET available_seats = available_seats + $2 WHERE id = $1",
            {PgParam(flightId), PgParam(result.Reservation.seat_count())}));
        SOA_ASSIGN_OR_RETURN(auto updated, connection.Execute(
            Format("UPDATE seat_reservations AS r SET status = 'RELEASED', updated_at = now() "
                "WHERE r.booking_id = $1 RETURNING %v",
                ReservationColumns),
            {PgParam(bookingId)}));
        SOA_ASSIGN_OR_RETURN(result.Reservation, ReservationFromRow(updated, 0));
        return result;
    }));
}

TErrorOr<SeatReservation> TPgFlightRepository::ConfirmReservation(const std::string& bookingId)
{
    // Места не меняются, поэтому рейс не блокируем: достаточно строки резервации.
    // Фоновая задача берёт резервации с SKIP LOCKED и подтверждаемую не тронет.
    return Translate(Client_->WithTransaction([&] (TPgConnection& connection) -> TErrorOr<SeatReservation> {
        SOA_ASSIGN_OR_RETURN(auto locked, connection.Execute(
            Format("SELECT %v, (%v) AS expired FROM seat_reservations r WHERE r.booking_id = $1 FOR UPDATE",
                ReservationColumns,
                ExpiredReservationCondition),
            {PgParam(bookingId)}));
        if (locked.GetRowCount() == 0) {
            return MakeError(EErrorCode::NotFound, "Reservation for booking %v not found", bookingId);
        }
        SOA_ASSIGN_OR_RETURN(auto reservation, ReservationFromRow(locked, 0));
        SOA_ASSIGN_OR_RETURN(auto expired, locked.Get<bool>(0, "expired"));

        if (reservation.status() != RESERVATION_STATUS_ACTIVE) {
            return MakeError(
                EErrorCode::FailedPrecondition,
                "Reservation for booking %v is %v",
                bookingId,
                ReservationStatus_Name(reservation.status()));
        }
        if (reservation.has_confirmed_at()) {
            return reservation;
        }
        if (expired) {
            // Места вернёт фоновая задача.
            return MakeError(EErrorCode::FailedPrecondition, "Reservation for booking %v has expired", bookingId);
        }

        SOA_ASSIGN_OR_RETURN(auto confirmed, connection.Execute(
            Format("UPDATE seat_reservations AS r SET confirmed_at = now(), expires_at = NULL, updated_at = now() "
                "WHERE r.booking_id = $1 RETURNING %v",
                ReservationColumns),
            {PgParam(bookingId)}));
        return ReservationFromRow(confirmed, 0);
    }));
}

TErrorOr<std::optional<TExpiredReservations>> TPgFlightRepository::ExpireFlightReservations(i64 flightId)
{
    return Client_->WithTransaction([&] (TPgConnection& connection) -> TErrorOr<std::optional<TExpiredReservations>> {
        // Тот же порядок блокировок, что в ReserveSeats/ReleaseReservation: сначала рейс.
        // SKIP LOCKED: если рейс сейчас резервируют (или его обрабатывает другая реплика),
        // не ждём, а заберём его на следующем проходе.
        SOA_ASSIGN_OR_RETURN(auto flightResult, connection.Execute(
            SelectFlightSql("f.id = $1", "FOR UPDATE OF f SKIP LOCKED"),
            {PgParam(flightId)}));
        if (flightResult.GetRowCount() == 0) {
            return std::optional<TExpiredReservations>();
        }

        SOA_ASSIGN_OR_RETURN(auto expired, connection.Execute(
            Format("WITH expired AS ("
                "  SELECT r.id FROM seat_reservations r"
                "  WHERE r.flight_id = $1 AND %v"
                "  FOR UPDATE SKIP LOCKED"
                ") "
                "UPDATE seat_reservations r SET status = 'EXPIRED', updated_at = now() "
                "FROM expired e WHERE r.id = e.id "
                "RETURNING r.booking_id::text AS booking_id, r.seat_count",
                ExpiredReservationCondition),
            {PgParam(flightId)}));
        if (expired.GetRowCount() == 0) {
            return std::optional<TExpiredReservations>();
        }

        TExpiredReservations result;
        SOA_ASSIGN_OR_RETURN(result.Route, RouteFromRow(flightResult, 0));
        for (int row = 0; row < expired.GetRowCount(); ++row) {
            auto reader = expired.Row(row);
            result.BookingIds.push_back(reader.Get<std::string>("booking_id"));
            result.SeatCount += reader.Get<int>("seat_count");
            SOA_RETURN_IF_ERROR(reader.GetError());
        }

        SOA_RETURN_IF_ERROR(connection.Execute(
            "UPDATE flights SET available_seats = available_seats + $2 WHERE id = $1",
            {PgParam(flightId), PgParam(result.SeatCount)}));
        return std::optional<TExpiredReservations>(std::move(result));
    });
}

TErrorOr<std::vector<TExpiredReservations>> TPgFlightRepository::ExpireReservations(int maxFlights)
{
    return Translate([&] () -> TErrorOr<std::vector<TExpiredReservations>> {
        // Кандидаты без блокировок; каждый рейс - отдельная короткая транзакция.
        SOA_ASSIGN_OR_RETURN(auto candidates, Client_->Execute(
            Format("SELECT DISTINCT r.flight_id FROM seat_reservations r WHERE %v LIMIT $1",
                ExpiredReservationCondition),
            {PgParam(maxFlights)}));

        std::vector<TExpiredReservations> result;
        for (int row = 0; row < candidates.GetRowCount(); ++row) {
            SOA_ASSIGN_OR_RETURN(auto flightId, candidates.Get<i64>(row, "flight_id"));
            SOA_ASSIGN_OR_RETURN(auto expired, ExpireFlightReservations(flightId));
            if (expired) {
                result.push_back(std::move(*expired));
            }
        }
        return result;
    }());
}

TErrorOr<TUpdateResult> TPgFlightRepository::CreateFlight(const TNewFlight& flight)
{
    auto result = [&] () -> TErrorOr<TUpdateResult> {
        SOA_ASSIGN_OR_RETURN(auto inserted, Client_->Execute(
            "INSERT INTO flights (airline_code, flight_no, origin, destination, departure_time, arrival_time, "
            "total_seats, available_seats, price_amount, currency) "
            "VALUES ($1, $2, $3, $4, to_timestamp($5::bigint / 1000000.0), to_timestamp($6::bigint / 1000000.0), "
            "$7, $7, $8, $9) RETURNING id",
            {
                PgParam(flight.AirlineCode),
                PgParam(flight.FlightNo),
                PgParam(flight.Origin),
                PgParam(flight.Destination),
                PgParam(flight.DepartureTime.MicroSeconds()),
                PgParam(flight.ArrivalTime.MicroSeconds()),
                PgParam(flight.TotalSeats),
                PgParam(flight.Price.amount_minor()),
                PgParam(std::string(flight.Price.currency())),
            }));
        SOA_ASSIGN_OR_RETURN(auto flightId, inserted.Get<i64>(0, "id"));

        SOA_ASSIGN_OR_RETURN(auto created, Client_->Execute(SelectFlightSql("f.id = $1"), {PgParam(flightId)}));
        TUpdateResult result;
        SOA_ASSIGN_OR_RETURN(result.Flight, FlightFromRow(created, 0));
        SOA_ASSIGN_OR_RETURN(result.Route, RouteFromRow(created, 0));
        return result;
    }();

    if (!result.IsOK()) {
        // Ограничения БД здесь - ошибки ввода администратора, а не сбой хранилища.
        auto sqlState = GetSqlState(result);
        if (sqlState == ForeignKeyViolation) {
            return MakeError(EErrorCode::InvalidArgument, "Unknown airline or airport code").With(TError(result));
        }
        if (sqlState == UniqueViolation) {
            return MakeError(
                EErrorCode::AlreadyExists,
                "Flight %v%v already exists on %v",
                flight.AirlineCode,
                flight.FlightNo,
                flight.DepartureTime.FormatGmTime("%Y-%m-%d"));
        }
        if (sqlState == CheckViolation) {
            return MakeError(EErrorCode::InvalidArgument, "Flight data violates constraints").With(TError(result));
        }
    }
    return Translate(std::move(result));
}

TErrorOr<std::vector<Airport>> TPgFlightRepository::ListAirports()
{
    return Translate([&] () -> TErrorOr<std::vector<Airport>> {
        SOA_ASSIGN_OR_RETURN(auto result, Client_->Execute("SELECT iata_code, name, city FROM airports ORDER BY city, iata_code"));
        std::vector<Airport> airports;
        for (int row = 0; row < result.GetRowCount(); ++row) {
            auto reader = result.Row(row);
            Airport airport;
            airport.set_iata_code(reader.Get<std::string>("iata_code"));
            airport.set_name(reader.Get<std::string>("name"));
            airport.set_city(reader.Get<std::string>("city"));
            SOA_RETURN_IF_ERROR(reader.GetError());
            airports.push_back(std::move(airport));
        }
        return airports;
    }());
}

TErrorOr<std::vector<Airline>> TPgFlightRepository::ListAirlines()
{
    return Translate([&] () -> TErrorOr<std::vector<Airline>> {
        SOA_ASSIGN_OR_RETURN(auto result, Client_->Execute("SELECT code, name FROM airlines ORDER BY name"));
        std::vector<Airline> airlines;
        for (int row = 0; row < result.GetRowCount(); ++row) {
            auto reader = result.Row(row);
            Airline airline;
            airline.set_code(reader.Get<std::string>("code"));
            airline.set_name(reader.Get<std::string>("name"));
            SOA_RETURN_IF_ERROR(reader.GetError());
            airlines.push_back(std::move(airline));
        }
        return airlines;
    }());
}

TErrorOr<TUpdateResult> TPgFlightRepository::UpdateFlight(
    i64 flightId,
    std::optional<FlightStatus> status,
    std::optional<Money> price)
{
    std::optional<std::string> statusParam;
    if (status) {
        statusParam = FormatFlightStatus(*status);
    }
    std::optional<i64> priceAmount;
    std::optional<std::string> currency;
    if (price) {
        priceAmount = price->amount_minor();
        currency = price->currency();
    }

    return Translate(Client_->WithTransaction([&] (TPgConnection& connection) -> TErrorOr<TUpdateResult> {
        SOA_ASSIGN_OR_RETURN(auto updated, connection.Execute(
            "UPDATE flights SET "
            "status = COALESCE($2::flight_status, status), "
            "price_amount = COALESCE($3::bigint, price_amount), "
            "currency = COALESCE($4, currency) "
            "WHERE id = $1",
            {PgParam(flightId), PgParam(statusParam), PgParam(priceAmount), PgParam(currency)}));
        if (updated.GetAffectedRows() == 0) {
            return MakeError(EErrorCode::NotFound, "Flight %v not found", flightId);
        }

        SOA_ASSIGN_OR_RETURN(auto result, connection.Execute(SelectFlightSql("f.id = $1"), {PgParam(flightId)}));
        TUpdateResult update;
        SOA_ASSIGN_OR_RETURN(update.Flight, FlightFromRow(result, 0));
        SOA_ASSIGN_OR_RETURN(update.Route, RouteFromRow(result, 0));
        return update;
    }));
}

} // namespace

////////////////////////////////////////////////////////////////////////////////

IFlightRepositoryPtr CreatePgFlightRepository(NPg::TPgClientPtr client, TDuration reservationTtl)
{
    return New<TPgFlightRepository>(std::move(client), reservationTtl);
}

////////////////////////////////////////////////////////////////////////////////

} // namespace NSoa::NFlight
