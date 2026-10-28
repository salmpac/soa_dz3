#!/usr/bin/env bash
# Сквозная проверка поднятого docker compose: REST-сценарии, кеш, администрирование
# рейсов, retry, circuit breaker, failover Redis. Запуск: scripts/e2e.sh (после `docker compose up -d`).
#
# NB: логи проверяются через `grep -q ... <<< "$LOGS"`, а не через пайп: с pipefail
# ранний выход grep -q даёт SIGPIPE у echo и ложный провал.
set -uo pipefail

API="${API:-http://localhost:8080}"
COMPOSE=(docker compose)
FAILED=0

pass() { printf '  \e[32mPASS\e[0m %s\n' "$1"; }
fail() { printf '  \e[31mFAIL\e[0m %s\n' "$1"; FAILED=1; }

ADMIN_TOKEN="${ADMIN_TOKEN:-admin}"

# request <method> <path> [json] -> STATUS, BODY
request() {
    local method="$1" path="$2" data="${3:-}"
    local args=(-s -o /tmp/e2e-body -w '%{http_code}' -X "$method" "$API$path")
    [ -n "$data" ] && args+=(-H 'Content-Type: application/json' -d "$data")
    STATUS="$(curl "${args[@]}" "${EXTRA_HEADERS[@]}")"
    BODY="$(cat /tmp/e2e-body)"
}
EXTRA_HEADERS=()

# Как request, но с токеном администратора.
admin_request() {
    EXTRA_HEADERS=(-H "X-Admin-Token: $ADMIN_TOKEN")
    request "$@"
    EXTRA_HEADERS=()
}

expect_status() {
    local expected="$1" what="$2"
    if [ "$STATUS" = "$expected" ]; then pass "$what ($STATUS)"; else fail "$what: expected $expected, got $STATUS: $BODY"; fi
}

json() { python3 -c "import sys, json; d = json.loads(sys.argv[1]); print($2)" "$BODY"; }

wait_ready() {
    for _ in $(seq 60); do
        request GET "/flights?origin=SVO&destination=LED"
        [ "$STATUS" = 200 ] && return 0
        sleep 1
    done
    echo "Booking Service is not ready" >&2
    exit 1
}

echo "== Ожидание готовности"
wait_ready

echo "== Рейсы"
request GET "/flights?origin=svo&destination=led"
expect_status 200 "поиск рейсов"
FLIGHT_ID="$(json "$BODY" 'd[0]["id"]')"
PRICE="$(json "$BODY" 'd[0]["price"]["amount_minor"]')"
SEATS_BEFORE="$(json "$BODY" 'd[0]["available_seats"]')"
request GET "/flights?origin=SVO"
expect_status 400 "поиск без destination"
request GET "/flights/$FLIGHT_ID"
expect_status 200 "рейс по id"
request GET "/flights/999999"
expect_status 404 "несуществующий рейс"
request GET "/flights?origin=SVO&destination=LED"
if json "$BODY" '"OK" if all(f["departure_time"] > __import__("datetime").datetime.now(__import__("datetime").timezone.utc).strftime("%Y-%m-%dT%H:%M:%SZ") for f in d) else "PAST"' | grep -q OK; then pass "в поиске только будущие рейсы"; else fail "в поиске есть вылетевшие рейсы"; fi
DEPARTED_ID="$("${COMPOSE[@]}" exec -T flight-db psql -U "${POSTGRES_USER:-soa}" -d flights -tAc "SELECT id FROM flights WHERE departure_time < now() AND status = 'SCHEDULED' AND available_seats > 0 LIMIT 1")"
if [ -n "$DEPARTED_ID" ]; then
    request POST /bookings "{\"user_id\":\"e2e-user\",\"flight_id\":$DEPARTED_ID,\"passenger_name\":\"E2E\",\"passenger_email\":\"e2e@example.com\",\"seat_count\":1}"
    expect_status 409 "бронирование вылетевшего рейса"
fi

