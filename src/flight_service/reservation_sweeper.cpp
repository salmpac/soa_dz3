#include "reservation_sweeper.h"

#include <yt/yt/core/concurrency/periodic_executor.h>

#include <yt/yt/core/logging/log.h>

namespace NSoa::NFlight {

using namespace NYT;
using namespace NYT::NConcurrency;

////////////////////////////////////////////////////////////////////////////////

YT_DEFINE_LEAKY_GLOBAL(const NLogging::TLogger, SweeperLogger, "ReservationSweeper");
constinit const auto Logger = SweeperLogger;

//! Сколько рейсов обрабатывается за один проход.
static constexpr int MaxFlightsPerSweep = 100;

////////////////////////////////////////////////////////////////////////////////

TPeriodicExecutorPtr StartReservationSweeper(
    IFlightRepositoryPtr repository,
    IInvokerPtr invoker,
    TDuration period)
{
    auto executor = New<TPeriodicExecutor>(
        std::move(invoker),
        BIND([repository = std::move(repository)] {
            auto expiredOrError = repository->ExpireReservations(MaxFlightsPerSweep);
            if (!expiredOrError.IsOK()) {
                // Не страшно: следующий проход повторит.
                YT_LOG_WARNING(expiredOrError, "Failed to expire reservations");
                return;
            }
            for (const auto& flight : expiredOrError.Value()) {
                YT_LOG_INFO("Reservations expired, seats returned (FlightId: %v, SeatCount: %v, BookingIds: %v)",
                    flight.Route.FlightId,
                    flight.SeatCount,
                    flight.BookingIds);
            }
        }),
        period);
    executor->Start();
    YT_LOG_INFO("Reservation sweeper started (Period: %v)", period);
    return executor;
}

////////////////////////////////////////////////////////////////////////////////

} // namespace NSoa::NFlight
