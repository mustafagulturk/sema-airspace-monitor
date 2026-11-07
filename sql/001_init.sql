-- Semâ schema, version 1.
-- Military and government aircraft are filtered out in the C++ collector
-- before anything reaches this database; no table here ever holds them.

CREATE EXTENSION IF NOT EXISTS postgis;

CREATE TABLE IF NOT EXISTS schema_version (
    version    integer PRIMARY KEY,
    applied_at timestamptz NOT NULL DEFAULT now()
);

-- Area used for the second-stage spatial filter (land + territorial sea).
CREATE TABLE IF NOT EXISTS boundaries (
    name       text PRIMARY KEY,
    geom       geometry(MultiPolygon, 4326) NOT NULL,
    updated_at timestamptz NOT NULL DEFAULT now()
);
CREATE INDEX IF NOT EXISTS boundaries_geom_idx ON boundaries USING gist (geom);

-- Raw state vectors (civil aircraft over Turkey only), kept for a short window.
CREATE TABLE IF NOT EXISTS positions (
    fetched_at       timestamptz NOT NULL,
    icao24           char(6)     NOT NULL,
    callsign         text,
    origin_country   text,
    time_position    timestamptz,
    last_contact     timestamptz NOT NULL,
    geom             geography(Point, 4326) NOT NULL,
    baro_altitude_m  real,
    geo_altitude_m   real,
    velocity_ms      real,
    true_track_deg   real,
    vertical_rate_ms real,
    on_ground        boolean     NOT NULL,
    squawk           text,
    position_source  smallint,
    PRIMARY KEY (icao24, fetched_at)
);
CREATE INDEX IF NOT EXISTS positions_fetched_at_idx ON positions (fetched_at);
CREATE INDEX IF NOT EXISTS positions_geom_idx ON positions USING gist (geom);

-- Hourly per-aircraft summary that survives after raw rows are pruned.
CREATE TABLE IF NOT EXISTS positions_hourly (
    hour            timestamptz NOT NULL,
    icao24          char(6)     NOT NULL,
    callsign        text,
    origin_country  text,
    samples         integer     NOT NULL,
    first_seen      timestamptz NOT NULL,
    last_seen       timestamptz NOT NULL,
    min_altitude_m  real,
    max_altitude_m  real,
    track           geometry(LineString, 4326),
    PRIMARY KEY (hour, icao24)
);

-- One row per OpenSky request, used for quota and latency measurements.
CREATE TABLE IF NOT EXISTS fetch_log (
    id                bigserial PRIMARY KEY,
    started_at        timestamptz NOT NULL,
    latency_ms        integer     NOT NULL,
    http_status       integer,
    attempts          integer     NOT NULL,
    credits_cost      integer     NOT NULL,
    credits_remaining integer,
    data_time         timestamptz,
    states_total      integer     NOT NULL DEFAULT 0,
    states_kept       integer     NOT NULL DEFAULT 0,
    dropped_outside   integer     NOT NULL DEFAULT 0,
    dropped_military  integer     NOT NULL DEFAULT 0,
    error             text
);
CREATE INDEX IF NOT EXISTS fetch_log_started_at_idx ON fetch_log (started_at);

-- Summarise whole hours older than `keep` into positions_hourly, then delete them.
-- Returns the number of raw rows deleted.
CREATE OR REPLACE FUNCTION sema_rollup_and_prune(keep interval)
RETURNS bigint LANGUAGE plpgsql AS $$
DECLARE
    cutoff  timestamptz := date_trunc('hour', now() - keep);
    deleted bigint;
BEGIN
    INSERT INTO positions_hourly
        (hour, icao24, callsign, origin_country, samples, first_seen, last_seen,
         min_altitude_m, max_altitude_m, track)
    SELECT date_trunc('hour', fetched_at),
           icao24,
           (array_agg(callsign ORDER BY fetched_at DESC) FILTER (WHERE callsign IS NOT NULL))[1],
           max(origin_country),
           count(*),
           min(fetched_at),
           max(fetched_at),
           min(coalesce(baro_altitude_m, geo_altitude_m)),
           max(coalesce(baro_altitude_m, geo_altitude_m)),
           CASE WHEN count(*) > 1
                THEN ST_MakeLine(geom::geometry ORDER BY fetched_at) END
    FROM positions
    WHERE fetched_at < cutoff
    GROUP BY 1, 2
    ON CONFLICT (hour, icao24) DO NOTHING;

    DELETE FROM positions WHERE fetched_at < cutoff;
    GET DIAGNOSTICS deleted = ROW_COUNT;
    RETURN deleted;
END;
$$;

-- Daily measurements for phase 1: credit usage, aircraft per request and the
-- real interval between distinct OpenSky snapshots.
CREATE OR REPLACE VIEW daily_metrics AS
WITH ok AS (
    SELECT *,
           extract(epoch FROM data_time - lag(data_time) OVER (ORDER BY started_at)) AS data_gap_s,
           extract(epoch FROM started_at - lag(started_at) OVER (ORDER BY started_at)) AS request_gap_s
    FROM fetch_log
    WHERE http_status = 200
)
SELECT date_trunc('day', started_at)::date               AS day,
       count(*)                                          AS requests,
       sum(credits_cost)                                 AS credits_used,
       min(credits_remaining)                            AS min_credits_remaining,
       round(avg(states_total), 1)                       AS avg_states_in_box,
       round(avg(states_kept), 1)                        AS avg_aircraft_kept,
       round(avg(dropped_military), 2)                   AS avg_dropped_military,
       round(avg(request_gap_s)::numeric, 1)             AS avg_request_interval_s,
       round(avg(data_gap_s) FILTER (WHERE data_gap_s > 0)::numeric, 1) AS avg_data_interval_s,
       round(avg(latency_ms), 0)                         AS avg_latency_ms
FROM ok
GROUP BY 1
ORDER BY 1;

INSERT INTO schema_version (version) VALUES (1) ON CONFLICT DO NOTHING;
