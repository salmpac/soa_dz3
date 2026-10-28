#include "booking_service.h"

#include <soa/lib/common/error_macros.h>
#include <soa/lib/common/errors.h>

#include <yt/yt/core/crypto/crypto.h>

#include <yt/yt/core/logging/log.h>

#include <contrib/libs/re2/re2/re2.h>

namespace NSoa::NBooking {

using namespace NYT;
using namespace flight::v1;
using NSoa::EErrorCode;

////////////////////////////////////////////////////////////////////////////////

YT_DEFINE_LEAKY_GLOBAL(const NLogging::TLogger, BookingServiceLogger, "BookingService");
constinit const auto Logger = BookingServiceLogger;

static constexpr int MaxSeatsPerBooking = 50;
static constexpr size_t MaxUserIdLength = 64;
static constexpr size_t MaxPassengerNameLength = 200;
static constexpr size_t MaxEmailLength = 254;

////////////////////////////////////////////////////////////////////////////////

namespace {

//! UUID v4 из криптостойкого генератора.
TErrorOr<std::string> GenerateBookingId()
{
    // GenerateCryptoStrongRandomString сообщает о сбое RAND_bytes исключением.
    SOA_ASSIGN_OR_RETURN(auto bytes, CatchErrors([] {
        return NCrypto::GenerateCryptoStrongRandomString(16);
    }));
    bytes[6] = static_cast<char>((bytes[6] & 0x0f) | 0x40);  // версия 4
    bytes[8] = static_cast<char>((bytes[8] & 0x3f) | 0x80);  // вариант RFC 4122

    static constexpr char Hex[] = "0123456789abcdef";
    std::string result;
    result.reserve(36);
    for (int index = 0; index < 16; ++index) {
        if (index == 4 || index == 6 || index == 8 || index == 10) {
            result.push_back('-');
        }
        auto byte = static_cast<unsigned char>(bytes[index]);
        result.push_back(Hex[byte >> 4]);
        result.push_back(Hex[byte & 0x0f]);
    }
    return result;
}

bool IsUuid(const std::string& value)
{
    static const re2::RE2 UuidRegex("[0-9a-fA-F]{8}-[0-9a-fA-F]{4}-[0-9a-fA-F]{4}-[0-9a-fA-F]{4}-[0-9a-fA-F]{12}");
    return re2::RE2::FullMatch(value, UuidRegex);
}

TError ValidateBookingId(const std::string& bookingId)
{
    // Невалидный id не может существовать: отвечаем 404, а не ошибкой БД.
    if (!IsUuid(bookingId)) {
        return MakeError(EErrorCode::NotFound, "Booking %v not found", bookingId);
    }
    return {};
}

TError ValidateCreateRequest(const TCreateBookingRequest& request)
{
    static const re2::RE2 EmailRegex(R"([^@\s]+@[^@\s]+\.[^@\s]+)");

    if (request.UserId.empty() || request.UserId.size() > MaxUserIdLength) {
        return MakeError(EErrorCode::InvalidArgument, "Field user_id must be a non-empty string up to %v characters", MaxUserIdLength);
    }
    if (request.FlightId <= 0) {
        return MakeError(EErrorCode::InvalidArgument, "Field flight_id must be positive");
    }
    if (request.PassengerName.empty() || request.PassengerName.size() > MaxPassengerNameLength) {
        return MakeError(EErrorCode::InvalidArgument, "Field passenger_name must be a non-empty string up to %v characters", MaxPassengerNameLength);
    }
    if (request.PassengerEmail.size() > MaxEmailLength || !re2::RE2::FullMatch(request.PassengerEmail, EmailRegex)) {
        return MakeError(EErrorCode::InvalidArgument, "Field passenger_email must be a valid email address");
    }
    if (request.SeatCount <= 0 || request.SeatCount > MaxSeatsPerBooking) {
        return MakeError(EErrorCode::InvalidArgument, "Field seat_count must be between 1 and %v", MaxSeatsPerBooking);
    }
    return {};
}

} // namespace

////////////////////////////////////////////////////////////////////////////////

TBookingService::TBookingService(TFlightClientPtr flightClient, TBookingRepository* repository)
    : FlightClient_(std::move(flightClient))
    , Repository_(repository)
{ }

TErrorOr<std::vector<Flight>> TBookingService::SearchFlights(
    const std::string& origin,
    const std::string& destination,
    const std::optional<Date>& departureDate)
{
    return FlightClient_->SearchFlights(origin, destination, departureDate);
}

TErrorOr<Flight> TBookingService::GetFlight(i64 flightId)
{
    if (flightId <= 0) {
        return MakeError(EErrorCode::NotFound, "Flight %v not found", flightId);
    }
    return FlightClient_->GetFlight(flightId);
}

TErrorOr<TBooking> TBookingService::CreateBooking(const TCreateBookingRequest& request)
{
    SOA_RETURN_IF_ERROR(ValidateCreateRequest(request));
    // id генерируем до вызова Flight Service: он же ключ идемпотентности ReserveSeats,
    // поэтому повтор после таймаута не создаст вторую резервацию.
    SOA_ASSIGN_OR_RETURN(auto bookingId, GenerateBookingId());

    auto Logger = BookingServiceLogger().WithTag("BookingId", bookingId);
    YT_LOG_INFO("Creating booking (UserId: %v, FlightId: %v, SeatCount: %v)",
        request.UserId,
        request.FlightId,
        request.SeatCount);

    // 1. Рейс и его цена.
    SOA_ASSIGN_OR_RETURN(auto flight, FlightClient_->GetFlight(request.FlightId));

    // 2. Атомарное резервирование мест.
    auto reservationOrError = FlightClient_->ReserveSeats(request.FlightId, request.SeatCount, bookingId);
    if (!reservationOrError.IsOK()) {
        // Бизнес-отказ (нет мест, рейс отменён) - резервации точно нет. Сбой транспорта -
        // резервация могла создаться, а ответ потеряться: отменяем её на всякий случай.
        if (!FindBusinessErrorCode(reservationOrError)) {
            CompensateReservation(bookingId);
        }
        YT_LOG_INFO(reservationOrError, "Seat reservation failed, booking is not created");
        return TError(reservationOrError);
    }

    // 3. Фиксируем цену на момент бронирования.
    i64 totalPrice;
    if (__builtin_mul_overflow(static_cast<i64>(request.SeatCount), flight.price().amount_minor(), &totalPrice)) {
        CompensateReservation(bookingId);
        return MakeError(EErrorCode::InvalidArgument, "Total price overflow");
    }

    // 4. Запись бронирования и подтверждение резервации - одна транзакция Booking DB:
    //    подтверждение вызывается до COMMIT. Если что-то упадёт (в т.ч. процесс),
    //    бронирования не будет, а неподтверждённая резервация истечёт по TTL.
    auto bookingOrError = Repository_->Create(
        TNewBooking{
            .Id = bookingId,
            .UserId = request.UserId,
            .FlightId = request.FlightId,
            .PassengerName = request.PassengerName,
            .PassengerEmail = request.PassengerEmail,
            .SeatCount = request.SeatCount,
            .TotalPriceAmount = totalPrice,
            .Currency = flight.price().currency(),
        },
        [&] (const TBooking& /*booking*/) -> TError {
            SOA_RETURN_IF_ERROR(FlightClient_->ConfirmReservation(bookingId));
            return {};
        });
    if (!bookingOrError.IsOK()) {
        // Освобождаем места сразу, не дожидаясь TTL (если не выйдет - освободит TTL).
        YT_LOG_WARNING(bookingOrError, "Failed to store booking, releasing reservation");
        CompensateReservation(bookingId);
        return bookingOrError;
    }

    YT_LOG_INFO("Booking created (TotalPrice: %v %v, ReservationId: %v)",
        totalPrice,
        flight.price().currency(),
        reservationOrError.Value().reservation().id());
    return bookingOrError;
}

void TBookingService::CompensateReservation(const std::string& bookingId)
{
    auto releaseOrError = FlightClient_->ReleaseReservation(bookingId);
    if (releaseOrError.IsOK() || FindBusinessErrorCode(releaseOrError) == EErrorCode::NotFound) {
        YT_LOG_INFO("Reservation compensated (BookingId: %v)", bookingId);
        return;
    }
    // Неподтверждённую резервацию освободит фоновая задача Flight Service по TTL.
    YT_LOG_WARNING(releaseOrError, "Failed to compensate reservation, it will expire by TTL (BookingId: %v)", bookingId);
}

TErrorOr<TBooking> TBookingService::GetBooking(const std::string& bookingId)
{
    SOA_RETURN_IF_ERROR(ValidateBookingId(bookingId));
    return Repository_->Get(bookingId);
}

TErrorOr<std::vector<TBooking>> TBookingService::ListBookings(const std::string& userId)
{
    if (userId.empty() || userId.size() > MaxUserIdLength) {
        return MakeError(EErrorCode::InvalidArgument, "Query parameter user_id is required");
    }
    return Repository_->ListByUser(userId);
}

TErrorOr<TBooking> TBookingService::CancelBooking(const std::string& bookingId)
{
    SOA_RETURN_IF_ERROR(ValidateBookingId(bookingId));

    // Возврат мест - внутри транзакции с заблокированной строкой бронирования:
    // если Flight Service недоступен, статус не меняется и отмену можно повторить
    // (ReleaseReservation идемпотентен).
    auto bookingOrError = Repository_->Cancel(bookingId, [&] (const TBooking& booking) -> TError {
        SOA_RETURN_IF_ERROR(FlightClient_->ReleaseReservation(booking.Id));
        return {};
    });
    if (bookingOrError.IsOK()) {
        YT_LOG_INFO("Booking cancelled (BookingId: %v)", bookingId);
    }
    return bookingOrError;
}

////////////////////////////////////////////////////////////////////////////////

} // namespace NSoa::NBooking
