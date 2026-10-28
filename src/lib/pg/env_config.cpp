#include "env_config.h"

#include <soa/lib/common/env.h>
#include <soa/lib/common/error_macros.h>

namespace NSoa::NPg {

using namespace NYT;

////////////////////////////////////////////////////////////////////////////////

TErrorOr<TPgConfig> LoadPgConfigFromEnv()
{
    TPgConfig config;
    config.Host = GetEnvString("DB_HOST", "localhost");
    SOA_ASSIGN_OR_RETURN(config.Port, GetEnvValue("DB_PORT", 5432));
    SOA_ASSIGN_OR_RETURN(config.User, GetRequiredEnvString("DB_USER"));
    SOA_ASSIGN_OR_RETURN(config.Password, GetRequiredEnvString("DB_PASSWORD"));
    SOA_ASSIGN_OR_RETURN(config.Database, GetRequiredEnvString("DB_NAME"));
    SOA_ASSIGN_OR_RETURN(config.SslMode, GetEnvValue("DB_SSLMODE", EPgSslMode::Disable));
    config.SslRootCertPath = GetEnvString("DB_SSLROOTCERT");
    SOA_ASSIGN_OR_RETURN(config.PoolSize, GetEnvValue("DB_POOL_SIZE", 16));
    return config;
}

////////////////////////////////////////////////////////////////////////////////

} // namespace NSoa::NPg
