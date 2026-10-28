LIBRARY()

INCLUDE(${ARCADIA_ROOT}/yt/ya_cpp.make.inc)

SRCS(
    flight_service.proto
)

PEERDIR(
    yt/yt/core
)

END()
