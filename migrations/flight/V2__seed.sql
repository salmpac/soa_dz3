-- Тестовые данные: авиакомпании, аэропорты и рейсы на ближайшие даты.

INSERT INTO airlines (code, name) VALUES
    ('SU', 'Aeroflot'),
    ('S7', 'S7 Airlines'),
    ('DP', 'Pobeda'),
    ('U6', 'Ural Airlines');

INSERT INTO airports (iata_code, name, city) VALUES
    ('SVO', 'Sheremetyevo', 'Moscow'),
    ('VKO', 'Vnukovo', 'Moscow'),
    ('DME', 'Domodedovo', 'Moscow'),
    ('LED', 'Pulkovo', 'Saint Petersburg'),
    ('KZN', 'Kazan International', 'Kazan'),
    ('AER', 'Sochi International', 'Sochi'),
    ('OVB', 'Tolmachevo', 'Novosibirsk');

-- Пример из задания: SU1234 SVO -> LED на 2026-04-01.
INSERT INTO flights (airline_code, flight_no, origin, destination, departure_time, arrival_time,
                     total_seats, available_seats, price_amount, currency)
VALUES ('SU', 1234, 'SVO', 'LED', '2026-04-01 09:00:00+03', '2026-04-01 10:30:00+03', 150, 150, 550000, 'RUB');

-- Расписание на 30 дней вперёд от даты миграции.
INSERT INTO flights (airline_code, flight_no, origin, destination, departure_time, arrival_time,
                     total_seats, available_seats, price_amount, currency)
SELECT
    route.airline_code,
    route.flight_no,
    route.origin,
    route.destination,
    d.day_utc + route.departure_offset,
    d.day_utc + route.departure_offset + route.duration,
    route.total_seats,
    route.total_seats,
    route.price_amount,
    'RUB'
FROM generate_series(date_trunc('day', now() AT TIME ZONE 'UTC') + interval '1 day',
                     date_trunc('day', now() AT TIME ZONE 'UTC') + interval '30 days',
                     interval '1 day') AS day
CROSS JOIN (VALUES
    ('SU', 1010, 'SVO', 'LED', interval '06 hours', interval '1 hour 30 minutes', 150, 650000),
    ('SU', 1011, 'LED', 'SVO', interval '15 hours', interval '1 hour 30 minutes', 150, 620000),
    ('S7', 2020, 'DME', 'LED', interval '09 hours', interval '1 hour 25 minutes', 120, 480000),
    ('DP', 3030, 'VKO', 'LED', interval '12 hours', interval '1 hour 35 minutes', 180, 299000),
    ('DP', 3031, 'VKO', 'AER', interval '07 hours', interval '2 hours 20 minutes', 180, 450000),
    ('U6', 4040, 'SVO', 'KZN', interval '10 hours', interval '1 hour 20 minutes', 100, 390000),
    ('S7', 2050, 'DME', 'OVB', interval '20 hours', interval '4 hours',            160, 1150000),
    -- Рейс с тремя местами: удобно проверять RESOURCE_EXHAUSTED и гонку за последнее место.
    ('SU', 1099, 'SVO', 'AER', interval '18 hours', interval '2 hours 15 minutes', 3,   990000)
) AS route (airline_code, flight_no, origin, destination, departure_offset, duration, total_seats, price_amount)
-- day имеет тип timestamp без зоны: трактуем его как полночь UTC.
CROSS JOIN LATERAL (SELECT day AT TIME ZONE 'UTC' AS day_utc) AS d;
