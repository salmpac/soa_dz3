#pragma once

#include "repository.h"

#include <soa/lib/pg/client.h>

namespace NSoa::NFlight {

////////////////////////////////////////////////////////////////////////////////

//! Репозиторий рейсов в PostgreSQL. Резервирование, отмена и истечение
//! резерваций - транзакции с SELECT FOR UPDATE по строке рейса.
//! #reservationTtl - срок жизни неподтверждённой резервации.
IFlightRepositoryPtr CreatePgFlightRepository(NPg::TPgClientPtr client, TDuration reservationTtl);

////////////////////////////////////////////////////////////////////////////////

} // namespace NSoa::NFlight
