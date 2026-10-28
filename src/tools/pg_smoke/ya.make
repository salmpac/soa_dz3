PROGRAM(pg_smoke)

INCLUDE(${ARCADIA_ROOT}/yt/ya_cpp.make.inc)

SRCS(
    main.cpp
)

PEERDIR(
    soa/lib/pg
    soa/lib/common
    yt/yt/core
)

END()
