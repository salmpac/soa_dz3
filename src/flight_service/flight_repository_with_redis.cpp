#include "flight_repository_with_redis.h"

#include <soa/lib/common/error_macros.h>

#include <yt/yt/core/logging/log.h>

namespace NSoa::NFlight {

using namespace NYT;
using namespace flight::v1;

////////////////////////////////////////////////////////////////////////////////

YT_DEFINE_LEAKY_GLOBAL(const NLogging::TLogger, CacheLogger, "Cache");
constinit const auto Logger = CacheLogger;

////////////////////////////////////////////////////////////////////////////////

namespace {

std::string FlightKey(i64 flightId)
{
    return "flight:" + std::to_string(flightId);
}

std::string SearchKey(const std::string& origin, const std::string& destination, const std::optional<TDate>& date)
{
    return "search:" + origin + ":" + destination + ":" + date.value_or("any");
}

class TFlightRepositoryWithRedis
    : public IFlightRepository
{
public:
    TFlightRepositoryWithRedis(IFlightRepositoryPtr underlying, NRedis::TRedisClientPtr redis, TDuration ttl)
        : Underlying_(std::move(underlying))
        , Redis_(std::move(redis))
        , Ttl_(ttl)
    { }

    TErrorOr<std::vector<Flight>> SearchFlights(
        const std::string& origin,
        const std::string& destination,
        const std::optional<TDate>& departureDate) override
    {
        // В кеше список рейсов лежит как сериализованный SearchFlightsResponse.
        SOA_ASSIGN_OR_RETURN(auto response, GetOrLoad<SearchFlightsResponse>(
            SearchKey(origin, destination, departureDate),
            [&] () -> TErrorOr<SearchFlightsResponse> {
                SOA_ASSIGN_OR_RETURN(auto flights, Underlying_->SearchFlights(origin, destination, departureDate));
                SearchFlightsResponse result;
                for (auto& flight : flights) {
                    *result.add_flights() = std::move(flight);
                }
                return result;
            }));
        return std::vector<Flight>(
            std::make_move_iterator(response.mutable_flights()->begin()),
            std::make_move_iterator(response.mutable_flights()->end()));
    }

    TErrorOr<Flight> GetFlight(i64 flightId) override
    {
        return GetOrLoad<Flight>(FlightKey(flightId), [&] {
            return Underlying_->GetFlight(flightId);
        });
    }

    TErrorOr<TReserveResult> ReserveSeats(i64 flightId, int seatCount, const std::string& bookingId) override
    {
        SOA_ASSIGN_OR_RETURN(auto result, Underlying_->ReserveSeats(flightId, seatCount, bookingId));
        if (!result.Replayed) {
            Invalidate(result.Route);
        }
        return result;
    }

    TErrorOr<SeatReservation> ConfirmReservation(const std::string& bookingId) override
    {
        // Места не меняются - кешированные данные остаются верными.
        return Underlying_->ConfirmReservation(bookingId);
    }

    TErrorOr<std::vector<TExpiredReservations>> ExpireReservations(int maxFlights) override
    {
        SOA_ASSIGN_OR_RETURN(auto expired, Underlying_->ExpireReservations(maxFlights));
        for (const auto& flight : expired) {
            Invalidate(flight.Route);
        }
        return expired;
    }

    TErrorOr<TReleaseResult> ReleaseReservation(const std::string& bookingId) override
    {
        SOA_ASSIGN_OR_RETURN(auto result, Underlying_->ReleaseReservation(bookingId));
        if (!result.Replayed) {
            Invalidate(result.Route);
        }
        return result;
    }

    TErrorOr<TUpdateResult> CreateFlight(const TNewFlight& flight) override
    {
        SOA_ASSIGN_OR_RETURN(auto result, Underlying_->CreateFlight(flight));
        // Новый рейс должен сразу появиться в закешированных результатах поиска по маршруту.
        Invalidate(result.Route);
        return result;
    }

    TErrorOr<std::vector<Airport>> ListAirports() override
    {
        return Underlying_->ListAirports();
    }

    TErrorOr<std::vector<Airline>> ListAirlines() override
    {
        return Underlying_->ListAirlines();
    }

    TErrorOr<TUpdateResult> UpdateFlight(
        i64 flightId,
        std::optional<FlightStatus> status,
        std::optional<Money> price) override
    {
        SOA_ASSIGN_OR_RETURN(auto result, Underlying_->UpdateFlight(flightId, status, price));
        Invalidate(result.Route);
        return result;
    }

private:
    const IFlightRepositoryPtr Underlying_;
    const NRedis::TRedisClientPtr Redis_;
    const TDuration Ttl_;

    template <class TMessage>
    TErrorOr<TMessage> GetOrLoad(const std::string& key, const std::function<TErrorOr<TMessage>()>& loader)
    {
        if (auto cachedOrError = Redis_->Get(key); !cachedOrError.IsOK()) {
            YT_LOG_WARNING(cachedOrError, "Cache read failed, falling back to database (Key: %v)", key);
        } else if (const auto& cached = cachedOrError.Value()) {
            TMessage message;
            if (message.ParseFromString(*cached)) {
                YT_LOG_INFO("Cache hit (Key: %v)", key);
                return message;
            }
            YT_LOG_WARNING("Cached value is corrupted, ignoring (Key: %v)", key);
        } else {
            YT_LOG_INFO("Cache miss (Key: %v)", key);
        }

        SOA_ASSIGN_OR_RETURN(auto message, loader());

        if (auto error = Redis_->SetEx(key, message.SerializeAsString(), Ttl_); !error.IsOK()) {
            YT_LOG_WARNING(error, "Cache write failed (Key: %v)", key);
        } else {
            YT_LOG_DEBUG("Cache populated (Key: %v, Ttl: %v)", key, Ttl_);
        }
        return message;
    }

    void Invalidate(const TFlightRoute& route)
    {
        // Рейс может лежать в трёх ключах: сам рейс, поиск по его дате и поиск без даты.
        std::vector<std::string> keys{
            FlightKey(route.FlightId),
            SearchKey(route.Origin, route.Destination, route.DepartureDate),
            SearchKey(route.Origin, route.Destination, std::nullopt),
        };
        auto deletedOrError = Redis_->Del(keys);
        if (!deletedOrError.IsOK()) {
            // Не удалось сбросить: устаревшее значение проживёт не дольше TTL.
            YT_LOG_ERROR(deletedOrError, "Cache invalidation failed (Keys: %v)", keys);
            return;
        }
        YT_LOG_INFO("Cache invalidated (Keys: %v, Deleted: %v)", keys, deletedOrError.Value());
    }
};

} // namespace

////////////////////////////////////////////////////////////////////////////////

IFlightRepositoryPtr CreateFlightRepositoryWithRedis(
    IFlightRepositoryPtr underlying,
    NRedis::TRedisClientPtr redis,
    TDuration ttl)
{
    YT_VERIFY(underlying);
    YT_VERIFY(redis);
    return New<TFlightRepositoryWithRedis>(std::move(underlying), std::move(redis), ttl);
}

////////////////////////////////////////////////////////////////////////////////

} // namespace NSoa::NFlight
