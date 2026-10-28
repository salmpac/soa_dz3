-- Flight Service: справочники, рейсы и резервации мест.

CREATE TABLE airlines (
    code CHAR(2) PRIMARY KEY CHECK (code ~ '^[A-Z0-9]{2}$'),
    name TEXT NOT NULL CHECK (length(btrim(name)) > 0)
);

CREATE TABLE airports (
    iata_code CHAR(3) PRIMARY KEY CHECK (iata_code ~ '^[A-Z]{3}$'),
    name TEXT NOT NULL CHECK (length(btrim(name)) > 0),
    city TEXT NOT NULL CHECK (length(btrim(city)) > 0)
);

CREATE TYPE flight_status AS ENUM ('SCHEDULED', 'DEPARTED', 'CANCELLED', 'COMPLETED');

-- Номер рейса хранится как (airline_code, flight_no): "SU1234" = 'SU' + 1234.
-- Хранить строку "SU1234" целиком вместе с airline_code нельзя: код авиакомпании
-- функционально зависел бы от части ключа (нарушение 2NF/3NF).
CREATE TABLE flights (
    id BIGINT GENERATED ALWAYS AS IDENTITY PRIMARY KEY,
    airline_code CHAR(2) NOT NULL REFERENCES airlines (code),
    flight_no INTEGER NOT NULL CHECK (flight_no BETWEEN 1 AND 9999),
    origin CHAR(3) NOT NULL REFERENCES airports (iata_code),
    destination CHAR(3) NOT NULL REFERENCES airports (iata_code),
    departure_time TIMESTAMPTZ NOT NULL,
    arrival_time TIMESTAMPTZ NOT NULL,
    total_seats INTEGER NOT NULL CHECK (total_seats > 0),
    available_seats INTEGER NOT NULL CHECK (available_seats >= 0),
    -- Цена в минимальных единицах валюты (копейках).
    price_amount BIGINT NOT NULL CHECK (price_amount > 0),
    currency CHAR(3) NOT NULL DEFAULT 'RUB' CHECK (currency ~ '^[A-Z]{3}$'),
    status flight_status NOT NULL DEFAULT 'SCHEDULED',

    CONSTRAINT flights_available_le_total CHECK (available_seats <= total_seats),
    CONSTRAINT flights_distinct_airports CHECK (origin <> destination),
    CONSTRAINT flights_arrival_after_departure CHECK (arrival_time > departure_time)
);

-- Номер рейса + дата вылета (UTC) уникальны: SU1234 на 2026-04-01 - один конкретный рейс.
CREATE UNIQUE INDEX flights_number_departure_date_key
    ON flights (airline_code, flight_no, ((departure_time AT TIME ZONE 'UTC')::date));

-- Поиск по маршруту и дате среди рейсов в продаже.
CREATE INDEX flights_search_idx
    ON flights (origin, destination, departure_time)
    WHERE status = 'SCHEDULED';

CREATE TYPE reservation_status AS ENUM ('ACTIVE', 'RELEASED', 'EXPIRED');

CREATE TABLE seat_reservations (
    id BIGINT GENERATED ALWAYS AS IDENTITY PRIMARY KEY,
    flight_id BIGINT NOT NULL REFERENCES flights (id),
    -- Идентификатор бронирования из Booking Service: одна резервация на бронирование.
    booking_id UUID NOT NULL UNIQUE,
    seat_count INTEGER NOT NULL CHECK (seat_count > 0),
    status reservation_status NOT NULL DEFAULT 'ACTIVE',
    created_at TIMESTAMPTZ NOT NULL DEFAULT now(),
    updated_at TIMESTAMPTZ NOT NULL DEFAULT now(),

    CONSTRAINT seat_reservations_updated_after_created CHECK (updated_at >= created_at)
);

CREATE INDEX seat_reservations_flight_idx ON seat_reservations (flight_id);
