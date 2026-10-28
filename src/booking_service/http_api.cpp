#include "http_api.h"

#include <soa/lib/common/error_macros.h>
#include <soa/lib/common/errors.h>

#include <yt/yt/core/concurrency/scheduler_api.h>

#include <yt/yt/core/crypto/crypto.h>

#include <yt/yt/core/http/http.h>
#include <yt/yt/core/http/server.h>

#include <yt/yt/core/logging/log.h>

#include <library/cpp/cgiparam/cgiparam.h>
#include <library/cpp/json/json_reader.h>
#include <library/cpp/json/json_writer.h>

#include <util/string/ascii.h>
#include <util/string/split.h>

namespace NSoa::NBooking {

using namespace NYT;
using namespace NYT::NConcurrency;
using namespace NYT::NHttp;
using namespace flight::v1;
using NSoa::EErrorCode;

////////////////////////////////////////////////////////////////////////////////

YT_DEFINE_LEAKY_GLOBAL(const NLogging::TLogger, HttpApiLogger, "HttpApi");
constinit const auto Logger = HttpApiLogger;

static constexpr size_t MaxRequestBodySize = 64 * 1024;

////////////////////////////////////////////////////////////////////////////////

namespace {

struct TJsonResponse
{
    EStatusCode Status = EStatusCode::OK;
    NJson::TJsonValue Body;
};

using THandler = std::function<TErrorOr<TJsonResponse>(const IRequestPtr& request, const std::vector<TStringBuf>& path)>;

////////////////////////////////////////////////////////////////////////////////
// Ошибки -> HTTP.

struct THttpError
{
    EStatusCode Status;
    TStringBuf Code;
};

THttpError ToHttpError(const TError& error)
{
    if (auto businessCode = FindBusinessErrorCode(error)) {
        switch (*businessCode) {
            case EErrorCode::InvalidArgument:    return {EStatusCode::BadRequest, "INVALID_ARGUMENT"};
            case EErrorCode::NotFound:           return {EStatusCode::NotFound, "NOT_FOUND"};
            case EErrorCode::ResourceExhausted:  return {EStatusCode::Conflict, "NOT_ENOUGH_SEATS"};
            case EErrorCode::FailedPrecondition: return {EStatusCode::Conflict, "FAILED_PRECONDITION"};
            case EErrorCode::AlreadyExists:      return {EStatusCode::Conflict, "ALREADY_EXISTS"};
            // Flight Service не принял наш ключ - это наша проблема конфигурации.
            case EErrorCode::Unauthenticated:    return {EStatusCode::BadGateway, "UPSTREAM_AUTH_FAILED"};
            case EErrorCode::CircuitOpen:        return {EStatusCode::ServiceUnavailable, "FLIGHT_SERVICE_UNAVAILABLE"};
            case EErrorCode::Unavailable:        return {EStatusCode::ServiceUnavailable, "SERVICE_UNAVAILABLE"};
            case EErrorCode::Internal:           return {EStatusCode::InternalServerError, "INTERNAL"};
        }
    }
    // Транспорт/таймаут до Flight Service после всех повторов.
    if (error.FindMatching(NYT::EErrorCode::Timeout)) {
        return {EStatusCode::GatewayTimeout, "FLIGHT_SERVICE_TIMEOUT"};
    }
    if (error.FindMatching(THashSet<TErrorCode>{NYT::NRpc::EErrorCode::TransportError, NYT::NRpc::EErrorCode::Unavailable})) {
        return {EStatusCode::ServiceUnavailable, "FLIGHT_SERVICE_UNAVAILABLE"};
    }
    return {EStatusCode::InternalServerError, "INTERNAL"};
}

//! Сообщение для клиента: текст первой бизнес-ошибки в цепочке (без служебных подробностей YT).
std::string ToClientMessage(const TError& error)
{
    if (auto businessError = FindBusinessError(error)) {
        return businessError->GetMessage();
    }
    return error.GetMessage();
}

NJson::TJsonValue MakeErrorBody(TStringBuf code, const std::string& message)
{
    NJson::TJsonValue error;
    error["code"] = code;
    error["message"] = message;
    NJson::TJsonValue body;
    body["error"] = std::move(error);
    return body;
}

TError BadRequest(TStringBuf message)
{
    return MakeError(EErrorCode::InvalidArgument, "%v", message);
}

////////////////////////////////////////////////////////////////////////////////
// JSON.

std::string FormatTimestamp(const google::protobuf::Timestamp& timestamp)
{
    return std::string(TInstant::Seconds(timestamp.seconds()).ToStringUpToSeconds());
}

std::string FormatFlightStatus(FlightStatus status)
{
    // FLIGHT_STATUS_SCHEDULED -> SCHEDULED
    return FlightStatus_Name(status).substr(std::string_view("FLIGHT_STATUS_").size());
}

NJson::TJsonValue MoneyToJson(i64 amountMinor, const std::string& currency)
{
    NJson::TJsonValue json;
    json["amount_minor"] = amountMinor;
    json["currency"] = currency;
    return json;
}

NJson::TJsonValue FlightToJson(const Flight& flight)
{
    NJson::TJsonValue json;
    json["id"] = flight.id();
    json["flight_number"] = flight.flight_number();
    json["airline_code"] = flight.airline_code();
    json["airline_name"] = flight.airline_name();
    json["origin"] = flight.origin();
    json["destination"] = flight.destination();
    json["departure_time"] = FormatTimestamp(flight.departure_time());
    json["arrival_time"] = FormatTimestamp(flight.arrival_time());
    json["total_seats"] = flight.total_seats();
    json["available_seats"] = flight.available_seats();
    json["price"] = MoneyToJson(flight.price().amount_minor(), flight.price().currency());
    json["status"] = FormatFlightStatus(flight.status());
    return json;
}

NJson::TJsonValue BookingToJson(const TBooking& booking)
{
    NJson::TJsonValue json;
    json["id"] = booking.Id;
    json["user_id"] = booking.UserId;
    json["flight_id"] = booking.FlightId;
    json["passenger_name"] = booking.PassengerName;
    json["passenger_email"] = booking.PassengerEmail;
    json["seat_count"] = booking.SeatCount;
    json["total_price"] = MoneyToJson(booking.TotalPriceAmount, booking.Currency);
    json["status"] = FormatBookingStatus(booking.Status);
    json["created_at"] = std::string(booking.CreatedAt.ToStringUpToSeconds());
    json["updated_at"] = std::string(booking.UpdatedAt.ToStringUpToSeconds());
    return json;
}

TErrorOr<NJson::TJsonValue> ReadJsonBody(const IRequestPtr& request)
{
    // IRequest::ReadAll сообщает об ошибках чтения исключением.
    SOA_ASSIGN_OR_RETURN(auto body, CatchErrors([&] {
        return request->ReadAll();
    }));
    if (body.Size() > MaxRequestBodySize) {
        return BadRequest("Request body is too large");
    }
    NJson::TJsonValue json;
    if (!NJson::ReadJsonTree(TStringBuf(body.Begin(), body.Size()), &json, /*throwOnError*/ false) || !json.IsMap()) {
        return BadRequest("Request body must be a JSON object");
    }
    return json;
}

TErrorOr<std::string> GetStringField(const NJson::TJsonValue& json, TStringBuf field)
{
    const NJson::TJsonValue* value;
    if (!json.GetValuePointer(field, &value) || !value->IsString()) {
        return MakeError(EErrorCode::InvalidArgument, "Field %v is required and must be a string", field);
    }
    return std::string(value->GetString());
}

TErrorOr<i64> GetIntegerField(const NJson::TJsonValue& json, TStringBuf field)
{
    const NJson::TJsonValue* value;
    if (!json.GetValuePointer(field, &value) || !value->IsInteger()) {
        return MakeError(EErrorCode::InvalidArgument, "Field %v is required and must be an integer", field);
    }
    return value->GetInteger();
}

//! YYYY-MM-DD -> Date (корректность даты проверяет Flight Service).
TErrorOr<Date> ParseDate(TStringBuf value)
{
    int year, month, day;
    TStringBuf yearPart, monthPart, dayPart;
    if (!StringSplitter(value).Split('-').TryCollectInto(&yearPart, &monthPart, &dayPart) ||
        yearPart.size() != 4 || monthPart.size() != 2 || dayPart.size() != 2 ||
        !TryFromString(yearPart, year) || !TryFromString(monthPart, month) || !TryFromString(dayPart, day))
    {
        return MakeError(EErrorCode::InvalidArgument, "Parameter date must be in YYYY-MM-DD format, got %Qv", value);
    }
    Date date;
    date.set_year(year);
    date.set_month(month);
    date.set_day(day);
    return date;
}

TErrorOr<i64> ParseId(TStringBuf value, TStringBuf what)
{
    i64 id;
    if (!TryFromString(value, id)) {
        return MakeError(EErrorCode::NotFound, "%v %v not found", what, value);
    }
    return id;
}

//! Ошибка уровня HTTP со своим статусом и кодом (не бизнес-ошибка сервиса).
TError MakeHttpError(EStatusCode status, TStringBuf code, TStringBuf message)
{
    return TError("%v", message)
        .With("http_status", static_cast<int>(status))
        .With("http_error_code", std::string(code));
}

TError MethodNotAllowed()
{
    return MakeHttpError(EStatusCode::MethodNotAllowed, "METHOD_NOT_ALLOWED", "Method not allowed");
}

TErrorOr<FlightStatus> ParseFlightStatus(TStringBuf value)
{
    FlightStatus status;
    if (!FlightStatus_Parse("FLIGHT_STATUS_" + std::string(value), &status) || status == FLIGHT_STATUS_UNSPECIFIED) {
        return MakeError(EErrorCode::InvalidArgument, "Unknown flight status %Qv", value);
    }
    return status;
}

TErrorOr<TInstant> GetTimeField(const NJson::TJsonValue& json, TStringBuf field)
{
    SOA_ASSIGN_OR_RETURN(auto value, GetStringField(json, field));
    TInstant instant;
    if (!TInstant::TryParseIso8601(value, instant)) {
        return MakeError(EErrorCode::InvalidArgument, "Field %v must be an ISO 8601 time, got %Qv", field, value);
    }
    return instant;
}

TErrorOr<Money> GetMoneyField(const NJson::TJsonValue& json, TStringBuf field)
{
    const NJson::TJsonValue* value;
    if (!json.GetValuePointer(field, &value) || !value->IsMap()) {
        return MakeError(EErrorCode::InvalidArgument, "Field %v must be an object {amount_minor, currency}", field);
    }
    Money money;
    SOA_ASSIGN_OR_RETURN(auto amount, GetIntegerField(*value, "amount_minor"));
    SOA_ASSIGN_OR_RETURN(auto currency, GetStringField(*value, "currency"));
    money.set_amount_minor(amount);
    money.set_currency(currency);
    return money;
}

void ToTimestamp(google::protobuf::Timestamp* timestamp, TInstant instant)
{
    timestamp->set_seconds(instant.Seconds());
    timestamp->set_nanos(static_cast<int>(instant.MicroSecondsOfSecond() * 1000));
}

////////////////////////////////////////////////////////////////////////////////
// Обработчики.

class THttpApi
{
public:
    THttpApi(TBookingService* service, TFlightClientPtr flightClient, std::string adminToken)
        : Service_(service)
        , FlightClient_(std::move(flightClient))
        , AdminToken_(std::move(adminToken))
    { }

