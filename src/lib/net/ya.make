LIBRARY()

INCLUDE(${ARCADIA_ROOT}/yt/ya_cpp.make.inc)

SRCS(
    buffered_connection.cpp
)

PEERDIR(
    soa/lib/common
    yt/yt/core
)

END()
