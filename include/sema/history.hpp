#pragma once

#include <cstddef>
#include <cstdint>
#include <deque>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

#include <nlohmann/json.hpp>

#include "sema/api.hpp"
#include "sema/flights.hpp"
#include "sema/state_vector.hpp"
#include "sema/traffic_stats.hpp"

namespace sema {

// One recorded position report, as replayed in the browser.
struct ReplayFix {
    double t = 0.0;  // Unix seconds (OpenSky time_position, else last_contact)
    double lat = 0.0;
    double lon = 0.0;
    std::optional<double> alt_m;
    std::optional<double> speed_ms;
    std::optional<double> track_deg;
    bool on_ground = false;
};

struct ReplayAircraft {
    std::string icao24;
    std::string callsign;
    std::string origin_country;
    std::vector<ReplayFix> fixes;  // time order, no repeated timestamps
};

struct TimeRange {
    double from = 0.0;
    double to = 0.0;
};

// Past data for the "Geçmiş" (replay) and "İstatistik" views. Like the live
// map, it only ever holds filtered (civil, in-area) data. Implementations must
// be safe to call from the API worker thread while the collector loop records.
class HistorySource {
public:
    virtual ~HistorySource() = default;

    // Window for which position reports can be replayed, or nullopt if none.
    virtual std::optional<TimeRange> available() = 0;
    // Position reports with a timestamp in [from, to], grouped by aircraft.
    virtual std::vector<ReplayAircraft> replay(double from, double to) = 0;
    // Completed flights that started in [from, to).
    virtual std::vector<FlightRecord> flights(double from, double to) = 0;
    // The current unbroken stretch of data collection: from its start to the
    // newest data, where "unbroken" means no gap over kStreakGapS. Nullopt if
    // there is no data or the newest data is older than that gap.
    static constexpr double kStreakGapS = 900.0;
    virtual std::optional<TimeRange> collection_streak() = 0;

    // Called by the collection loop after each snapshot. The PostgreSQL source
    // ignores this (PgStore already wrote the rows).
    virtual void record(const StatesSnapshot& snapshot, const std::vector<FlightRecord>& ended) = 0;

    // Flights still in progress, so statistics include "today" up to now.
    void set_open_flights(std::vector<FlightRecord> open);
    [[nodiscard]] std::vector<FlightRecord> open_flights(double from, double to) const;

private:
    mutable std::mutex open_mu_;
    std::vector<FlightRecord> open_;
};

// Keeps recent history in memory: for --dry-run, sema_replay and tests.
class MemoryHistory : public HistorySource {
public:
    struct Limits {
        double keep_fixes_s = 24 * 3600.0;   // replay window
        std::size_t max_fixes = 4'000'000;    // ~100 MB worst case
        double keep_flights_s = 31 * 86400.0;
        std::size_t max_flights = 300'000;
    };
    MemoryHistory() : MemoryHistory(Limits{}) {}
    explicit MemoryHistory(Limits limits);

    std::optional<TimeRange> available() override;
    std::vector<ReplayAircraft> replay(double from, double to) override;
    std::vector<FlightRecord> flights(double from, double to) override;
    std::optional<TimeRange> collection_streak() override;
    void record(const StatesSnapshot& snapshot, const std::vector<FlightRecord>& ended) override;

    [[nodiscard]] std::size_t fix_count() const;
    [[nodiscard]] std::size_t flight_count() const;

private:
    struct Stored {
        std::uint32_t aircraft;  // index into aircraft_
        ReplayFix fix;
    };
    struct Meta {
        std::string icao24;
        std::string callsign;
        std::string origin_country;
        double last_t = -1.0;
    };

    void prune_locked();

    Limits limits_;
    mutable std::mutex mu_;
    std::deque<Stored> fixes_;   // in arrival order (snapshot time order)
    std::vector<Meta> aircraft_;
    std::unordered_map<std::string, std::uint32_t> index_;
    std::deque<FlightRecord> flights_;  // in end order
    std::optional<double> streak_start_;
    double newest_ = 0.0;
};

struct HistoryApiConfig {
    std::string mode = "live";            // "live" or "replay"
    std::string label;                    // shown in the UI (e.g. "sentetik veri")
    double max_replay_s = 4 * 3600.0;
    double max_stats_s = 92 * 86400.0;
    double default_stats_s = 7 * 86400.0;
    int utc_offset_s = 3 * 3600;
    double stats_cache_s = 60.0;
};

// JSON API for the web map:
//   GET /api/status                    mode, uptime, replayable window
//   GET /api/replay?from=T&to=T        position reports to replay in the browser
//   GET /api/stats?from=T&to=T         traffic statistics (default: last 7 days)
// Times are Unix seconds. Unknown /api/ paths give 404; other paths nullopt.
class HistoryApi {
public:
    HistoryApi(std::shared_ptr<HistorySource> source, std::shared_ptr<const AirportIndex> airports,
               HistoryApiConfig config, std::function<double()> now = {});

    std::optional<ApiResponse> handle(const ApiRequest& req);

    [[nodiscard]] nlohmann::json status_json();

private:
    ApiResponse replay(const ApiRequest& req);
    ApiResponse stats(const ApiRequest& req);

    std::shared_ptr<HistorySource> source_;
    std::shared_ptr<const AirportIndex> airports_;
    HistoryApiConfig cfg_;
    std::function<double()> now_;
    double started_at_;
    std::mutex cache_mu_;
    std::map<std::pair<std::int64_t, std::int64_t>, std::pair<double, std::string>> stats_cache_;
};

// {"from", "to", "aircraft": [{"icao24", "callsign", "country",
//   "fixes": [[t, lat, lon, alt|null, speed|null, track|null, ground(0/1)], ...]}]}
nlohmann::json replay_to_json(const std::vector<ReplayAircraft>& aircraft, double from, double to);

}  // namespace sema
