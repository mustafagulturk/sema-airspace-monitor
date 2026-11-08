-- Semâ schema, version 2: anomaly events (phase 4).
-- Events are only ever raised for civil aircraft: the detector runs after the
-- military/government filter, like everything else that reaches this database.

CREATE TABLE IF NOT EXISTS anomaly_events (
    key            text PRIMARY KEY,           -- kind:icao24:start (Unix s), stable across restarts
    kind           text        NOT NULL CHECK (kind IN
                       ('emergency', 'rapid_descent', 'holding', 'route_deviation', 'signal_loss')),
    icao24         char(6)     NOT NULL,
    callsign       text,
    origin_country text,
    start_time     timestamptz NOT NULL,
    detect_time    timestamptz NOT NULL,
    end_time       timestamptz NOT NULL,
    active         boolean     NOT NULL,
    geom           geography(Point, 4326) NOT NULL,
    altitude_m     real,
    summary        text        NOT NULL,
    details        jsonb       NOT NULL DEFAULT '{}',
    -- The aircraft's track around the event, for showing it on the map later.
    track          geometry(LineString, 4326),
    updated_at     timestamptz NOT NULL DEFAULT now()
);
CREATE INDEX IF NOT EXISTS anomaly_events_start_idx ON anomaly_events (start_time);
CREATE INDEX IF NOT EXISTS anomaly_events_kind_idx ON anomaly_events (kind, start_time);

-- Events per day and kind, for the README tables and threshold tuning.
CREATE OR REPLACE VIEW daily_events AS
SELECT date_trunc('day', start_time)::date AS day,
       kind,
       count(*)                            AS events,
       round(avg(extract(epoch FROM end_time - start_time))::numeric, 0) AS avg_duration_s
FROM anomaly_events
GROUP BY 1, 2
ORDER BY 1, 2;

INSERT INTO schema_version (version) VALUES (2) ON CONFLICT DO NOTHING;
