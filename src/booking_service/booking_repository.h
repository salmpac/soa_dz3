#pragma once

#include <soa/lib/pg/client.h>

#include <library/cpp/yt/error/error.h>
#include <library/cpp/yt/misc/enum.h>

#include <util/datetime/base.h>

#include <functional>
#include <string>
#include <vector>

namespace NSoa::NBooking {

////////////////////////////////////////////////////////////////////////////////

DEFINE_ENUM(EBookingStatus,
    (Confirmed)
    (Cancelled)
);

//! CONFIRMED / CANCELLED - как в БД и в REST API.
std::string FormatBookingStatus(EBookingStatus status);

struct TBooking
{
    std::string Id;
    std::string UserId;
    i64 FlightId = 0;
    std::string PassengerName;
    std::string PassengerEmail;
    int SeatCount = 0;
    //! Цена, зафиксированная при бронировании, в минимальных единицах валюты.
    i64 TotalPriceAmount = 0;
    std::string Currency;
    EBookingStatus Status = EBookingStatus::Confirmed;
    TInstant CreatedAt;
    TInstant UpdatedAt;
};

struct TNewBooking
{
    std::string Id;
    std::string UserId;
    i64 FlightId = 0;
    std::string PassengerName;
    std::string PassengerEmail;
    int SeatCount = 0;
    i64 TotalPriceAmount = 0;
    std::string Currency;
};

//! Бронирования в PostgreSQL Booking Service.
/*!
 *  Ошибки возвращаются с кодами из soa/lib/common/errors.h;
 *  недоступность БД - Unavailable.
 */
class TBookingRepository
{
public:
    explicit TBookingRepository(NPg::TPgClientPtr client);

    //! В одной транзакции: INSERT бронирования, затем #beforeCommit (подтверждение
    //! резервации во Flight Service), затем COMMIT. Если #beforeCommit вернул
    //! ошибку - бронирование не создаётся.
    NYT::TErrorOr<TBooking> Create(
        const TNewBooking& booking,
        const std::function<NYT::TError(const TBooking&)>& beforeCommit);
    NYT::TErrorOr<TBooking> Get(const std::string& bookingId);
    NYT::TErrorOr<std::vector<TBooking>> ListByUser(const std::string& userId);

    //! В одной транзакции: блокирует бронирование (SELECT FOR UPDATE), проверяет,
    //! что оно CONFIRMED, вызывает #beforeCancel (возврат мест во Flight Service)
    //! и ставит CANCELLED. Если #beforeCancel вернул ошибку - ничего не меняется.
    NYT::TErrorOr<TBooking> Cancel(
        const std::string& bookingId,
        const std::function<NYT::TError(const TBooking&)>& beforeCancel);

private:
    const NPg::TPgClientPtr Client_;
};

////////////////////////////////////////////////////////////////////////////////

} // namespace NSoa::NBooking
