LIBRARY()

INCLUDE(${ARCADIA_ROOT}/yt/ya_cpp.make.inc)

SRCS(
    client.cpp
    env_config.cpp
)

PEERDIR(
    soa/lib/common
    soa/lib/net
    yt/yt/core
    library/cpp/string_utils/base64
)

END()
