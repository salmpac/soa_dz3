#include "service.h"

#include <soa/lib/common/error_macros.h>
#include <soa/lib/common/errors.h>

#include <soa/proto/flight_service_proxy.h>

#include <yt/yt/core/crypto/crypto.h>

#include <yt/yt/core/rpc/authenticator.h>
#include <yt/yt/core/rpc/service_detail.h>

#include <yt/yt_proto/yt/core/rpc/proto/rpc.pb.h>

#include <contrib/libs/re2/re2/re2.h>

namespace NSoa::NFlight {

using namespace NYT;
using namespace NYT::NRpc;
using namespace flight::v1;
using NSoa::EErrorCode;

////////////////////////////////////////////////////////////////////////////////

YT_DEFINE_LEAKY_GLOBAL(const NLogging::TLogger, FlightServiceLogger, "FlightService");
constinit const auto Logger = FlightServiceLogger;

static constexpr TStringBuf ApiKeyMetadataKey = "x-api-key";

////////////////////////////////////////////////////////////////////////////////

namespace {

class TApiKeyAuthenticator
    : public IAuthenticator
{
public:
    explicit TApiKeyAuthenticator(std::string apiKey)
        : ApiKey_(std::move(apiKey))
    {
        YT_VERIFY(!ApiKey_.empty());
    }

    bool CanAuthenticate(const TAuthenticationContext& /*context*/) override
    {
        // Берём на себя все запросы: отсутствие ключа - тоже ошибка аутентификации.
        return true;
    }

    TFuture<TAuthenticationResult> AsyncAuthenticate(const TAuthenticationContext& context) override
    {
        const auto& header = *context.Header;
        const TString* apiKey = nullptr;
        if (header.HasExtension(NRpc::NProto::TCustomMetadataExt::custom_metadata_ext)) {
            const auto& entries = header.GetExtension(NRpc::NProto::TCustomMetadataExt::custom_metadata_ext).entries();
            if (auto it = entries.find(std::string(ApiKeyMetadataKey)); it != entries.end()) {
                apiKey = &it->second;
            }
        }

        if (!apiKey) {
            YT_LOG_WARNING("Request without API key rejected (Method: %v.%v, Peer: %v)",
                header.service(),
                header.method(),
                context.UserIP);
            return MakeFuture<TAuthenticationResult>(MakeError(
                EErrorCode::Unauthenticated,
                "Missing API key in %Qv metadata",
                ApiKeyMetadataKey));
        }
        if (!NCrypto::ConstantTimeCompare(ApiKey_, *apiKey)) {
            YT_LOG_WARNING("Request with invalid API key rejected (Method: %v.%v, Peer: %v)",
                header.service(),
                header.method(),
                context.UserIP);
            return MakeFuture<TAuthenticationResult>(MakeError(EErrorCode::Unauthenticated, "Invalid API key"));
        }

        return MakeFuture(TAuthenticationResult{
            .User = "service",
        });
    }

private:
    const std::string ApiKey_;
};

////////////////////////////////////////////////////////////////////////////////

TError ValidateIataCode(const std::string& code, TStringBuf field)
{
    static const re2::RE2 IataRegex("[A-Z]{3}");
    if (!re2::RE2::FullMatch(code, IataRegex)) {
        return MakeError(EErrorCode::InvalidArgument, "Field %v must be an IATA airport code (3 uppercase letters), got %Qv", field, code);
    }
    return {};
}

TError ValidateBookingId(const std::string& bookingId)
{
    static const re2::RE2 UuidRegex("[0-9a-fA-F]{8}-[0-9a-fA-F]{4}-[0-9a-fA-F]{4}-[0-9a-fA-F]{4}-[0-9a-fA-F]{12}");
    if (!re2::RE2::FullMatch(bookingId, UuidRegex)) {
        return MakeError(EErrorCode::InvalidArgument, "Field booking_id must be a UUID, got %Qv", bookingId);
    }
    return {};
}

TError ValidateFlightId(i64 flightId)
{
    if (flightId <= 0) {
        return MakeError(EErrorCode::InvalidArgument, "Field flight_id must be positive, got %v", flightId);
    }
    return {};
}

TErrorOr<TDate> FormatDate(const Date& date)
{
    static constexpr int DaysInMonth[] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
    auto year = date.year();
    auto month = date.month();
    auto day = date.day();
    bool leap = (year % 4 == 0 && year % 100 != 0) || year % 400 == 0;
    bool valid = year >= 1970 && year <= 9999 &&
        month >= 1 && month <= 12 &&
        day >= 1 && day <= DaysInMonth[month - 1] + (month == 2 && leap ? 1 : 0);
    if (!valid) {
        return MakeError(EErrorCode::InvalidArgument, "Invalid departure date %04d-%02d-%02d", year, month, day);
    }
    return std::string(Format("%04d-%02d-%02d", year, month, day));
}

////////////////////////////////////////////////////////////////////////////////

class TFlightService
    : public TServiceBase
{
public:
    TFlightService(
        IInvokerPtr invoker,
        IFlightRepositoryPtr repository,
        IAuthenticatorPtr authenticator)
        : TServiceBase(
            std::move(invoker),
            GetFlightServiceDescriptor(),
            FlightServiceLogger(),
            TServiceOptions{
                .Authenticator = std::move(authenticator),
            })
        , Repository_(std::move(repository))
    {
        RegisterMethod(RPC_SERVICE_METHOD_DESC(SearchFlights));
        RegisterMethod(RPC_SERVICE_METHOD_DESC(GetFlight));
        RegisterMethod(RPC_SERVICE_METHOD_DESC(ReserveSeats));
        RegisterMethod(RPC_SERVICE_METHOD_DESC(ConfirmReservation));
        RegisterMethod(RPC_SERVICE_METHOD_DESC(ReleaseReservation));
        RegisterMethod(RPC_SERVICE_METHOD_DESC(CreateFlight));
        RegisterMethod(RPC_SERVICE_METHOD_DESC(UpdateFlight));
        RegisterMethod(RPC_SERVICE_METHOD_DESC(ListAirports));
        RegisterMethod(RPC_SERVICE_METHOD_DESC(ListAirlines));
    }

private:
    const IFlightRepositoryPtr Repository_;

    // Каждый обработчик - тонкая обёртка: логика в Do*, которая возвращает TError,
    // а ответ (успешный или с ошибкой) уходит одним Reply.

    DECLARE_RPC_SERVICE_METHOD_VIA_MESSAGES(SearchFlightsRequest, SearchFlightsResponse, SearchFlights)
    {
        context->Reply(DoSearchFlights(*request, response, context));
    }

    TError DoSearchFlights(const SearchFlightsRequest& request, SearchFlightsResponse* response, const TCtxSearchFlightsPtr& context)
    {
        SOA_RETURN_IF_ERROR(ValidateIataCode(request.origin(), "origin"));
        SOA_RETURN_IF_ERROR(ValidateIataCode(request.destination(), "destination"));
        std::optional<TDate> date;
        if (request.has_departure_date()) {
            SOA_ASSIGN_OR_RETURN(date, FormatDate(request.departure_date()));
        }
        context->AnnotateRequest()
            .With("Origin", request.origin())
            .With("Destination", request.destination())
            .With("Date", date);

        SOA_ASSIGN_OR_RETURN(auto flights, Repository_->SearchFlights(request.origin(), request.destination(), date));
        for (auto& flight : flights) {
            *response->add_flights() = std::move(flight);
        }

        context->AnnotateResponse()
            .With("FlightCount", response->flights_size());
        return {};
    }

    DECLARE_RPC_SERVICE_METHOD_VIA_MESSAGES(GetFlightRequest, GetFlightResponse, GetFlight)
    {
        context->Reply(DoGetFlight(*request, response, context));
    }

    TError DoGetFlight(const GetFlightRequest& request, GetFlightResponse* response, const TCtxGetFlightPtr& context)
    {
        SOA_RETURN_IF_ERROR(ValidateFlightId(request.flight_id()));
        context->AnnotateRequest()
            .With("FlightId", request.flight_id());

        SOA_ASSIGN_OR_RETURN(*response->mutable_flight(), Repository_->GetFlight(request.flight_id()));
        return {};
    }

    DECLARE_RPC_SERVICE_METHOD_VIA_MESSAGES(ReserveSeatsRequest, ReserveSeatsResponse, ReserveSeats)
    {
        context->Reply(DoReserveSeats(*request, response, context));
    }

    TError DoReserveSeats(const ReserveSeatsRequest& request, ReserveSeatsResponse* response, const TCtxReserveSeatsPtr& context)
    {
        SOA_RETURN_IF_ERROR(ValidateFlightId(request.flight_id()));
        SOA_RETURN_IF_ERROR(ValidateBookingId(request.booking_id()));
        if (request.seat_count() <= 0) {
            return MakeError(EErrorCode::InvalidArgument, "Field seat_count must be positive, got %v", request.seat_count());
        }
        context->AnnotateRequest()
            .With("FlightId", request.flight_id())
            .With("SeatCount", request.seat_count())
            .With("BookingId", request.booking_id());

        SOA_ASSIGN_OR_RETURN(auto result, Repository_->ReserveSeats(request.flight_id(), request.seat_count(), request.booking_id()));

        *response->mutable_reservation() = std::move(result.Reservation);
        *response->mutable_seat_price() = std::move(result.SeatPrice);
        context->AnnotateResponse()
            .With("ReservationId", response->reservation().id())
            .With("Replayed", result.Replayed);
        return {};
    }

    DECLARE_RPC_SERVICE_METHOD_VIA_MESSAGES(ConfirmReservationRequest, ConfirmReservationResponse, ConfirmReservation)
    {
        context->Reply(DoConfirmReservation(*request, response, context));
    }

    TError DoConfirmReservation(const ConfirmReservationRequest& request, ConfirmReservationResponse* response, const TCtxConfirmReservationPtr& context)
    {
        SOA_RETURN_IF_ERROR(ValidateBookingId(request.booking_id()));
        context->AnnotateRequest()
            .With("BookingId", request.booking_id());

        SOA_ASSIGN_OR_RETURN(*response->mutable_reservation(), Repository_->ConfirmReservation(request.booking_id()));
        return {};
    }

    DECLARE_RPC_SERVICE_METHOD_VIA_MESSAGES(ReleaseReservationRequest, ReleaseReservationResponse, ReleaseReservation)
    {
        context->Reply(DoReleaseReservation(*request, response, context));
    }

    TError DoReleaseReservation(const ReleaseReservationRequest& request, ReleaseReservationResponse* response, const TCtxReleaseReservationPtr& context)
    {
        SOA_RETURN_IF_ERROR(ValidateBookingId(request.booking_id()));
        context->AnnotateRequest()
            .With("BookingId", request.booking_id());

        SOA_ASSIGN_OR_RETURN(auto result, Repository_->ReleaseReservation(request.booking_id()));

        *response->mutable_reservation() = std::move(result.Reservation);
        context->AnnotateResponse()
            .With("Replayed", result.Replayed);
        return {};
    }

    DECLARE_RPC_SERVICE_METHOD_VIA_MESSAGES(CreateFlightRequest, CreateFlightResponse, CreateFlight)
    {
        context->Reply(DoCreateFlight(*request, response, context));
    }

    TError DoCreateFlight(const CreateFlightRequest& request, CreateFlightResponse* response, const TCtxCreateFlightPtr& context)
    {
        static const re2::RE2 AirlineCodeRegex("[A-Z0-9]{2}");
        static const re2::RE2 CurrencyRegex("[A-Z]{3}");
        static constexpr int MaxSeats = 1000;

        if (!re2::RE2::FullMatch(request.airline_code(), AirlineCodeRegex)) {
            return MakeError(EErrorCode::InvalidArgument, "Field airline_code must be a 2-character IATA code, got %Qv", request.airline_code());
        }
        if (request.flight_no() < 1 || request.flight_no() > 9999) {
            return MakeError(EErrorCode::InvalidArgument, "Field flight_no must be between 1 and 9999");
        }
        SOA_RETURN_IF_ERROR(ValidateIataCode(request.origin(), "origin"));
        SOA_RETURN_IF_ERROR(ValidateIataCode(request.destination(), "destination"));
        if (request.origin() == request.destination()) {
            return MakeError(EErrorCode::InvalidArgument, "Origin and destination must differ");
        }
        if (!request.has_departure_time() || !request.has_arrival_time()) {
            return MakeError(EErrorCode::InvalidArgument, "Fields departure_time and arrival_time are required");
        }
        auto departure = TInstant::Seconds(request.departure_time().seconds());
        auto arrival = TInstant::Seconds(request.arrival_time().seconds());
        if (departure <= TInstant::Now()) {
            return MakeError(EErrorCode::InvalidArgument, "Departure time must be in the future");
        }
        if (arrival <= departure) {
            return MakeError(EErrorCode::InvalidArgument, "Arrival time must be after departure time");
        }
        if (request.total_seats() < 1 || request.total_seats() > MaxSeats) {
            return MakeError(EErrorCode::InvalidArgument, "Field total_seats must be between 1 and %v", MaxSeats);
        }
        if (request.price().amount_minor() <= 0 || !re2::RE2::FullMatch(request.price().currency(), CurrencyRegex)) {
            return MakeError(EErrorCode::InvalidArgument, "Price must be positive with ISO 4217 currency");
        }
        context->AnnotateRequest()
            .With("FlightNumber", Format("%v%v", request.airline_code(), request.flight_no()))
            .With("Origin", request.origin())
            .With("Destination", request.destination())
            .With("Departure", departure);

        SOA_ASSIGN_OR_RETURN(auto result, Repository_->CreateFlight(TNewFlight{
            .AirlineCode = request.airline_code(),
            .FlightNo = request.flight_no(),
            .Origin = request.origin(),
            .Destination = request.destination(),
            .DepartureTime = departure,
            .ArrivalTime = arrival,
            .TotalSeats = request.total_seats(),
            .Price = request.price(),
        }));

        *response->mutable_flight() = std::move(result.Flight);
        context->AnnotateResponse()
            .With("FlightId", response->flight().id());
        return {};
    }

    DECLARE_RPC_SERVICE_METHOD_VIA_MESSAGES(ListAirportsRequest, ListAirportsResponse, ListAirports)
    {
        context->Reply(DoListAirports(response));
    }

    TError DoListAirports(ListAirportsResponse* response)
    {
        SOA_ASSIGN_OR_RETURN(auto airports, Repository_->ListAirports());
        for (auto& airport : airports) {
            *response->add_airports() = std::move(airport);
        }
        return {};
    }

    DECLARE_RPC_SERVICE_METHOD_VIA_MESSAGES(ListAirlinesRequest, ListAirlinesResponse, ListAirlines)
    {
        context->Reply(DoListAirlines(response));
    }

    TError DoListAirlines(ListAirlinesResponse* response)
    {
        SOA_ASSIGN_OR_RETURN(auto airlines, Repository_->ListAirlines());
        for (auto& airline : airlines) {
            *response->add_airlines() = std::move(airline);
        }
        return {};
    }

    DECLARE_RPC_SERVICE_METHOD_VIA_MESSAGES(UpdateFlightRequest, UpdateFlightResponse, UpdateFlight)
    {
        context->Reply(DoUpdateFlight(*request, response, context));
    }

    TError DoUpdateFlight(const UpdateFlightRequest& request, UpdateFlightResponse* response, const TCtxUpdateFlightPtr& context)
    {
        SOA_RETURN_IF_ERROR(ValidateFlightId(request.flight_id()));
        std::optional<FlightStatus> status;
        if (request.has_status()) {
            if (request.status() == FLIGHT_STATUS_UNSPECIFIED || !FlightStatus_IsValid(request.status())) {
                return MakeError(EErrorCode::InvalidArgument, "Invalid flight status %v", static_cast<int>(request.status()));
            }
            status = request.status();
        }
        std::optional<Money> price;
        if (request.has_price()) {
            static const re2::RE2 CurrencyRegex("[A-Z]{3}");
            if (request.price().amount_minor() <= 0 || !re2::RE2::FullMatch(request.price().currency(), CurrencyRegex)) {
                return MakeError(EErrorCode::InvalidArgument, "Price must be positive with ISO 4217 currency");
            }
            price = request.price();
        }
        context->AnnotateRequest()
            .With("FlightId", request.flight_id());

        SOA_ASSIGN_OR_RETURN(auto result, Repository_->UpdateFlight(request.flight_id(), status, price));

        *response->mutable_flight() = std::move(result.Flight);
        return {};
    }
};

} // namespace

////////////////////////////////////////////////////////////////////////////////

IAuthenticatorPtr CreateApiKeyAuthenticator(std::string apiKey)
{
    return New<TApiKeyAuthenticator>(std::move(apiKey));
}

IServicePtr CreateFlightService(
    IInvokerPtr invoker,
    IFlightRepositoryPtr repository,
    IAuthenticatorPtr authenticator)
{
    return New<TFlightService>(std::move(invoker), std::move(repository), std::move(authenticator));
}

////////////////////////////////////////////////////////////////////////////////

} // namespace NSoa::NFlight