    // /flights, /flights/{id}
    TErrorOr<TJsonResponse> HandleFlights(const IRequestPtr& request, const std::vector<TStringBuf>& path)
    {
        if (request->GetMethod() != EMethod::Get) {
            return MethodNotAllowed();
        }

        if (path.size() == 1) {
            TCgiParameters query(request->GetUrl().RawQuery);
            auto origin = std::string(query.Get("origin"));
            auto destination = std::string(query.Get("destination"));
            if (origin.empty() || destination.empty()) {
                return BadRequest("Query parameters origin and destination are required");
            }
            // IATA-коды принимаем в любом регистре.
            origin = to_upper(TString(origin));
            destination = to_upper(TString(destination));

            std::optional<Date> date;
            if (query.Has("date")) {
                SOA_ASSIGN_OR_RETURN(date, ParseDate(query.Get("date")));
            }

            SOA_ASSIGN_OR_RETURN(auto flights, Service_->SearchFlights(origin, destination, date));
            NJson::TJsonValue body(NJson::JSON_ARRAY);
            for (const auto& flight : flights) {
                body.AppendValue(FlightToJson(flight));
            }
            return TJsonResponse{.Body = std::move(body)};
        }

        if (path.size() == 2) {
            SOA_ASSIGN_OR_RETURN(auto flightId, ParseId(path[1], "Flight"));
            SOA_ASSIGN_OR_RETURN(auto flight, Service_->GetFlight(flightId));
            return TJsonResponse{.Body = FlightToJson(flight)};
        }

        return MakeError(EErrorCode::NotFound, "Not found");
    }

