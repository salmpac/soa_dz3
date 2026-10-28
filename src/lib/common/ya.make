LIBRARY()

INCLUDE(${ARCADIA_ROOT}/yt/ya_cpp.make.inc)

SRCS(
    bootstrap.cpp
    errors.cpp
)

PEERDIR(
    yt/yt/core
)

END()
