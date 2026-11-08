-- Semâ schema, version 3: flight log for statistics (phase 5).
-- One row per flight seen over Turkey (one tracker track). Like every other
-- table, it only ever holds civil aircraft: the collector drops military and
-- government aircraft before tracking. Flights are small and kept for good,
-- so statistics outlive the raw `positions` window.

CREATE TABLE IF NOT EXISTS flights (
    id              bigserial   PRIMARY KEY,
    track_id        bigint      NOT NULL,
    icao24          char(6)     NOT NULL,
    callsign        text,
    origin_country  text,
    first_seen      timestamptz NOT NULL,
    last_seen       timestamptz NOT NULL,
    first_pos       geography(Point, 4326) NOT NULL,
    last_pos        geography(Point, 4326) NOT NULL,
    min_altitude_m  real,
    max_altitude_m  real,
    fixes           integer     NOT NULL,
    airborne        boolean     NOT NULL,
    departure       char(4),
    arrival         char(4),
    end_reason      text        NOT NULL,
    -- 0.25 degree corridor cells crossed (see sema::corridor_cell).
    cells           integer[]   NOT NULL DEFAULT '{}',
    UNIQUE (icao24, first_seen)
);
CREATE INDEX IF NOT EXISTS flights_first_seen_idx ON flights (first_seen);

-- Daily traffic in Turkish local time, for quick checks in psql.
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

INSERT INTO schema_version (version) VALUES (3) ON CONFLICT DO NOTHING;
