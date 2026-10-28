PROGRAM(booking-service)

INCLUDE(${ARCADIA_ROOT}/yt/ya_cpp.make.inc)

SRCS(
    booking_repository.cpp
    booking_service.cpp
    flight_client.cpp
    http_api.cpp
    main.cpp
)

PEERDIR(
    soa/lib/common
    soa/lib/pg
    soa/lib/rpc
    soa/proto
    yt/yt/core
    yt/yt/core/http
    yt/yt/core/rpc/grpc
    contrib/libs/re2
    library/cpp/cgiparam
    library/cpp/json
)

END()