    // /bookings, /bookings/{id}, /bookings/{id}/cancel
    TErrorOr<TJsonResponse> HandleBookings(const IRequestPtr& request, const std::vector<TStringBuf>& path)
    {
        auto method = request->GetMethod();

        if (path.size() == 1 && method == EMethod::Post) {
            SOA_ASSIGN_OR_RETURN(auto json, ReadJsonBody(request));
            TCreateBookingRequest createRequest;
            SOA_ASSIGN_OR_RETURN(createRequest.UserId, GetStringField(json, "user_id"));
            SOA_ASSIGN_OR_RETURN(createRequest.FlightId, GetIntegerField(json, "flight_id"));
            SOA_ASSIGN_OR_RETURN(createRequest.PassengerName, GetStringField(json, "passenger_name"));
            SOA_ASSIGN_OR_RETURN(createRequest.PassengerEmail, GetStringField(json, "passenger_email"));
            SOA_ASSIGN_OR_RETURN(auto seatCount, GetIntegerField(json, "seat_count"));
            if (seatCount <= 0 || seatCount > std::numeric_limits<int>::max()) {
                return BadRequest("Field seat_count must be positive");
            }
            createRequest.SeatCount = static_cast<int>(seatCount);

            SOA_ASSIGN_OR_RETURN(auto booking, Service_->CreateBooking(createRequest));
            return TJsonResponse{.Status = EStatusCode::Created, .Body = BookingToJson(booking)};
        }

        if (path.size() == 1 && method == EMethod::Get) {
            TCgiParameters query(request->GetUrl().RawQuery);
            SOA_ASSIGN_OR_RETURN(auto bookings, Service_->ListBookings(std::string(query.Get("user_id"))));
            NJson::TJsonValue body(NJson::JSON_ARRAY);
            for (const auto& booking : bookings) {
                body.AppendValue(BookingToJson(booking));
            }
            return TJsonResponse{.Body = std::move(body)};
        }

        if (path.size() == 2 && method == EMethod::Get) {
            SOA_ASSIGN_OR_RETURN(auto booking, Service_->GetBooking(std::string(path[1])));
            return TJsonResponse{.Body = BookingToJson(booking)};
        }

        if (path.size() == 3 && path[2] == "cancel" && method == EMethod::Post) {
            SOA_ASSIGN_OR_RETURN(auto booking, Service_->CancelBooking(std::string(path[1])));
            return TJsonResponse{.Body = BookingToJson(booking)};
        }

        if (path.size() <= 2 || (path.size() == 3 && path[2] == "cancel")) {
            return MethodNotAllowed();
        }
        return MakeError(EErrorCode::NotFound, "Not found");
    }

