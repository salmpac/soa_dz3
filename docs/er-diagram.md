# ER-диаграмма

У каждого сервиса своя БД. Связь `bookings.flight_id → flights.id` и
`seat_reservations.booking_id ↔ bookings.id` логическая (между разными БД
внешних ключей нет), поэтому на диаграмме она пунктирная.

```mermaid
erDiagram
    %% ---------------- Flight Service (БД flights) ----------------
    AIRLINES {
        char2 code PK "IATA-код авиакомпании, ^[A-Z0-9]{2}$"
        text name "NOT NULL, не пустое"
    }

    AIRPORTS {
        char3 iata_code PK "^[A-Z]{3}$"
        text name "NOT NULL, не пустое"
        text city "NOT NULL, не пустое"
    }

    FLIGHTS {
        bigint id PK "identity"
        char2 airline_code FK "-> airlines.code"
        int flight_no "1..9999; номер рейса = airline_code || flight_no"
        char3 origin FK "-> airports.iata_code"
        char3 destination FK "-> airports.iata_code, <> origin"
        timestamptz departure_time "NOT NULL"
        timestamptz arrival_time "NOT NULL, > departure_time"
        int total_seats "> 0"
        int available_seats ">= 0, <= total_seats"
        bigint price_amount "> 0, в копейках"
        char3 currency "ISO 4217"
        flight_status status "SCHEDULED | DEPARTED | CANCELLED | COMPLETED"
    }

    SEAT_RESERVATIONS {
        bigint id PK "identity"
        bigint flight_id FK "-> flights.id"
        uuid booking_id UK "одна резервация на бронирование"
        int seat_count "> 0"
        reservation_status status "ACTIVE | RELEASED | EXPIRED"
        timestamptz created_at "NOT NULL"
        timestamptz updated_at ">= created_at"
        timestamptz expires_at "срок неподтверждённой резервации"
        timestamptz confirmed_at "NULL - резервация временная"
    }

    AIRLINES ||--o{ FLIGHTS : "выполняет"
    AIRPORTS ||--o{ FLIGHTS : "вылет из (origin)"
    AIRPORTS ||--o{ FLIGHTS : "прилёт в (destination)"
    FLIGHTS ||--o{ SEAT_RESERVATIONS : "резервирует места"

    %% ---------------- Booking Service (БД bookings) ----------------
    BOOKINGS {
        uuid id PK "генерирует Booking Service"
        varchar64 user_id "NOT NULL, не пустой"
        bigint flight_id "> 0, ссылка на flights.id (другая БД)"
        varchar200 passenger_name "NOT NULL, не пустое"
        varchar254 passenger_email "формат email"
        int seat_count "> 0"
        bigint total_price_amount "> 0; seat_count * цена на момент брони"
        char3 currency "ISO 4217"
        booking_status status "CONFIRMED | CANCELLED"
        timestamptz created_at "NOT NULL"
        timestamptz updated_at ">= created_at"
    }

    BOOKINGS |o..o| SEAT_RESERVATIONS : "booking_id (между сервисами)"
    BOOKINGS }o..|| FLIGHTS : "flight_id (между сервисами)"
```

## Ограничения целостности

| Таблица | Ограничение |
|---|---|
| `flights` | `available_seats >= 0`, `available_seats <= total_seats`, `total_seats > 0`, `price_amount > 0` |
| `flights` | `origin <> destination`, `arrival_time > departure_time` |
| `flights` | `UNIQUE (airline_code, flight_no, (departure_time AT TIME ZONE 'UTC')::date)` — номер рейса + дата вылета уникальны |
| `seat_reservations` | `seat_count > 0`, `UNIQUE (booking_id)`, `updated_at >= created_at` |
| `seat_reservations` | активная резервация либо подтверждена (`confirmed_at`), либо имеет срок (`expires_at`); `confirmed_at >= created_at`, `expires_at > created_at` |
| `bookings` | `seat_count > 0`, `total_price_amount > 0`, формат email, `updated_at >= created_at` |
| справочники | формат IATA-кодов (`^[A-Z]{3}$` для аэропортов, `^[A-Z0-9]{2}$` для авиакомпаний) и ISO 4217 для валют |

## 3NF

- **1NF.** Все атрибуты атомарны. Номер рейса не хранится строкой `"SU1234"`:
  он разложен на `airline_code` + `flight_no`.
- **2NF.** Во всех таблицах суррогатный первичный ключ из одного столбца
  (`id`, `code`, `iata_code`), поэтому частичных зависимостей от составного ключа нет.
  Альтернативный ключ рейса — `(airline_code, flight_no, дата вылета)`. Если бы
  хранилась строка `"SU1234"`, `airline_code` зависел бы от её части, и это
  нарушило бы 2NF. Поэтому номер разложен.
- **3NF.** Нет транзитивных зависимостей от ключа:
  - название авиакомпании зависит от `airline_code`, поэтому вынесено в `airlines`;
  - название и город аэропорта зависят от IATA-кода, поэтому вынесены в `airports`;
  - в `seat_reservations` нет данных рейса, только `flight_id`;
  - `bookings.total_price_amount` не вычисляется из других атрибутов строки:
    это снимок цены на момент бронирования (цена рейса потом может измениться),
    а не производное поле.
  - Дата вылета отдельно не хранится: для уникальности используется индекс
    по выражению от `departure_time`.
