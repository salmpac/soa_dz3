LIBRARY()

INCLUDE(${ARCADIA_ROOT}/yt/ya_cpp.make.inc)

SRCS(
    client.cpp
)

PEERDIR(
    soa/lib/common
    soa/lib/net
    yt/yt/core
)

END()
