#pragma once

#include "booking_service.h"

#include <yt/yt/core/http/public.h>

namespace NSoa::NBooking {

////////////////////////////////////////////////////////////////////////////////

//! REST API Booking Service:
//!   GET  /flights?origin=SVO&destination=LED[&date=2026-04-01]
//!   GET  /flights/{id}
//!   POST /bookings                 {user_id, flight_id, passenger_name, passenger_email, seat_count}
//!   GET  /bookings?user_id=X
//!   GET  /bookings/{id}
//!   POST /bookings/{id}/cancel
//!
//! Ошибки: {"error": {"code": "NOT_FOUND", "message": "..."}} с HTTP-статусом
//! по коду ошибки (400/404/409/502/503/500).
//!
//! Администрирование (заголовок X-Admin-Token == #adminToken; пустой токен - админка выключена):
//!   GET  /admin/airports
//!   GET  /admin/airlines
//!   POST /admin/flights            {airline_code, flight_no, origin, destination,
//!                                   departure_time, arrival_time, total_seats, price}
//!   POST /admin/flights/{id}       {status?, price?}
void RegisterHttpHandlers(
    const NYT::NHttp::IServerPtr& server,
    TBookingService* service,
    TFlightClientPtr flightClient,
    std::string adminToken);

////////////////////////////////////////////////////////////////////////////////

} // namespace NSoa::NBooking
