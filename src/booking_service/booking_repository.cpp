#include "booking_repository.h"

#include <soa/lib/common/error_macros.h>
#include <soa/lib/common/errors.h>

namespace NSoa::NBooking {

using namespace NYT;

using NPg::PgParam;
using NPg::TPgConnection;
using NPg::TPgResult;
using NSoa::EErrorCode;

////////////////////////////////////////////////////////////////////////////////

namespace {

constexpr TStringBuf BookingColumns = R"sql(
    id::text AS id,
    user_id,
    flight_id,
    passenger_name,
    passenger_email,
    seat_count,
    total_price_amount,
    currency,
    status::text AS status,
    (extract(epoch FROM created_at) * 1000000)::bigint AS created_us,
    (extract(epoch FROM updated_at) * 1000000)::bigint AS updated_us
)sql";

} // namespace

std::string FormatBookingStatus(EBookingStatus status)
{
    return status == EBookingStatus::Confirmed ? "CONFIRMED" : "CANCELLED";
}

namespace {

TErrorOr<TBooking> BookingFromRow(const TPgResult& result, int row)
{
    auto reader = result.Row(row);
    TBooking booking{
        .Id = reader.Get<std::string>("id"),
        .UserId = reader.Get<std::string>("user_id"),
        .FlightId = reader.Get<i64>("flight_id"),
        .PassengerName = reader.Get<std::string>("passenger_name"),
        .PassengerEmail = reader.Get<std::string>("passenger_email"),
        .SeatCount = reader.Get<int>("seat_count"),
        .TotalPriceAmount = reader.Get<i64>("total_price_amount"),
        .Currency = reader.Get<std::string>("currency"),
        .CreatedAt = TInstant::MicroSeconds(reader.Get<i64>("created_us")),
        .UpdatedAt = TInstant::MicroSeconds(reader.Get<i64>("updated_us")),
    };
    auto status = reader.Get<std::string>("status");
    SOA_RETURN_IF_ERROR(reader.GetError());

    if (status == "CONFIRMED") {
        booking.Status = EBookingStatus::Confirmed;
    } else if (status == "CANCELLED") {
        booking.Status = EBookingStatus::Cancelled;
    } else {
        return TError("Unknown booking status %Qv", status);
    }
    return booking;
}

//! Наружу - только бизнес-ошибки.
TError TranslateStorageError(const TError& error)
{
    if (error.IsOK() || FindBusinessErrorCode(error)) {
        return error;
    }
    if (error.FindMatching(NPg::EErrorCode::ConnectionError)) {
        return MakeError(EErrorCode::Unavailable, "Booking database is unavailable").With(error);
    }
    return MakeError(EErrorCode::Internal, "Booking storage error").With(error);
}

template <class T>
TErrorOr<T> Translate(TErrorOr<T> result)
{
    if (!result.IsOK()) {
        return TranslateStorageError(result);
    }
    return result;
}

} // namespace

////////////////////////////////////////////////////////////////////////////////

TBookingRepository::TBookingRepository(NPg::TPgClientPtr client)
    : Client_(std::move(client))
{ }

TErrorOr<TBooking> TBookingRepository::Create(
    const TNewBooking& booking,
    const std::function<TError(const TBooking&)>& beforeCommit)
{
    return Translate(Client_->WithTransaction([&] (TPgConnection& connection) -> TErrorOr<TBooking> {
        SOA_ASSIGN_OR_RETURN(auto result, connection.Execute(
            Format("INSERT INTO bookings (id, user_id, flight_id, passenger_name, passenger_email, "
                "seat_count, total_price_amount, currency) "
                "VALUES ($1, $2, $3, $4, $5, $6, $7, $8) RETURNING %v",
                BookingColumns),
            {
                PgParam(booking.Id),
                PgParam(booking.UserId),
                PgParam(booking.FlightId),
                PgParam(booking.PassengerName),
                PgParam(booking.PassengerEmail),
                PgParam(booking.SeatCount),
                PgParam(booking.TotalPriceAmount),
                PgParam(booking.Currency),
            }));
        SOA_ASSIGN_OR_RETURN(auto created, BookingFromRow(result, 0));
        SOA_RETURN_IF_ERROR(beforeCommit(created));
        return created;
    }));
}

TErrorOr<TBooking> TBookingRepository::Get(const std::string& bookingId)
{
    return Translate([&] () -> TErrorOr<TBooking> {
        SOA_ASSIGN_OR_RETURN(auto result, Client_->Execute(
            Format("SELECT %v FROM bookings WHERE id = $1", BookingColumns),
            {PgParam(bookingId)}));
        if (result.GetRowCount() == 0) {
            return MakeError(EErrorCode::NotFound, "Booking %v not found", bookingId);
        }
        return BookingFromRow(result, 0);
    }());
}

TErrorOr<std::vector<TBooking>> TBookingRepository::ListByUser(const std::string& userId)
{
    return Translate([&] () -> TErrorOr<std::vector<TBooking>> {
        SOA_ASSIGN_OR_RETURN(auto result, Client_->Execute(
            Format("SELECT %v FROM bookings WHERE user_id = $1 ORDER BY created_at DESC, id", BookingColumns),
            {PgParam(userId)}));
        std::vector<TBooking> bookings;
        bookings.reserve(result.GetRowCount());
        for (int row = 0; row < result.GetRowCount(); ++row) {
            SOA_ASSIGN_OR_RETURN(auto booking, BookingFromRow(result, row));
            bookings.push_back(std::move(booking));
        }
        return bookings;
    }());
}

TErrorOr<TBooking> TBookingRepository::Cancel(
    const std::string& bookingId,
    const std::function<TError(const TBooking&)>& beforeCancel)
{
    return Translate(Client_->WithTransaction([&] (TPgConnection& connection) -> TErrorOr<TBooking> {
        // Блокировка строки не даёт двум параллельным отменам дважды вернуть места.
        SOA_ASSIGN_OR_RETURN(auto locked, connection.Execute(
            Format("SELECT %v FROM bookings WHERE id = $1 FOR UPDATE", BookingColumns),
            {PgParam(bookingId)}));
        if (locked.GetRowCount() == 0) {
            return MakeError(EErrorCode::NotFound, "Booking %v not found", bookingId);
        }
        SOA_ASSIGN_OR_RETURN(auto booking, BookingFromRow(locked, 0));
        if (booking.Status != EBookingStatus::Confirmed) {
            return MakeError(EErrorCode::FailedPrecondition, "Booking %v is already %v", bookingId, FormatBookingStatus(booking.Status));
        }

        SOA_RETURN_IF_ERROR(beforeCancel(booking));

        SOA_ASSIGN_OR_RETURN(auto updated, connection.Execute(
            Format("UPDATE bookings SET status = 'CANCELLED', updated_at = now() WHERE id = $1 RETURNING %v",
                BookingColumns),
            {PgParam(bookingId)}));
        return BookingFromRow(updated, 0);
    }));
}

////////////////////////////////////////////////////////////////////////////////

} // namespace NSoa::NBooking
