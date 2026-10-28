LIBRARY()

INCLUDE(${ARCADIA_ROOT}/yt/ya_cpp.make.inc)

SRCS(
    api_key_channel.cpp
    circuit_breaker_channel.cpp
    error_classification.cpp
    retrying_channel.cpp
)

PEERDIR(
    soa/lib/common
    yt/yt/core
)

END()