echo "== Бронирование"
request POST /bookings "{\"user_id\":\"e2e-user\",\"flight_id\":$FLIGHT_ID,\"passenger_name\":\"E2E Test\",\"passenger_email\":\"e2e@example.com\",\"seat_count\":2}"
expect_status 201 "создание бронирования"
BOOKING_ID="$(json "$BODY" 'd["id"]')"
TOTAL="$(json "$BODY" 'd["total_price"]["amount_minor"]')"
if [ "$TOTAL" = "$((PRICE * 2))" ]; then pass "total_price = seat_count * price ($TOTAL)"; else fail "total_price $TOTAL != 2 * $PRICE"; fi
request GET "/flights/$FLIGHT_ID"
SEATS_AFTER="$(json "$BODY" 'd["available_seats"]')"
if [ "$SEATS_AFTER" = "$((SEATS_BEFORE - 2))" ]; then pass "места списаны ($SEATS_BEFORE -> $SEATS_AFTER), кеш инвалидирован"; else fail "available_seats $SEATS_AFTER, ожидалось $((SEATS_BEFORE - 2))"; fi

CONFIRMED_RESERVATION="$("${COMPOSE[@]}" exec -T flight-db psql -U "${POSTGRES_USER:-soa}" -d flights -tAc \
    "SELECT status || ':' || (confirmed_at IS NOT NULL) || ':' || (expires_at IS NULL) FROM seat_reservations WHERE booking_id = '$BOOKING_ID'")"
if [ "$CONFIRMED_RESERVATION" = "ACTIVE:true:true" ]; then pass "резервация подтверждена (без срока жизни)"; else fail "резервация: $CONFIRMED_RESERVATION"; fi

request GET "/bookings/$BOOKING_ID"
expect_status 200 "бронирование по id"
request GET "/bookings?user_id=e2e-user"
expect_status 200 "бронирования пользователя"
request POST /bookings "{\"user_id\":\"e2e-user\",\"flight_id\":$FLIGHT_ID,\"passenger_name\":\"E2E\",\"passenger_email\":\"bad\",\"seat_count\":1}"
expect_status 400 "невалидный email"
request POST /bookings "{\"user_id\":\"e2e-user\",\"flight_id\":999999,\"passenger_name\":\"E2E\",\"passenger_email\":\"e2e@example.com\",\"seat_count\":1}"
expect_status 404 "бронирование на несуществующий рейс"

echo "== Нехватка мест и гонка за последние места"
request GET "/flights?origin=SVO&destination=AER"
SMALL_ID="$(json "$BODY" '[f["id"] for f in d if f["total_seats"] == 3 and f["available_seats"] == 3][0]')"
request POST /bookings "{\"user_id\":\"e2e-race\",\"flight_id\":$SMALL_ID,\"passenger_name\":\"E2E\",\"passenger_email\":\"e2e@example.com\",\"seat_count\":4}"
expect_status 409 "4 места на рейс с 3 местами"
RACE_DIR="$(mktemp -d)"
seq 10 | xargs -P 10 -I{} curl -s -o "$RACE_DIR/{}.json" -w '%{http_code}\n' -X POST "$API/bookings" \
    -H 'Content-Type: application/json' \
    -d "{\"user_id\":\"e2e-race\",\"flight_id\":$SMALL_ID,\"passenger_name\":\"E2E\",\"passenger_email\":\"e2e@example.com\",\"seat_count\":1}" \
    > "$RACE_DIR/codes"
