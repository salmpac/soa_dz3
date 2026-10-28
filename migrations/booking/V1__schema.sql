-- Booking Service: бронирования.

CREATE TYPE booking_status AS ENUM ('CONFIRMED', 'CANCELLED');

CREATE TABLE bookings (
    -- UUID генерирует Booking Service до вызова ReserveSeats:
    -- он же служит ключом идемпотентности резервации во Flight Service.
    id UUID PRIMARY KEY,
    user_id VARCHAR(64) NOT NULL CHECK (length(btrim(user_id)) > 0),
    -- Ссылка на рейс из другого сервиса (другой БД), поэтому без FOREIGN KEY.
    flight_id BIGINT NOT NULL CHECK (flight_id > 0),
    passenger_name VARCHAR(200) NOT NULL CHECK (length(btrim(passenger_name)) > 0),
    passenger_email VARCHAR(254) NOT NULL CHECK (passenger_email ~ '^[^@[:space:]]+@[^@[:space:]]+\.[^@[:space:]]+$'),
    seat_count INTEGER NOT NULL CHECK (seat_count > 0),
    -- Цена, зафиксированная на момент бронирования: seat_count * цена рейса.
    total_price_amount BIGINT NOT NULL CHECK (total_price_amount > 0),
    currency CHAR(3) NOT NULL CHECK (currency ~ '^[A-Z]{3}$'),
    status booking_status NOT NULL DEFAULT 'CONFIRMED',
    created_at TIMESTAMPTZ NOT NULL DEFAULT now(),
    updated_at TIMESTAMPTZ NOT NULL DEFAULT now(),

    CONSTRAINT bookings_updated_after_created CHECK (updated_at >= created_at)
);

CREATE INDEX bookings_user_idx ON bookings (user_id, created_at DESC);
