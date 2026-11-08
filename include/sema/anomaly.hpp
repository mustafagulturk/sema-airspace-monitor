#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <filesystem>
#include <fstream>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include <nlohmann/json.hpp>

#include "sema/geo.hpp"
#include "sema/live.hpp"
#include "sema/metrics.hpp"
#include "sema/state_vector.hpp"
#include "sema/tracker.hpp"

namespace sema {

// Phase 4: anomaly detection on civil traffic. The detector sees exactly what
// the tracker sees (civil aircraft inside the area; military and government
// aircraft are dropped before either runs), so it can never raise an event
// about them. There are no military-area or base-related rules by design.

enum class EventKind {
    Emergency,       // squawk 7500 / 7600 / 7700
    RapidDescent,    // sustained altitude loss far above a normal descent
    Holding,         // airborne at speed but staying inside a small circle
    RouteDeviation,  // course reversal at cruise (turn-back / diversion)
    SignalLoss,      // long gap in an area with good receiver coverage
};
inline constexpr std::size_t kEventKinds = 5;

const char* to_string(EventKind k);

// Thresholds. Defaults are chosen against what normal traffic does (see the
// "Anomali tespiti" section of the README); `sema_replay --events` prints the
// event counts and rates needed to tune them on a recording.
struct AnomalyConfig {
    // Emergency squawk: the same code on this many consecutive fresh reports
    // (one corrupt reply must not raise a hijack alarm), and cleared after as
    // many reports without it.
    int squawk_confirm_reports = 2;

    // Rapid descent: altitude lost over at most `descent_window_s`, measured
    // between accepted fixes that each go down (no single corrupt altitude
    // can make one), above `descent_min_start_alt_m`.
    double descent_window_s = 240.0;
    double descent_min_drop_m = 1500.0;    // ~5,000 ft
    double descent_min_rate_ms = 22.0;     // ~4,300 ft/min average
    int descent_min_fixes = 3;
    double descent_min_start_alt_m = 4000.0;

    // Holding / circling: over at least `holding_min_duration_s`, every fix
    // stays within `holding_max_radius_m` of the fixes' centroid while the
    // reported ground speed averages at least `holding_min_speed_ms`. An
    // aircraft at that speed covers 30+ km in 8 minutes; staying inside a
    // 15 km circle means it is flying loops or a racetrack. Robust to the
    // ~60-75 s sampling, which aliases heading-based turn counting.
    double holding_min_duration_s = 480.0;
    double holding_max_radius_m = 15000.0;
    double holding_min_speed_ms = 60.0;
    int holding_min_fixes = 6;

    // Route deviation: a straight established course (at least
    // `deviation_course_fixes` fixes whose legs stay within
    // `deviation_straight_tol_deg` of it) followed by a new course at least
    // `deviation_min_turn_deg` away held for two legs, at or above
    // `deviation_min_alt_m`. Airway turns are mostly below 90 degrees; turning
    // back at cruise is what a diversion or return looks like. Approaches and
    // departures (where reversals are routine) are below the altitude floor.
    int deviation_course_fixes = 4;
    double deviation_straight_tol_deg = 12.0;
    double deviation_new_course_tol_deg = 20.0;
    double deviation_min_turn_deg = 120.0;
    double deviation_min_alt_m = 6000.0;
    double deviation_min_leg_m = 3000.0;  // legs shorter than this have a noisy bearing

    // Signal loss: a confirmed airborne track gets no fix for this long while
    // its predicted position is still inside the area, and the coverage cell
    // of its last fix had at least `signal_loss_min_coverage` reports over
    // the coverage window (so eastern coverage holes do not raise events).
    double signal_loss_after_s = 300.0;
    double signal_loss_min_alt_m = 3000.0;
    std::uint64_t signal_loss_min_coverage = 200;
    double coverage_cell_deg = 0.5;
    int coverage_window_h = 24;

    // Track kept per event, for the map: fixes from this long before the
    // start up to the end, at most `event_track_points` (oldest dropped).
    double event_track_before_s = 900.0;
    std::size_t event_track_points = 120;
    double history_s = 1800.0;   // per-aircraft fix history
    std::size_t recent_events = 50;  // events kept for the live map
};

// Sets one numeric threshold by its field name (e.g. "descent_min_rate_ms"),
// for tuning from the command line. Returns false for an unknown name or an
// invalid (negative or non-finite) value. `anomaly_param_names()` lists them.
bool set_anomaly_param(AnomalyConfig& cfg, std::string_view name, double value);
std::vector<std::string> anomaly_param_names();

struct EventPoint {
    double time;
    LatLon pos;
    std::optional<double> altitude_m;
};

struct AnomalyEvent {
    std::uint64_t id = 0;
    EventKind kind = EventKind::Emergency;
    std::string icao24;
    std::string callsign;
    std::string origin_country;
    std::uint64_t track_id = 0;
    double start_time = 0.0;  // data time the anomaly began (first fix that shows it)
    double detect_time = 0.0;  // snapshot time it was raised
    double end_time = 0.0;    // last time it was seen to hold (== start while just opened)
    bool active = true;
    LatLon pos{};             // where it was raised
    std::optional<double> altitude_m;
    std::string summary;      // one-line Turkish description for the map
    nlohmann::json details = nlohmann::json::object();  // kind-specific numbers
    std::vector<EventPoint> track;

