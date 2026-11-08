#pragma once

#include <cstddef>
#include <cstdint>
#include <deque>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

#include <nlohmann/json.hpp>

#include "sema/geo.hpp"
#include "sema/state_vector.hpp"
#include "sema/tracker.hpp"

namespace sema {

// Version of the JSON messages sent to the web map over WebSocket. Bump on
// incompatible changes; the browser refuses a version it does not know.
inline constexpr int kLiveProtocolVersion = 1;

struct LiveConfig {
    std::size_t trail_points = 20;   // most recent fixes kept per aircraft
    double trail_max_age_s = 900.0;  // fixes older than this drop off the tail
    double coverage_cell_deg = 0.5;
    int coverage_window_h = 24;
};

// Time base the browser extrapolates on. `time` is the data clock when the
// message was built (Unix seconds); it advances at `rate` seconds per real
// second (1 for live data, N for an N-times replay, 0 when paused).
struct LiveClock {
    double time = 0.0;
    double rate = 1.0;
};

// Rolling count of accepted position reports per grid cell over the last
// `window_hours` hours, for the cells that overlap the monitored area. A cell
// with no reports in the window is a coverage gap (or has no traffic: the two
// cannot be told apart from the data alone). Hourly buckets in a ring, so
// memory is fixed and old hours fall off without a scan.
class CoverageGrid {
public:
    CoverageGrid(const MultiPolygon& area, double cell_deg, int window_hours);

    // Counts a report at `p` at data time `time` (Unix seconds). Reports
    // outside every area cell, or older than the window, are ignored.
    void add(LatLon p, double time);

    // Reports in the window ending at `now`, for the cell holding `p`.
    [[nodiscard]] std::uint64_t count_at(LatLon p, double now) const;
    [[nodiscard]] std::size_t cell_count() const { return cells_.size(); }
    [[nodiscard]] double cell_deg() const { return cell_deg_; }

    // {"type":"coverage", "cell_deg", "window_h", "time", "max",
    //  "cells": [[lat_min, lon_min, count], ...]} for every area cell.
    [[nodiscard]] nlohmann::json to_json(double now) const;

private:
    struct Cell {
        int row;
        int col;
        std::vector<std::uint32_t> counts;  // one per ring slot
    };

    [[nodiscard]] std::optional<std::size_t> cell_index(LatLon p) const;
    [[nodiscard]] std::uint64_t window_sum(const Cell& c, std::int64_t now_hour) const;

    double cell_deg_;
    int window_h_;
    double lat0_;
    double lon0_;
    int rows_ = 0;
    int cols_ = 0;
    std::vector<Cell> cells_;
    std::vector<std::int32_t> index_;       // rows_*cols_ -> cell index or -1
    std::vector<std::int64_t> slot_hour_;   // hour (Unix hours) held by each ring slot
};

// What the web map shows: tracker estimates plus short trails, origin
// country and coverage. Fed with the same filtered snapshots as the tracker,
// so it only ever sees civil aircraft inside the area. Not thread safe; the
// messages it builds are plain strings that can be handed to another thread.
class LiveState {
public:
    explicit LiveState(const MultiPolygon& area, LiveConfig config = {});

    // Call right after `tracker.process(snapshot)` with the same snapshot.
    void update(const StatesSnapshot& snapshot, const Tracker& tracker);

    // {"type":"snapshot", "protocol", "data_time", "clock":{"time","rate"},
    //  "counts":{...}, "aircraft":[...]} with every live track estimated at
    // the snapshot time. The browser moves each aircraft on from there.
    [[nodiscard]] nlohmann::json snapshot_json(const LiveClock& clock) const;
    [[nodiscard]] nlohmann::json coverage_json() const { return coverage_.to_json(data_time_); }

    [[nodiscard]] std::size_t aircraft_count() const { return views_.size(); }
    [[nodiscard]] double data_time() const { return data_time_; }

private:
    struct TrailPoint {
        double time;
        LatLon pos;
    };
    struct Extra {
        std::uint64_t track_id = 0;
        std::string origin_country;
        std::deque<TrailPoint> trail;
    };

    LiveConfig cfg_;
    CoverageGrid coverage_;
    double data_time_ = 0.0;
    std::vector<TrackView> views_;
    std::unordered_map<std::string, Extra> extra_;
};

// {"type":"hello", "protocol", "source", "label", "attribution"}: the first
// message every client gets. `source` is "live" or "replay".
nlohmann::json live_hello_json(const std::string& source, const std::string& label);

}  // namespace sema
