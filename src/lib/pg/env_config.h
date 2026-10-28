#pragma once

#include "client.h"

namespace NSoa::NPg {

////////////////////////////////////////////////////////////////////////////////

//! TPgConfig из переменных окружения:
//!   DB_HOST, DB_PORT, DB_USER*, DB_PASSWORD*, DB_NAME*,
//!   DB_SSLMODE (disable|require|verify_full), DB_SSLROOTCERT, DB_POOL_SIZE.
//! (* - обязательные)
NYT::TErrorOr<TPgConfig> LoadPgConfigFromEnv();

////////////////////////////////////////////////////////////////////////////////

} // namespace NSoa::NPg
