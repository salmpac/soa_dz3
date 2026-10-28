PROGRAM(redis_smoke)

INCLUDE(${ARCADIA_ROOT}/yt/ya_cpp.make.inc)

SRCS(
    main.cpp
)

PEERDIR(
    soa/lib/common
    soa/lib/redis
    yt/yt/core
)

END()
