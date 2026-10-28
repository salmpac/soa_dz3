-- Резервация с таймаутом: ReserveSeats создаёт неподтверждённую резервацию
-- с expires_at; Booking Service подтверждает её (ConfirmReservation) в той же
-- транзакции, где записывает бронирование. Неподтверждённые просроченные
-- резервации фоновая задача Flight Service переводит в EXPIRED и возвращает места.

ALTER TABLE seat_reservations
    ADD COLUMN expires_at TIMESTAMPTZ,
    ADD COLUMN confirmed_at TIMESTAMPTZ;

-- Резервации, созданные до этой миграции, принадлежат существующим бронированиям.
UPDATE seat_reservations SET confirmed_at = created_at;

ALTER TABLE seat_reservations
    -- Активная резервация либо подтверждена, либо имеет срок жизни.
    ADD CONSTRAINT seat_reservations_pending_has_expiry
        CHECK (status <> 'ACTIVE' OR confirmed_at IS NOT NULL OR expires_at IS NOT NULL),
    ADD CONSTRAINT seat_reservations_confirmed_after_created
        CHECK (confirmed_at IS NULL OR confirmed_at >= created_at),
    ADD CONSTRAINT seat_reservations_expires_after_created
        CHECK (expires_at IS NULL OR expires_at > created_at);

-- Для фоновой задачи: только неподтверждённые активные резервации.
CREATE INDEX seat_reservations_pending_expiry_idx
    ON seat_reservations (expires_at)
    WHERE status = 'ACTIVE' AND confirmed_at IS NULL;