    // /admin/airports, /admin/airlines, /admin/flights, /admin/flights/{id}
    // Тонкий прокси к административным методам Flight Service под токеном администратора.
    TErrorOr<TJsonResponse> HandleAdmin(const IRequestPtr& request, const std::vector<TStringBuf>& path)
    {
        SOA_RETURN_IF_ERROR(CheckAdminToken(request));
        auto method = request->GetMethod();

        if (path.size() == 2 && path[1] == "airports") {
            if (method != EMethod::Get) {
                return MethodNotAllowed();
            }
            SOA_ASSIGN_OR_RETURN(auto airports, FlightClient_->ListAirports());
            NJson::TJsonValue body(NJson::JSON_ARRAY);
            for (const auto& airport : airports) {
                NJson::TJsonValue json;
                json["iata_code"] = airport.iata_code();
                json["name"] = airport.name();
                json["city"] = airport.city();
                body.AppendValue(std::move(json));
            }
            return TJsonResponse{.Body = std::move(body)};
        }

        if (path.size() == 2 && path[1] == "airlines") {
            if (method != EMethod::Get) {
                return MethodNotAllowed();
            }
            SOA_ASSIGN_OR_RETURN(auto airlines, FlightClient_->ListAirlines());
            NJson::TJsonValue body(NJson::JSON_ARRAY);
            for (const auto& airline : airlines) {
                NJson::TJsonValue json;
                json["code"] = airline.code();
                json["name"] = airline.name();
                body.AppendValue(std::move(json));
            }
            return TJsonResponse{.Body = std::move(body)};
        }

        if (path.size() == 2 && path[1] == "flights") {
            if (method != EMethod::Post) {
                return MethodNotAllowed();
            }
            SOA_ASSIGN_OR_RETURN(auto json, ReadJsonBody(request));
            CreateFlightRequest createRequest;
            SOA_ASSIGN_OR_RETURN(auto airlineCode, GetStringField(json, "airline_code"));
            SOA_ASSIGN_OR_RETURN(auto flightNo, GetIntegerField(json, "flight_no"));
            SOA_ASSIGN_OR_RETURN(auto origin, GetStringField(json, "origin"));
            SOA_ASSIGN_OR_RETURN(auto destination, GetStringField(json, "destination"));
            SOA_ASSIGN_OR_RETURN(auto departure, GetTimeField(json, "departure_time"));
            SOA_ASSIGN_OR_RETURN(auto arrival, GetTimeField(json, "arrival_time"));
            SOA_ASSIGN_OR_RETURN(auto totalSeats, GetIntegerField(json, "total_seats"));
            SOA_ASSIGN_OR_RETURN(*createRequest.mutable_price(), GetMoneyField(json, "price"));
            if (flightNo < 1 || flightNo > 9999 || totalSeats < 1 || totalSeats > std::numeric_limits<int>::max()) {
                return BadRequest("Fields flight_no (1..9999) and total_seats must be positive");
            }
            createRequest.set_airline_code(to_upper(TString(airlineCode)));
            createRequest.set_flight_no(static_cast<int>(flightNo));
            createRequest.set_origin(to_upper(TString(origin)));
            createRequest.set_destination(to_upper(TString(destination)));
            ToTimestamp(createRequest.mutable_departure_time(), departure);
            ToTimestamp(createRequest.mutable_arrival_time(), arrival);
            createRequest.set_total_seats(static_cast<int>(totalSeats));

            SOA_ASSIGN_OR_RETURN(auto flight, FlightClient_->CreateFlight(createRequest));
            YT_LOG_INFO("Flight created by admin (FlightId: %v, FlightNumber: %v)", flight.id(), flight.flight_number());
            return TJsonResponse{.Status = EStatusCode::Created, .Body = FlightToJson(flight)};
        }

        if (path.size() == 3 && path[1] == "flights") {
            if (method != EMethod::Post) {
                return MethodNotAllowed();
            }
            SOA_ASSIGN_OR_RETURN(auto flightId, ParseId(path[2], "Flight"));
            SOA_ASSIGN_OR_RETURN(auto json, ReadJsonBody(request));

            std::optional<FlightStatus> status;
            if (json.Has("status")) {
                SOA_ASSIGN_OR_RETURN(auto statusString, GetStringField(json, "status"));
                SOA_ASSIGN_OR_RETURN(status, ParseFlightStatus(to_upper(TString(statusString))));
            }
            std::optional<Money> price;
            if (json.Has("price")) {
                SOA_ASSIGN_OR_RETURN(price, GetMoneyField(json, "price"));
            }
            if (!status && !price) {
                return BadRequest("Nothing to update: specify status and/or price");
            }

            SOA_ASSIGN_OR_RETURN(auto flight, FlightClient_->UpdateFlight(flightId, status, price));
            YT_LOG_INFO("Flight updated by admin (FlightId: %v, Status: %v)", flight.id(), FormatFlightStatus(flight.status()));
            return TJsonResponse{.Body = FlightToJson(flight)};
        }

        return MakeError(EErrorCode::NotFound, "Not found");
    }

private:
    TBookingService* const Service_;
    const TFlightClientPtr FlightClient_;
    const std::string AdminToken_;

