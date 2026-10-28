PROGRAM(flight-service)

INCLUDE(${ARCADIA_ROOT}/yt/ya_cpp.make.inc)

SRCS(
    flight_repository_with_redis.cpp
    main.cpp
    pg_repository.cpp
    reservation_sweeper.cpp
    service.cpp
)

PEERDIR(
    soa/lib/common
    soa/lib/pg
    soa/lib/redis
    soa/proto
    yt/yt/core
    yt/yt/core/rpc/grpc
    contrib/libs/re2
)

END()
