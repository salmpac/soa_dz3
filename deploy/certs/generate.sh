#!/bin/sh
# Генерирует локальный CA, серверные и клиентские сертификаты для docker-compose.
# Запускается одноразовым контейнером `certs`; результат - в томе /certs.
# Идемпотентен: существующие CA и сертификаты не трогает, недостающие выпускает.
set -eu

OUT=/certs
DAYS=825

# UID, под которыми работают процессы, читающие ключи.
POSTGRES_UID=999   # пользователь postgres в образе postgres:16
REDIS_UID=999      # пользователь redis в образе redis:7
SERVICE_UID=10001  # наши сервисы (USER в Dockerfile)

mkdir -p "$OUT"
cd "$OUT"

if [ ! -f ca.crt ]; then
    openssl req -x509 -newkey rsa:2048 -nodes -days "$DAYS" \
        -keyout ca.key -out ca.crt -subj "/CN=soa-flight-booking-ca" 2>/dev/null
    echo "Generated CA"
fi

# issue <name> <owner uid> <SAN...>
issue() {
    name="$1"; owner="$2"; shift 2
    if [ -f "$name.crt" ]; then
        return
    fi
    san=""
    for host in "$@"; do
        san="${san:+$san,}DNS:$host"
    done
    openssl req -newkey rsa:2048 -nodes -keyout "$name.key" -out "$name.csr" -subj "/CN=$1" 2>/dev/null
    printf 'subjectAltName=%s\nextendedKeyUsage=serverAuth,clientAuth\n' "$san" > "$name.ext"
    openssl x509 -req -in "$name.csr" -CA ca.crt -CAkey ca.key -CAcreateserial \
        -days "$DAYS" -out "$name.crt" -extfile "$name.ext" 2>/dev/null
    rm -f "$name.csr" "$name.ext"
    chown "$owner:$owner" "$name.key"
    chmod 600 "$name.key"
    chmod 644 "$name.crt"
    echo "Issued $name.crt for $*"
}

issue flight-db "$POSTGRES_UID" flight-db localhost
issue booking-db "$POSTGRES_UID" booking-db localhost
issue redis "$REDIS_UID" redis-master redis-replica redis-sentinel-1 redis-sentinel-2 redis-sentinel-3 localhost
issue flight-service "$SERVICE_UID" flight-service localhost
# Клиентский сертификат Booking Service для mTLS к Flight Service.
issue booking-service "$SERVICE_UID" booking-service

chmod 644 ca.crt
chmod 600 ca.key
echo "Certificates are ready in $OUT"
