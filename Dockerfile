# syntax=docker/dockerfile:1.7
#
# Сборка обоих сервисов через ya make (YTsaurus) со статической линковкой musl.
# Тяжёлые части (исходники ytsaurus ~3 ГБ, тулчейн ya, артефакты сборки) лежат
# в cache mount'ах BuildKit: первая сборка долгая, повторные - инкрементальные.

FROM ubuntu:24.04 AS builder

RUN apt-get update \
    && apt-get install -y --no-install-recommends git python3 rsync ca-certificates curl binutils \
    && rm -rf /var/lib/apt/lists/*

WORKDIR /work

COPY patches/ patches/
COPY scripts/ scripts/

# ytsaurus на зафиксированном коммите + наш патч + musl из yatool.
RUN --mount=type=cache,id=soa-third-party,target=/work/third_party \
    scripts/prepare-ytsaurus.sh

COPY src/ src/

RUN --mount=type=cache,id=soa-third-party,target=/work/third_party \
    --mount=type=cache,id=soa-ya-home,target=/root/.ya \
    --mount=type=cache,id=soa-build,target=/work/.build \
    scripts/build-local.sh soa/flight_service soa/booking_service \
    && mkdir -p /out \
    && cp -L .build/bin/flight-service .build/bin/booking-service /out/ \
    && strip /out/flight-service /out/booking-service

# Статические бинарники не нуждаются ни в libc, ни в shell.
FROM scratch AS flight-service
COPY --from=builder /out/flight-service /flight-service
USER 10001:10001
EXPOSE 50051
ENTRYPOINT ["/flight-service"]

FROM scratch AS booking-service
COPY --from=builder /out/booking-service /booking-service
USER 10001:10001
EXPOSE 8080
ENTRYPOINT ["/booking-service"]
