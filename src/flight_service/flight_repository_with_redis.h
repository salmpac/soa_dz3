#pragma once

#include "repository.h"

#include <soa/lib/redis/client.h>

namespace NSoa::NFlight {

////////////////////////////////////////////////////////////////////////////////

//! Декоратор репозитория: Cache-Aside в Redis поверх #underlying.
/*!
 *  Чтения:
 *    GetFlight      -> ключ flight:{id}
 *    SearchFlights  -> ключ search:{origin}:{destination}:{date|any}
 *  проверяют Redis, при промахе идут в #underlying и кладут результат с TTL.
 *
 *  Мутации (ReserveSeats, ReleaseReservation, ExpireReservations, CreateFlight, UpdateFlight) делегируются в
 *  #underlying и после успеха сбрасывают все ключи, где мог лежать рейс.
 *
 *  Ошибки Redis не фатальны: логируются, а запрос обслуживает #underlying.
 */
IFlightRepositoryPtr CreateFlightRepositoryWithRedis(
    IFlightRepositoryPtr underlying,
    NRedis::TRedisClientPtr redis,
    TDuration ttl);

////////////////////////////////////////////////////////////////////////////////

} // namespace NSoa::NFlight
