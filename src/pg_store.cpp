#include "sema/pg_store.hpp"

#include "pqxx_compat.hpp"

#include "sema/migrations.hpp"

namespace sema {
namespace {

double to_unix_seconds(std::chrono::system_clock::time_point tp) {
    return std::chrono::duration<double>(tp.time_since_epoch()).count();
}

std::optional<std::string> non_empty(const std::string& s) {
    if (s.empty()) return std::nullopt;
    return s;
}

int to_int(std::size_t n) { return static_cast<int>(n); }

}  // namespace

struct PgStore::Impl {
    explicit Impl(const std::string& conninfo) : conn(conninfo) {}
    pqxx::connection conn;
    bool prepared = false;

    void prepare() {
        if (prepared) return;
        conn.prepare("insert_position",
                     "INSERT INTO positions (fetched_at, icao24, callsign, origin_country, time_position, "
                     "last_contact, geom, baro_altitude_m, geo_altitude_m, velocity_ms, true_track_deg, "
                     "vertical_rate_ms, on_ground, squawk, position_source) VALUES ("
                     "to_timestamp($1), $2, $3, $4, to_timestamp($5), to_timestamp($6), "
                     "ST_SetSRID(ST_MakePoint($7, $8), 4326)::geography, $9, $10, $11, $12, $13, $14, $15, $16) "
                     "ON CONFLICT (icao24, fetched_at) DO NOTHING");
        conn.prepare("log_fetch",
                     "INSERT INTO fetch_log (started_at, latency_ms, http_status, attempts, credits_cost, "
                     "credits_remaining, data_time, states_total, states_kept, dropped_outside, dropped_military, "
                     "error) VALUES (to_timestamp($1), $2, $3, $4, $5, $6, to_timestamp($7), $8, $9, $10, $11, $12)");
        conn.prepare("upsert_boundary",
                     "INSERT INTO boundaries (name, geom) "
                     "VALUES ($1, ST_Multi(ST_SetSRID(ST_GeomFromGeoJSON($2), 4326))) "
                     "ON CONFLICT (name) DO UPDATE SET geom = EXCLUDED.geom, updated_at = now()");
        conn.prepare("upsert_event",
                     "INSERT INTO anomaly_events (key, kind, icao24, callsign, origin_country, start_time, "
                     "detect_time, end_time, active, geom, altitude_m, summary, details, track) VALUES ("
                     "$1, $2, $3, $4, $5, to_timestamp($6), to_timestamp($7), to_timestamp($8), $9, "
                     "ST_SetSRID(ST_MakePoint($10, $11), 4326)::geography, $12, $13, $14::jsonb, "
                     "CASE WHEN $15::text IS NULL THEN NULL ELSE ST_SetSRID(ST_GeomFromGeoJSON($15), 4326) END) "
                     "ON CONFLICT (key) DO UPDATE SET callsign = EXCLUDED.callsign, end_time = EXCLUDED.end_time, "
                     "active = EXCLUDED.active, summary = EXCLUDED.summary, details = EXCLUDED.details, "
                     "track = EXCLUDED.track, updated_at = now()");
        conn.prepare("rollup_and_prune", "SELECT sema_rollup_and_prune(make_interval(hours => $1))");
        prepared = true;
    }
};

PgStore::PgStore(const std::string& conninfo) : impl_(std::make_unique<Impl>(conninfo)) {}
PgStore::~PgStore() = default;

void PgStore::migrate() {
    pqxx::work tx(impl_->conn);
    // Serialise concurrent migrations (e.g. two collectors starting together).
    tx.exec("SELECT pg_advisory_xact_lock(7302551)");
    tx.exec(std::string(migrations::k001Init));
    tx.exec(std::string(migrations::k002Events));
    tx.commit();
    impl_->prepare();
}

void PgStore::upsert_boundary(const std::string& name, const std::string& geojson_geometry) {
    impl_->prepare();
    pqxx::work tx(impl_->conn);
    tx.exec_prepared("upsert_boundary", name, geojson_geometry);
    tx.commit();
}

std::size_t PgStore::insert_positions(std::chrono::system_clock::time_point fetched_at,
                                      const std::vector<StateVector>& states) {
    impl_->prepare();
    const double fetched = to_unix_seconds(fetched_at);
    std::size_t written = 0;
    pqxx::work tx(impl_->conn);
    for (const auto& s : states) {
        if (!s.latitude || !s.longitude) continue;
        std::optional<std::int64_t> tpos = s.time_position;
        auto r = tx.exec_prepared("insert_position", fetched, s.icao24, non_empty(s.callsign),
                                  non_empty(s.origin_country), tpos, s.last_contact, *s.longitude, *s.latitude,
                                  s.baro_altitude, s.geo_altitude, s.velocity, s.true_track, s.vertical_rate,
                                  s.on_ground, non_empty(s.squawk), s.position_source);
        written += static_cast<std::size_t>(r.affected_rows());
    }
    tx.commit();
    return written;
}

void PgStore::log_fetch(const FetchLogEntry& e) {
    impl_->prepare();
    pqxx::work tx(impl_->conn);
    tx.exec_prepared("log_fetch", to_unix_seconds(e.started_at), static_cast<int>(e.latency.count()), e.http_status,
                     e.attempts, e.credits_cost, e.credits_remaining, e.data_time, to_int(e.states_total),
                     to_int(e.states_kept), to_int(e.dropped_outside), to_int(e.dropped_military),
                     non_empty(e.error));
    tx.commit();
}

void PgStore::upsert_events(const std::vector<AnomalyEvent>& events) {
    if (events.empty()) return;
    impl_->prepare();
    pqxx::work tx(impl_->conn);
    for (const auto& e : events) {
        std::optional<std::string> track;
        if (e.track.size() >= 2) {
            nlohmann::json coords = nlohmann::json::array();
            for (const auto& p : e.track) coords.push_back({p.pos.lon, p.pos.lat});
            track = nlohmann::json{{"type", "LineString"}, {"coordinates", std::move(coords)}}.dump();
        }
        tx.exec_prepared("upsert_event", e.key(), std::string(to_string(e.kind)), e.icao24, non_empty(e.callsign),
                         non_empty(e.origin_country), e.start_time, e.detect_time, e.end_time, e.active, e.pos.lon,
                         e.pos.lat, e.altitude_m, e.summary, e.details.dump(), track);
    }
    tx.commit();
}

std::int64_t PgStore::rollup_and_prune(std::chrono::hours keep) {
    impl_->prepare();
    pqxx::work tx(impl_->conn);
    auto r = tx.exec_prepared1("rollup_and_prune", static_cast<int>(keep.count()));
    tx.commit();
    return r[0].as<std::int64_t>();
}

}  // namespace sema
