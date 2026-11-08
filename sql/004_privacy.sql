-- Semâ schema, version 4: identity retention (KVKK / GDPR data minimisation).
--
-- An ICAO24 address or a registration-style callsign can point at the owner of
-- a private aircraft, so identifiers are only kept as long as they are useful.
-- After `keep` (SEMA_IDENTITY_DAYS, default 30 days) the long-lived tables are
-- anonymised:
--   * flights:          icao24 -> per-day pseudonym, callsign removed
--   * positions_hourly: icao24 -> per-day pseudonym, callsign and track removed
--   * anomaly_events:   icao24 -> per-day pseudonym, callsign and track removed
-- Raw `positions` are already deleted after SEMA_RETENTION_HOURS.
--
-- A pseudonym is '~' + 8 hex digits of md5(address, local day, salt), where the
-- salt is random for each call and never stored. Within one call every row of
-- a day gets the same pseudonym for the same aircraft, so per-day "distinct
-- aircraft" counts stay right; across days and calls nothing can be linked.
-- Whole local days are processed at once for that reason.

-- Pseudonyms are longer than an address.
DO $$
BEGIN
    IF (SELECT data_type FROM information_schema.columns
        WHERE table_name = 'flights' AND column_name = 'icao24') <> 'text' THEN
        DROP VIEW IF EXISTS daily_traffic;
        ALTER TABLE flights ALTER COLUMN icao24 TYPE text;
        ALTER TABLE positions_hourly ALTER COLUMN icao24 TYPE text;
        ALTER TABLE anomaly_events ALTER COLUMN icao24 TYPE text;
    END IF;
END;
$$;

-- Recreated here because the type change above has to drop it.
CREATE OR REPLACE VIEW daily_traffic AS
SELECT (first_seen AT TIME ZONE 'Europe/Istanbul')::date AS day,
       count(*)                 AS flights,
       count(DISTINCT icao24)   AS aircraft,
       count(departure)         AS departures,
       count(arrival)           AS arrivals,
       round(avg(extract(epoch FROM last_seen - first_seen) / 60.0)::numeric, 1) AS avg_minutes_seen
FROM flights
WHERE airborne AND fixes >= 2
GROUP BY 1
ORDER BY 1;

ALTER TABLE flights          ADD COLUMN IF NOT EXISTS anonymized boolean NOT NULL DEFAULT false;
ALTER TABLE positions_hourly ADD COLUMN IF NOT EXISTS anonymized boolean NOT NULL DEFAULT false;
ALTER TABLE anomaly_events   ADD COLUMN IF NOT EXISTS anonymized boolean NOT NULL DEFAULT false;
CREATE INDEX IF NOT EXISTS flights_identified_idx ON flights (first_seen) WHERE NOT anonymized;
CREATE INDEX IF NOT EXISTS positions_hourly_identified_idx ON positions_hourly (hour) WHERE NOT anonymized;
CREATE INDEX IF NOT EXISTS anomaly_events_identified_idx ON anomaly_events (start_time) WHERE NOT anonymized;

-- Anonymises every whole local day that ended more than `keep` ago.
-- Returns the number of rows changed.
CREATE OR REPLACE FUNCTION sema_anonymize_identities(keep interval)
RETURNS bigint LANGUAGE plpgsql AS $$
DECLARE
    cutoff  timestamptz := date_trunc('day', (now() - keep) AT TIME ZONE 'Europe/Istanbul') AT TIME ZONE 'Europe/Istanbul';
    salt    text := gen_random_uuid()::text || clock_timestamp()::text;
    changed bigint := 0;
    n       bigint;
BEGIN
    UPDATE flights
    SET icao24 = '~' || substr(md5(icao24 || '/' || (first_seen AT TIME ZONE 'Europe/Istanbul')::date || '/' || salt), 1, 8),
        callsign = NULL,
        anonymized = true
    WHERE NOT anonymized AND first_seen < cutoff;
    GET DIAGNOSTICS n = ROW_COUNT;
    changed := changed + n;

    UPDATE positions_hourly
    SET icao24 = '~' || substr(md5(icao24 || '/' || (hour AT TIME ZONE 'Europe/Istanbul')::date || '/' || salt), 1, 8),
        callsign = NULL,
        track = NULL,
        anonymized = true
    WHERE NOT anonymized AND hour < cutoff;
    GET DIAGNOSTICS n = ROW_COUNT;
    changed := changed + n;

    UPDATE anomaly_events e
    SET icao24 = p.pseudo,
        key = e.kind || ':' || p.pseudo || ':' || extract(epoch FROM e.start_time)::bigint,
        callsign = NULL,
        track = NULL,
        anonymized = true
    FROM (SELECT key AS old_key,
                 '~' || substr(md5(icao24 || '/' || (start_time AT TIME ZONE 'Europe/Istanbul')::date || '/' || salt), 1, 8) AS pseudo
          FROM anomaly_events
          WHERE NOT anonymized AND end_time < cutoff AND NOT active) p
    WHERE e.key = p.old_key;
    GET DIAGNOSTICS n = ROW_COUNT;
    changed := changed + n;

    RETURN changed;
END;
$$;

-- Removal request (privacy page, "Uçağınızı kaldırma"): deletes everything
-- stored about one aircraft, by ICAO24 address or callsign. Add the same entry
-- to data/optout.txt so that nothing new is stored. Returns the rows deleted.
--   SELECT sema_forget_aircraft('4bb1a2');   SELECT sema_forget_aircraft('TCABC');
CREATE OR REPLACE FUNCTION sema_forget_aircraft(id text)
RETURNS bigint LANGUAGE plpgsql AS $$
DECLARE
    a       text := lower(trim(id));
    c       text := upper(trim(id));
    deleted bigint := 0;
    n       bigint;
BEGIN
    DELETE FROM positions WHERE icao24 = a OR upper(trim(callsign)) = c;
    GET DIAGNOSTICS n = ROW_COUNT;
    deleted := deleted + n;
    DELETE FROM positions_hourly WHERE icao24 = a OR upper(trim(callsign)) = c;
    GET DIAGNOSTICS n = ROW_COUNT;
    deleted := deleted + n;
    DELETE FROM flights WHERE icao24 = a OR upper(trim(callsign)) = c;
    GET DIAGNOSTICS n = ROW_COUNT;
    deleted := deleted + n;
    DELETE FROM anomaly_events WHERE icao24 = a OR upper(trim(callsign)) = c;
    GET DIAGNOSTICS n = ROW_COUNT;
    deleted := deleted + n;
    RETURN deleted;
END;
$$;

INSERT INTO schema_version (version) VALUES (4) ON CONFLICT DO NOTHING;
