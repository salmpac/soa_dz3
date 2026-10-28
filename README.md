# Flight Booking: gRPC + Redis на YT fibers

Два микросервиса на C++20 поверх `yt/yt/core` из [YTsaurus](https://github.com/ytsaurus/ytsaurus):
fibers, gRPC-сервер и клиент, HTTP-сервер. Postgres и Redis — свои асинхронные клиенты
поверх YT-шного poller'а (epoll), без блокирующих драйверов.

```
Client (REST) ──► Booking Service ──gRPC/TLS + API key──► Flight Service
                        │                                   │        │
                   PostgreSQL (TLS)                  PostgreSQL   Redis master + replica
                                                        (TLS)     + 3 sentinel (TLS)
```

## Запуск

```bash
docker compose up --build
```

Нужен Docker Compose v2 (`docker compose`, входит в современный Docker): Dockerfile
использует cache mount'ы BuildKit (`RUN --mount=type=cache`), которые старый
`docker-compose` v1 по умолчанию не поддерживает. На v1 сборку можно запустить так:
`DOCKER_BUILDKIT=1 COMPOSE_DOCKER_CLI_BUILD=1 docker-compose up --build`.

Первая сборка долгая: в Docker скачивается ytsaurus (~3 ГБ) и тулчейн `ya`, а затем YT core
собирается под musl. Всё это кешируется в cache mount'ах BuildKit, повторные сборки инкрементальные.

Поднимаются:
- `web` — веб-интерфейс на http://localhost:3000 (поиск, бронирование, управление рейсами);
- `certs` — одноразово генерирует локальный CA и сертификаты (том `certs`);
- `flight-db`, `booking-db` — PostgreSQL 16, доступ по сети только через TLS + SCRAM;
- `flight-migrations`, `booking-migrations` — Flyway, применяются автоматически;
- `redis-master`, `redis-replica`, `redis-sentinel-{1,2,3}` — Redis Sentinel по TLS;
- `flight-service` (gRPC, `:50051`), `booking-service` (REST, `:8080`).

Секреты и параметры: `.env.example` → `.env`.

## REST API (Booking Service, `:8080`)

```bash
# Поиск рейсов (date опционален, коды в любом регистре)
curl 'localhost:8080/flights?origin=SVO&destination=LED'
curl 'localhost:8080/flights?origin=vko&destination=led&date=2026-11-05'

# Рейс по id
curl localhost:8080/flights/1

# Бронирование: GetFlight -> ReserveSeats -> total_price = seat_count * price -> CONFIRMED
curl -X POST localhost:8080/bookings -H 'Content-Type: application/json' -d '{
  "user_id": "user-1", "flight_id": 2, "passenger_name": "Ivan Ivanov",
  "passenger_email": "ivan@example.com", "seat_count": 2
}'

curl localhost:8080/bookings/<id>
curl 'localhost:8080/bookings?user_id=user-1'
curl -X POST localhost:8080/bookings/<id>/cancel
```

### Администрирование рейсов

Под заголовком `X-Admin-Token` (значение — env `ADMIN_TOKEN`, по умолчанию `admin`;
тем же демо-токеном пользуется веб-страница). Пустой `ADMIN_TOKEN` выключает админку.

```bash
curl -H 'X-Admin-Token: admin' localhost:8080/admin/airports
curl -H 'X-Admin-Token: admin' localhost:8080/admin/airlines

# Новый рейс (время в UTC; цена в копейках)
curl -X POST -H 'X-Admin-Token: admin' localhost:8080/admin/flights -d '{
  "airline_code": "SU", "flight_no": 7001, "origin": "SVO", "destination": "KZN",
  "departure_time": "2026-11-20T14:00:00Z", "arrival_time": "2026-11-20T15:30:00Z",
  "total_seats": 120, "price": {"amount_minor": 650000, "currency": "RUB"}
}'

# Сменить статус и/или цену (например, отменить рейс)
curl -X POST -H 'X-Admin-Token: admin' localhost:8080/admin/flights/<id> -d '{"status": "CANCELLED"}'
```

Новый рейс сразу виден в поиске: Flight Service сбрасывает кеш поиска по его маршруту.
Ошибки: 401 без токена, 400 для неизвестных кодов и некорректных данных, 409 если рейс с
таким номером в эту дату уже есть.

Ошибки — `{"error": {"code": "...", "message": "..."}}`:

| HTTP | code | когда |
|---|---|---|
| 400 | `INVALID_ARGUMENT` | невалидный ввод |
| 401 | `UNAUTHORIZED` | нет или неверный `X-Admin-Token` (только `/admin/*`) |
| 404 | `NOT_FOUND` | рейс/бронирование не найдено |
| 409 | `NOT_ENOUGH_SEATS` | недостаточно мест (gRPC `RESOURCE_EXHAUSTED`) |
| 409 | `FAILED_PRECONDITION` | бронирование уже отменено, рейс не в продаже |
| 503 | `FLIGHT_SERVICE_UNAVAILABLE` | Flight Service недоступен / circuit breaker разомкнут |
| 504 | `FLIGHT_SERVICE_TIMEOUT` | таймаут после всех повторов |

## gRPC API (Flight Service, `:50051`)

Контракт: [`src/proto/flight_service.proto`](src/proto/flight_service.proto). Сервер совместим
с обычными gRPC-клиентами. Доступ защищён в два слоя:

- **mTLS** — сервер принимает только клиентов с сертификатом, подписанным локальным CA
  (у Booking Service — `booking-service.crt`);
- **API key** в metadata `x-api-key` — проверяется для каждого метода.

```bash
mkdir -p tls && docker run --rm -v soa_dz3_certs:/certs -v "$PWD/tls:/out" alpine \
  sh -c 'cp /certs/ca.crt /certs/booking-service.crt /certs/booking-service.key /out/ && chmod 644 /out/*'

grpcurl -cacert tls/ca.crt -cert tls/booking-service.crt -key tls/booking-service.key \
  -authority flight-service -import-path src/proto -proto flight_service.proto \
  -H 'x-api-key: soa-flight-api-key' -d '{"flight_id": 1}' \
  localhost:50051 flight.v1.FlightService/GetFlight
```

Без клиентского сертификата соединение не устанавливается; без ключа или с неверным
ключом — `UNAUTHENTICATED`.

## Где что реализовано

| Пункт | Реализация |
|---|---|
| 1. gRPC-контракт | `src/proto/flight_service.proto`: `SearchFlights`, `GetFlight`, `ReserveSeats`, `ReleaseReservation` (+ `ConfirmReservation`, админские `CreateFlight`, `UpdateFlight`, `ListAirports`, `ListAirlines`), `google.protobuf.Timestamp`, enum статусов, коды ошибок описаны в контракте. Сообщения генерирует `protoc`. YT по умолчанию отдаёт любую ошибку как `UNKNOWN`, поэтому `patches/ytsaurus.patch` добавляет отображение ошибок в стандартные статусы (`NOT_FOUND`, `RESOURCE_EXHAUSTED`, `UNAUTHENTICATED`, ...). |
| 2. ER-диаграмма, 3NF | [`docs/er-diagram.md`](docs/er-diagram.md), ограничения — в `migrations/*/V1__schema.sql` |
| 3. PostgreSQL, миграции | две БД, Flyway (`migrations/`), клиент Postgres — `src/lib/pg` (wire protocol v3, SCRAM-SHA-256, TLS) |
| 4. Межсервисный gRPC | `src/booking_service/booking_service.cpp`: `GetFlight` → `ReserveSeats` → цена → запись |
| 5. Транзакции | `src/flight_service/pg_repository.cpp`: резерв и отмена — одна транзакция с `SELECT ... FOR UPDATE` по рейсу. Booking: при ошибке `ReserveSeats` бронь не создаётся; запись брони и `ConfirmReservation` — одна транзакция Booking DB (см. ниже). |
| 6. Аутентификация | API key в metadata `x-api-key`: сервер — `IAuthenticator` в `src/flight_service/service.cpp`, клиент — interceptor `src/lib/rpc/api_key_channel.cpp`; ключ из env `FLIGHT_SERVICE_API_KEY`. Дополнительно — mTLS между сервисами. |
| 7. Redis-кеш | `src/flight_service/flight_repository_with_redis.cpp`: Cache-Aside, ключи `flight:{id}` и `search:{origin}:{dest}:{date\|any}` с TTL (`CACHE_TTL_MS`, 5 мин), инвалидация после мутаций, логи `Cache hit` / `Cache miss` |
| 8. Retry | `src/lib/rpc/retrying_channel.cpp`: 3 попытки, backoff 100/200 мс, только `UNAVAILABLE`/`DEADLINE_EXCEEDED`; `ReserveSeats` идемпотентен по `booking_id` |
| 9. Redis Sentinel | master + replica + 3 sentinel (кворум 2); клиент `src/lib/redis` находит мастер через Sentinel и переподключается после failover |
| 10. Circuit Breaker | `src/lib/rpc/circuit_breaker_channel.cpp`: interceptor CLOSED/OPEN/HALF_OPEN, параметры `CB_*` из env, переходы в логах, в OPEN клиент сразу получает 503 |

## Резервации с таймаутом

`ReserveSeats` создаёт **временную** резервацию с `expires_at = now() + RESERVATION_TTL_MS`.
Booking Service записывает бронирование и вызывает `ConfirmReservation` в одной транзакции
своей БД: подтверждение уходит до `COMMIT`.

- Не удалось записать бронь или подтвердить резервацию — транзакция откатывается,
  брони нет, места освобождаются сразу (`ReleaseReservation`).
- Booking Service упал посередине — брони нет, а неподтверждённую резервацию освободит
  фоновая задача Flight Service (`src/flight_service/reservation_sweeper.cpp`,
  `TPeriodicExecutor`). Раз в `RESERVATION_SWEEP_PERIOD_MS` она для каждого рейса с
  просроченными резервациями делает одну транзакцию: `SELECT рейс FOR UPDATE SKIP LOCKED`
  → резервации `FOR UPDATE SKIP LOCKED` → `EXPIRED` → возврат мест. Порядок блокировок
  (рейс → резервации) тот же, что у `ReserveSeats`/`ReleaseReservation`, поэтому
  дедлоков нет. `SKIP LOCKED` позволяет запускать несколько реплик Flight Service
  без двойной обработки.

## Проверка отказоустойчивости

Всё ниже автоматизировано в `scripts/e2e.sh` (запускать после `docker compose up -d`).

```bash
# Логи кеша
docker compose logs -f flight-service | grep Cache

# Retry + Circuit Breaker: останавливаем Flight Service и шлём запросы
docker compose stop flight-service
for i in $(seq 6); do curl -s -o /dev/null -w '%{http_code} %{time_total}s\n' localhost:8080/flights/1; done
docker compose logs booking-service | grep -E 'Retry|Circuit breaker'
docker compose start flight-service   # через CB_OPEN_TIMEOUT_MS: HALF_OPEN -> CLOSED

# Redis failover: кеш продолжает работать с новым мастером
docker compose stop redis-master
docker compose logs -f flight-service | grep Redis
```

## Локальная разработка

```bash
scripts/prepare-ytsaurus.sh        # ytsaurus + патч + musl в third_party/
scripts/build-local.sh             # ya make всех программ -> .build/bin/
scripts/gen-compile-commands.sh    # compile_commands.json для clangd
```

В VS Code сборка по Ctrl+Shift+B (`.vscode/tasks.json`), навигация — через clangd.

### Структура

```
src/
  proto/              gRPC-контракт и прокси YT RPC
  lib/common/         bootstrap (SIGTERM, логи), env, бизнес-ошибки, макросы ошибок
  lib/net/            TCP-соединение с буфером, пул соединений
  lib/pg/             клиент PostgreSQL
  lib/redis/          клиент Redis + Sentinel
  lib/rpc/            interceptor'ы: API key, retry, circuit breaker
  flight_service/     gRPC-сервис, репозиторий (Postgres) и декоратор с Redis
  booking_service/    REST API, бизнес-логика, клиент Flight Service, репозиторий
  tools/              smoke-тесты клиентов Postgres/Redis
migrations/           Flyway
deploy/               сертификаты, pg_hba, конфиг Sentinel
patches/              патч ytsaurus и зафиксированные коммиты зависимостей
```

Ошибки в коде — значения (`TErrorOr<T>` + `SOA_RETURN_IF_ERROR` / `SOA_ASSIGN_OR_RETURN`),
исключения не используются.