    TError CheckAdminToken(const IRequestPtr& request) const
    {
        if (AdminToken_.empty()) {
            // Админка выключена (ADMIN_TOKEN не задан).
            return MakeError(EErrorCode::NotFound, "Not found");
        }
        const auto* token = request->GetHeaders()->Find("X-Admin-Token");
        if (!token || !NCrypto::ConstantTimeCompare(AdminToken_, *token)) {
            return MakeHttpError(EStatusCode::Unauthorized, "UNAUTHORIZED", "Missing or invalid X-Admin-Token");
        }
        return {};
    }
};

////////////////////////////////////////////////////////////////////////////////

void WriteJson(const IResponseWriterPtr& response, EStatusCode status, const NJson::TJsonValue& body)
{
    response->SetStatus(status);
    response->GetHeaders()->Set("Content-Type", "application/json; charset=utf-8");
    auto error = WaitFor(response->WriteBody(TSharedRef::FromString(std::string(NJson::WriteJson(body, /*formatOutput*/ false)))));
    if (!error.IsOK()) {
        YT_LOG_DEBUG(error, "Failed to write HTTP response");
    }
}

//! Общая обёртка: разбор пути, вызов обработчика, ошибка -> HTTP, access-лог.
void Dispatch(const THandler& handler, const IRequestPtr& request, const IResponseWriterPtr& response)
{
    auto start = TInstant::Now();
    auto path = request->GetUrl().Path;

    std::vector<TStringBuf> segments;
    for (const auto& part : StringSplitter(path).Split('/').SkipEmpty()) {
        segments.push_back(part.Token());
    }

    auto result = handler(request, segments);

    EStatusCode status;
    if (result.IsOK()) {
        status = result.Value().Status;
        WriteJson(response, status, result.Value().Body);
    } else if (auto httpStatus = result.Attributes().Find<int>("http_status")) {
        status = static_cast<EStatusCode>(*httpStatus);
        auto code = result.Attributes().Get<std::string>("http_error_code", "ERROR");
        WriteJson(response, status, MakeErrorBody(code, result.GetMessage()));
    } else {
        auto httpError = ToHttpError(result);
        status = httpError.Status;
        WriteJson(response, status, MakeErrorBody(httpError.Code, ToClientMessage(result)));
        if (httpError.Status >= EStatusCode::InternalServerError) {
            YT_LOG_WARNING(result, "Request failed (Method: %v, Path: %v, Status: %v)",
                request->GetMethod(),
                path,
                static_cast<int>(status));
        }
    }

    YT_LOG_INFO("HTTP %v %v -> %v (Duration: %v)",
        request->GetMethod(),
        path,
        static_cast<int>(status),
        TInstant::Now() - start);
}

} // namespace

////////////////////////////////////////////////////////////////////////////////

void RegisterHttpHandlers(
    const IServerPtr& server,
    TBookingService* service,
    TFlightClientPtr flightClient,
    std::string adminToken)
{
    auto api = std::make_shared<THttpApi>(service, std::move(flightClient), std::move(adminToken));

    // "/flights/" ловит и "/flights", и "/flights/..." - маршрутизация внутри.
    server->AddHandler("/flights/", BIND([api] (const IRequestPtr& request, const IResponseWriterPtr& response) {
        Dispatch(
            [&] (const IRequestPtr& request, const std::vector<TStringBuf>& path) {
                return api->HandleFlights(request, path);
            },
            request,
            response);
    }));
    server->AddHandler("/bookings/", BIND([api] (const IRequestPtr& request, const IResponseWriterPtr& response) {
        Dispatch(
            [&] (const IRequestPtr& request, const std::vector<TStringBuf>& path) {
                return api->HandleBookings(request, path);
            },
            request,
            response);
    }));
    server->AddHandler("/admin/", BIND([api] (const IRequestPtr& request, const IResponseWriterPtr& response) {
        Dispatch(
            [&] (const IRequestPtr& request, const std::vector<TStringBuf>& path) {
                return api->HandleAdmin(request, path);
            },
            request,
            response);
    }));
}

////////////////////////////////////////////////////////////////////////////////

} // namespace NSoa::NBooking