    // Stable across restarts: kind, address and start time.
    [[nodiscard]] std::string key() const;
    // {"id","key","kind","icao24",...,"track":[[lon,lat,alt|null,t],...]}
    [[nodiscard]] nlohmann::json to_json(bool with_track = true) const;
};

// What changed in one snapshot. An event appears in `opened` once, may then
// appear in `updated` (still active, new end time/details) and finally once
// in `closed`. Copies, so they can be stored or sent elsewhere.
struct AnomalyReport {
    std::vector<AnomalyEvent> opened;
    std::vector<AnomalyEvent> updated;
    std::vector<AnomalyEvent> closed;
    [[nodiscard]] bool empty() const { return opened.empty() && updated.empty() && closed.empty(); }
};

struct AnomalyMetrics {
    std::uint64_t snapshots = 0;
    std::array<std::uint64_t, kEventKinds> events_by_kind{};
    // Airborne confirmed tracks integrated over time, the denominator of the
    // event rate (events per 1,000 aircraft-hours) used to tune thresholds.
    double airborne_aircraft_hours = 0.0;
    Distribution duration_s;
    Distribution processing_us_per_snapshot;
    [[nodiscard]] std::uint64_t events() const;
    [[nodiscard]] nlohmann::json to_json() const;
};

// Runs every rule on each snapshot. Not thread safe.
class AnomalyDetector {
public:
    // `area` (optional) enables the signal-loss rule, which needs to know
    // whether an aircraft is still inside and how well its cell is covered.
    explicit AnomalyDetector(AnomalyConfig config = {}, std::shared_ptr<const MultiPolygon> area = nullptr);

    // Call right after `tracker.process(snapshot)` with the same snapshot and
    // the report it returned.
    AnomalyReport process(const StatesSnapshot& snapshot, const Tracker& tracker, const SnapshotReport& report);

    // Most recent events (active first, then newest), at most `recent_events`.
    [[nodiscard]] std::vector<AnomalyEvent> recent() const;
    [[nodiscard]] std::vector<AnomalyEvent> active() const;
    // {"type":"events","protocol","time","counts":{kind: n},"active":N,"events":[...]}
    [[nodiscard]] nlohmann::json events_json() const;
    [[nodiscard]] const AnomalyMetrics& metrics() const { return metrics_; }
    [[nodiscard]] const AnomalyConfig& config() const { return cfg_; }

private:
    struct Fix {
        double time;
        LatLon pos;
        std::optional<double> altitude;
        std::optional<double> speed;
        bool on_ground;
    };
    struct Aircraft {
        std::uint64_t track_id = 0;
        std::string callsign;
        std::string origin_country;
        std::deque<Fix> fixes;
        double last_fix_time = -1.0;
        // Squawk state.
        double last_contact = 0.0;  // newest OpenSky last_contact handled
        std::string squawk_candidate;
        int squawk_count = 0;
        double squawk_first_time = 0.0;
        // Index into active_ per kind, if an event of that kind is open.
        std::array<std::optional<std::uint64_t>, kEventKinds> open{};
        // End time of the last event of each kind (for the cool-down).
        std::array<double, kEventKinds> closed_at = {-1e18, -1e18, -1e18, -1e18, -1e18};
        double last_seen = 0.0;  // snapshot time of the last report
    };

    void add_fix(Aircraft& a, const Fix& f);
    void check_emergency(const std::string& icao, Aircraft& a, const StateVector& s, double now, AnomalyReport& out);
    void check_descent(const std::string& icao, Aircraft& a, double now, AnomalyReport& out);
    void check_holding(const std::string& icao, Aircraft& a, double now, AnomalyReport& out);
    void check_deviation(const std::string& icao, Aircraft& a, double now, AnomalyReport& out);
    void check_signal_loss(const Tracker& tracker, double now, AnomalyReport& out);

    AnomalyEvent& open_event(EventKind kind, const std::string& icao, Aircraft& a, double start, double now,
                             LatLon pos, std::optional<double> alt, AnomalyReport& out);
    void touch_event(Aircraft& a, EventKind kind, double end, const nlohmann::json& details, AnomalyReport& out);
    void close_event(Aircraft& a, EventKind kind, double end, AnomalyReport& out);
    void close_all(Aircraft& a, double end, AnomalyReport& out);
    void append_track(AnomalyEvent& e, const Fix& f) const;

    AnomalyConfig cfg_;
    std::shared_ptr<const MultiPolygon> area_;
    std::optional<CoverageGrid> coverage_;
    std::unordered_map<std::string, Aircraft> aircraft_;
    std::unordered_map<std::uint64_t, AnomalyEvent> active_;
    std::deque<AnomalyEvent> closed_;  // newest at the front
    std::uint64_t next_id_ = 1;
    double last_time_ = 0.0;
    AnomalyMetrics metrics_;
};

// Appends events to a JSON Lines file: one {"change":"opened"|"closed", ...}
// line per event change (updates are not written; the closed line carries
// the final state). Throws std::runtime_error if the file cannot be opened.
class EventLog {
public:
    explicit EventLog(const std::filesystem::path& path);
    void write(const AnomalyReport& report);

private:
    std::ofstream out_;
};

}  // namespace sema
