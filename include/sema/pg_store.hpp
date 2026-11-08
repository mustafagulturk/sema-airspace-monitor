#pragma once

#include <chrono>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "sema/anomaly.hpp"
#include "sema/flights.hpp"
#include "sema/state_vector.hpp"

namespace sema {

struct FetchLogEntry {
    std::chrono::system_clock::time_point started_at;
    std::chrono::milliseconds latency{0};
    std::optional<int> http_status;
    int attempts = 0;
    int credits_cost = 0;
    std::optional<int> credits_remaining;
    std::optional<std::int64_t> data_time;
    std::size_t states_total = 0;
    std::size_t states_kept = 0;
    std::size_t dropped_outside = 0;
    std::size_t dropped_military = 0;
    std::string error;
};

// PostgreSQL/PostGIS persistence. Not thread safe.
class PgStore {
public:
    explicit PgStore(const std::string& conninfo);
    ~PgStore();
    PgStore(const PgStore&) = delete;
    PgStore& operator=(const PgStore&) = delete;

    void migrate();
    void upsert_boundary(const std::string& name, const std::string& geojson_geometry);
    // Inserts positions for one snapshot; rows without a position are ignored.
    // Returns the number of rows written.
    std::size_t insert_positions(std::chrono::system_clock::time_point fetched_at,
                                 const std::vector<StateVector>& states);
    void log_fetch(const FetchLogEntry& e);
    // Inserts or updates anomaly events by key (opened, updated and closed
    // events can all be passed; the latest state wins).
    void upsert_events(const std::vector<AnomalyEvent>& events);
    // Stores finished flights (duplicates by icao24 + first_seen are ignored).
    // Returns the number of rows written.
    std::size_t insert_flights(const std::vector<FlightRecord>& flights);
    // Summarise and delete raw rows older than `keep`. Returns rows deleted.
    std::int64_t rollup_and_prune(std::chrono::hours keep);
    // Replaces ICAO24 with a pseudonym and drops callsigns/tracks in flights,
    // positions_hourly and anomaly_events for whole local days older than
    // `keep` (sql/004_privacy.sql). Returns the number of rows changed.
    std::int64_t anonymize_identities(std::chrono::days keep);

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace sema