RESULTS="$(sort "$RACE_DIR/codes" | uniq -c | tr '\n' ' ')"
if grep -q ' 3 201' <<< "$RESULTS" && grep -q ' 7 409' <<< "$RESULTS"; then pass "10 параллельных броней на 3 места: $RESULTS"; else fail "гонка: $RESULTS"; fi
# Убираем за собой: отменяем созданные бронирования, чтобы тест можно было повторять.
for file in "$RACE_DIR"/*.json; do
    RACE_BOOKING="$(python3 -c "import json, sys; print(json.load(open(sys.argv[1])).get('id', ''))" "$file" 2>/dev/null)"
    [ -n "$RACE_BOOKING" ] && request POST "/bookings/$RACE_BOOKING/cancel"
done
request GET "/flights/$SMALL_ID"
if [ "$(json "$BODY" 'd["available_seats"]')" = 3 ]; then pass "тестовые бронирования отменены, места вернулись"; else fail "места после отмены: $BODY"; fi
rm -rf "$RACE_DIR"

echo "== Отмена"
request POST "/bookings/$BOOKING_ID/cancel"
expect_status 200 "отмена"
request POST "/bookings/$BOOKING_ID/cancel"
expect_status 409 "повторная отмена"
request GET "/flights/$FLIGHT_ID"
if [ "$(json "$BODY" 'd["available_seats"]')" = "$SEATS_BEFORE" ]; then pass "места возвращены"; else fail "места не вернулись"; fi

echo "== Кеш (логи Flight Service)"
request GET "/flights/$FLIGHT_ID"
request GET "/flights/$FLIGHT_ID"
LOGS="$("${COMPOSE[@]}" logs flight-service 2>&1)"
grep -q "Cache hit (Key: flight:$FLIGHT_ID)" <<< "$LOGS" && pass "cache hit в логах" || fail "нет cache hit"
grep -q "Cache miss" <<< "$LOGS" && pass "cache miss в логах" || fail "нет cache miss"
grep -q "Cache invalidated" <<< "$LOGS" && pass "инвалидация в логах" || fail "нет инвалидации"

echo "== Администрирование рейсов"
request GET /admin/airports
expect_status 401 "админка без токена"
EXTRA_HEADERS=(-H "X-Admin-Token: wrong"); request GET /admin/airports; EXTRA_HEADERS=()
expect_status 401 "админка с неверным токеном"
admin_request GET /admin/airports
expect_status 200 "справочник аэропортов"
admin_request GET /admin/airlines
expect_status 200 "справочник авиакомпаний"

NEW_DATE="$(date -u -d '+5 days' +%Y-%m-%d)"
# Прогреваем кеш поиска: новый рейс должен появиться в нём сразу после создания.
request GET "/flights?origin=SVO&destination=KZN&date=$NEW_DATE"
new_flight_body() {
    printf '{"airline_code":"SU","flight_no":%s,"origin":"%s","destination":"%s","departure_time":"%sT13:00:00Z","arrival_time":"%sT14:30:00Z","total_seats":5,"price":{"amount_minor":123400,"currency":"RUB"}}' \
        "$1" "$2" "$3" "$NEW_DATE" "$NEW_DATE"
}
# Номер рейса случайный; при совпадении с рейсом прошлого прогона пробуем другой.
for _ in $(seq 5); do
    NEW_NO=$((7000 + RANDOM % 1000))
    admin_request POST /admin/flights "$(new_flight_body "$NEW_NO" SVO KZN)"
    [ "$STATUS" != 409 ] && break
done
expect_status 201 "создание рейса SU$NEW_NO"
NEW_ID="$(json "$BODY" 'd["id"]')"
request GET "/flights?origin=SVO&destination=KZN&date=$NEW_DATE"
if json "$BODY" "'OK' if any(f['id'] == $NEW_ID for f in d) else 'MISSING'" | grep -q OK; then pass "новый рейс сразу виден в поиске (кеш сброшен)"; else fail "нового рейса нет в поиске"; fi
admin_request POST /admin/flights "$(new_flight_body "$NEW_NO" SVO KZN)"
expect_status 409 "повторное создание того же рейса"
admin_request POST /admin/flights "$(new_flight_body $((NEW_NO + 1)) SVO XXX)"
expect_status 400 "неизвестный аэропорт"
admin_request POST /admin/flights "$(new_flight_body $((NEW_NO + 1)) SVO SVO)"
expect_status 400 "вылет и прилёт в одном аэропорту"
admin_request POST "/admin/flights/$NEW_ID" '{"price":{"amount_minor":99900,"currency":"RUB"}}'
if [ "$STATUS" = 200 ] && [ "$(json "$BODY" 'd["price"]["amount_minor"]')" = 99900 ]; then pass "смена цены"; else fail "смена цены: $STATUS $BODY"; fi

request POST /bookings "{\"user_id\":\"e2e-user\",\"flight_id\":$NEW_ID,\"passenger_name\":\"E2E\",\"passenger_email\":\"e2e@example.com\",\"seat_count\":2}"
expect_status 201 "бронирование на новый рейс"
if [ "$(json "$BODY" 'd["total_price"]["amount_minor"]')" = 199800 ]; then pass "цена брони по новой цене рейса"; else fail "цена брони: $BODY"; fi
request POST "/bookings/$(json "$BODY" 'd["id"]')/cancel"
expect_status 200 "отмена брони на новом рейсе"

admin_request POST "/admin/flights/$NEW_ID" '{"status":"CANCELLED"}'
expect_status 200 "отмена рейса"
request GET "/flights?origin=SVO&destination=KZN&date=$NEW_DATE"
if json "$BODY" "'GONE' if all(f['id'] != $NEW_ID for f in d) else 'PRESENT'" | grep -q GONE; then pass "отменённый рейс пропал из поиска"; else fail "отменённый рейс остался в поиске"; fi
request POST /bookings "{\"user_id\":\"e2e-user\",\"flight_id\":$NEW_ID,\"passenger_name\":\"E2E\",\"passenger_email\":\"e2e@example.com\",\"seat_count\":1}"
expect_status 409 "бронирование отменённого рейса"

echo "== Retry + Circuit Breaker (Flight Service остановлен)"
"${COMPOSE[@]}" stop flight-service >/dev/null 2>&1
for i in $(seq 4); do
    request GET "/flights/$FLIGHT_ID"
done
expect_status 503 "Flight Service недоступен"
START="$(date +%s%N)"; request GET "/flights/$FLIGHT_ID"; ELAPSED=$(( ($(date +%s%N) - START) / 1000000 ))
if [ "$STATUS" = 503 ] && [ "$ELAPSED" -lt 50 ]; then pass "OPEN: мгновенный 503 (${ELAPSED}ms)"; else fail "OPEN: $STATUS за ${ELAPSED}ms"; fi
LOGS="$("${COMPOSE[@]}" logs booking-service 2>&1)"
grep -q "retriable error" <<< "$LOGS" && pass "retry в логах" || fail "нет retry в логах"
grep -q "Closed -> Open" <<< "$LOGS" && pass "CLOSED -> OPEN в логах" || fail "нет перехода в OPEN"
"${COMPOSE[@]}" start flight-service >/dev/null 2>&1
echo "  ждём CB_OPEN_TIMEOUT_MS..."
for _ in $(seq 40); do
    request GET "/flights/$FLIGHT_ID"
    [ "$STATUS" = 200 ] && break
    sleep 1
done
expect_status 200 "восстановление после старта Flight Service"
LOGS="$("${COMPOSE[@]}" logs booking-service 2>&1)"
grep -q "HalfOpen -> Closed" <<< "$LOGS" && pass "HALF_OPEN -> CLOSED в логах" || fail "нет перехода в CLOSED"

echo "== Redis failover"
MASTER="$("${COMPOSE[@]}" exec -T redis-sentinel-1 redis-cli --tls --cacert /certs/ca.crt -p 26379 sentinel get-master-addr-by-name mymaster | head -1)"
echo "  текущий мастер: $MASTER, останавливаем"
"${COMPOSE[@]}" stop "$MASTER" >/dev/null 2>&1
for _ in $(seq 30); do
    NEW_MASTER="$("${COMPOSE[@]}" exec -T redis-sentinel-1 redis-cli --tls --cacert /certs/ca.crt -p 26379 sentinel get-master-addr-by-name mymaster | head -1)"
    [ "$NEW_MASTER" != "$MASTER" ] && break
    sleep 1
done
[ "$NEW_MASTER" != "$MASTER" ] && pass "sentinel переключил мастер на $NEW_MASTER" || fail "failover не произошёл"
request GET "/flights/$FLIGHT_ID"
request GET "/flights/$FLIGHT_ID"
expect_status 200 "рейсы доступны после failover"
LOGS="$("${COMPOSE[@]}" logs flight-service 2>&1)"
grep -q "Master: $NEW_MASTER" <<< "$LOGS" && pass "Flight Service нашёл новый мастер" || fail "Flight Service не переключился"
"${COMPOSE[@]}" start "$MASTER" >/dev/null 2>&1

echo
if [ "$FAILED" = 0 ]; then echo "ALL CHECKS PASSED"; else echo "SOME CHECKS FAILED"; exit 1; fi
