#include "sema/live.hpp"

#include <algorithm>
#include <cmath>
#include <limits>
#include <stdexcept>
#include <unordered_set>
#include <utility>

namespace sema {
namespace {

using nlohmann::json;

double round_to(double x, int decimals) {
    const double f = std::pow(10.0, decimals);
    return std::round(x * f) / f;
}

std::int64_t hour_of(double t) { return static_cast<std::int64_t>(std::floor(t / 3600.0)); }

}  // namespace

// --- CoverageGrid ------------------------------------------------------------

CoverageGrid::CoverageGrid(const MultiPolygon& area, double cell_deg, int window_hours)
    : cell_deg_(cell_deg), window_h_(window_hours) {
    if (!(cell_deg > 0.0) || window_hours < 1) throw std::invalid_argument("bad coverage grid parameters");
    const auto& b = area.bounds();
    lat0_ = std::floor(b.lat_min / cell_deg) * cell_deg;
    lon0_ = std::floor(b.lon_min / cell_deg) * cell_deg;
    rows_ = static_cast<int>(std::ceil((b.lat_max - lat0_) / cell_deg));
    cols_ = static_cast<int>(std::ceil((b.lon_max - lon0_) / cell_deg));
    index_.assign(static_cast<std::size_t>(rows_) * static_cast<std::size_t>(cols_), -1);
    slot_hour_.assign(static_cast<std::size_t>(window_h_), std::numeric_limits<std::int64_t>::min());

    // A cell belongs to the area if any of a 5x5 grid of sample points inside
    // it does (0.1 deg spacing for 0.5 deg cells: thin coastal slivers count).
    constexpr int kSamples = 5;
    for (int r = 0; r < rows_; ++r) {
        for (int c = 0; c < cols_; ++c) {
            bool inside = false;
            for (int i = 0; i < kSamples && !inside; ++i) {
                for (int j = 0; j < kSamples && !inside; ++j) {
                    const LatLon p{lat0_ + (r + (i + 0.5) / kSamples) * cell_deg,
                                   lon0_ + (c + (j + 0.5) / kSamples) * cell_deg};
                    inside = area.contains(p);
                }
            }
            if (!inside) continue;
            index_[static_cast<std::size_t>(r) * static_cast<std::size_t>(cols_) + static_cast<std::size_t>(c)] = static_cast<std::int32_t>(cells_.size());
            cells_.push_back({r, c, std::vector<std::uint32_t>(static_cast<std::size_t>(window_h_), 0)});
        }
    }
}

std::optional<std::size_t> CoverageGrid::cell_index(LatLon p) const {
    if (!std::isfinite(p.lat) || !std::isfinite(p.lon)) return std::nullopt;
    const int r = static_cast<int>(std::floor((p.lat - lat0_) / cell_deg_));
    const int c = static_cast<int>(std::floor((p.lon - lon0_) / cell_deg_));
    if (r < 0 || c < 0 || r >= rows_ || c >= cols_) return std::nullopt;
    const auto i = index_[static_cast<std::size_t>(r) * static_cast<std::size_t>(cols_) + static_cast<std::size_t>(c)];
    if (i < 0) return std::nullopt;
    return static_cast<std::size_t>(i);
}

void CoverageGrid::add(LatLon p, double time) {
    const auto cell = cell_index(p);
    if (!cell) return;
    const std::int64_t hour = hour_of(time);
    const auto w = static_cast<std::int64_t>(window_h_);
    const auto slot = static_cast<std::size_t>(((hour % w) + w) % w);
    if (slot_hour_[slot] > hour) return;  // older than the window
    if (slot_hour_[slot] < hour) {
        for (auto& c : cells_) c.counts[slot] = 0;
        slot_hour_[slot] = hour;
    }
    ++cells_[*cell].counts[slot];
}

std::uint64_t CoverageGrid::window_sum(const Cell& c, std::int64_t now_hour) const {
    std::uint64_t sum = 0;
    for (std::size_t s = 0; s < slot_hour_.size(); ++s) {
        const auto h = slot_hour_[s];
        if (h <= now_hour && h > now_hour - window_h_) sum += c.counts[s];
    }
    return sum;
}

std::uint64_t CoverageGrid::count_at(LatLon p, double now) const {
    const auto cell = cell_index(p);
    return cell ? window_sum(cells_[*cell], hour_of(now)) : 0;
}

json CoverageGrid::to_json(double now) const {
    const auto now_hour = hour_of(now);
    json cells = json::array();
    std::uint64_t max = 0;
    for (const auto& c : cells_) {
        const auto n = window_sum(c, now_hour);
        max = std::max(max, n);
        cells.push_back(json::array({round_to(lat0_ + c.row * cell_deg_, 4), round_to(lon0_ + c.col * cell_deg_, 4), n}));
    }
    return {{"type", "coverage"}, {"protocol", kLiveProtocolVersion}, {"cell_deg", cell_deg_},
            {"window_h", window_h_},  {"time", now},                    {"max", max},
            {"cells", std::move(cells)}};
}

// --- LiveState ----------------------------------------------------------------

LiveState::LiveState(const MultiPolygon& area, LiveConfig config)
    : cfg_(config), coverage_(area, config.coverage_cell_deg, config.coverage_window_h) {}

void LiveState::update(const StatesSnapshot& snapshot, const Tracker& tracker) {
    data_time_ = static_cast<double>(snapshot.time);
    views_ = tracker.tracks_at(data_time_);

    std::unordered_set<std::string> live;
    live.reserve(views_.size());
    for (const auto& v : views_) {
        live.insert(v.icao24);
        auto& e = extra_[v.icao24];
        if (e.track_id != v.id) {  // new or restarted track: start a fresh trail
            e.track_id = v.id;
            e.trail.clear();
        }
        if (v.updates > 0 && (e.trail.empty() || v.last_fix_time > e.trail.back().time)) {
            // The filtered position at the time of the new fix, not the raw report.
            if (const auto at_fix = tracker.track_at(v.icao24, v.last_fix_time)) {
                e.trail.push_back({v.last_fix_time, at_fix->pos});
                coverage_.add(at_fix->pos, v.last_fix_time);
            }
        }
        while (e.trail.size() > cfg_.trail_points ||
               (!e.trail.empty() && e.trail.front().time < data_time_ - cfg_.trail_max_age_s)) {
            e.trail.pop_front();
        }
    }
    for (const auto& s : snapshot.states) {
        if (!s.origin_country.empty() && live.count(s.icao24) != 0) extra_[s.icao24].origin_country = s.origin_country;
    }
    std::erase_if(extra_, [&live](const auto& kv) { return live.count(kv.first) == 0; });
}

json LiveState::snapshot_json(const LiveClock& clock) const {
    json aircraft = json::array();
    std::size_t confirmed = 0;
    std::size_t coasting = 0;
    for (const auto& v : views_) {
        if (v.state == TrackState::Confirmed) ++confirmed;
        if (v.state == TrackState::Coasting) ++coasting;
        json trail = json::array();
        std::string country;
        if (auto it = extra_.find(v.icao24); it != extra_.end()) {
            country = it->second.origin_country;
            for (const auto& p : it->second.trail) {
                trail.push_back(json::array({round_to(p.pos.lon, 5), round_to(p.pos.lat, 5)}));
            }
        }
        json a{{"id", v.id},
               {"icao24", v.icao24},
               {"callsign", v.callsign},
               {"country", country},
               {"state", to_string(v.state)},
               {"t", v.time},
               {"fix_t", v.last_fix_time},
               {"lat", round_to(v.pos.lat, 5)},
               {"lon", round_to(v.pos.lon, 5)},
               {"speed", round_to(v.speed_ms, 1)},
               {"track", round_to(v.track_deg, 1)},
               {"sigma", round_to(v.pos_sigma_m, 0)},
               {"ground", v.on_ground},
               {"alt", nullptr},
               {"vrate", nullptr},
               {"turn", nullptr},
               {"trail", std::move(trail)}};
        if (v.altitude_m) a["alt"] = round_to(*v.altitude_m, 0);
        if (v.vertical_rate_ms) a["vrate"] = round_to(*v.vertical_rate_ms, 1);
        if (v.turn_rate_dps) a["turn"] = round_to(*v.turn_rate_dps, 2);
        aircraft.push_back(std::move(a));
    }
    return {{"type", "snapshot"},
            {"protocol", kLiveProtocolVersion},
            {"data_time", data_time_},
            {"clock", {{"time", clock.time}, {"rate", clock.rate}}},
            {"counts", {{"tracked", views_.size()}, {"confirmed", confirmed}, {"coasting", coasting}}},
            {"aircraft", std::move(aircraft)}};
}

json live_hello_json(const std::string& source, const std::string& label) {
    return {{"type", "hello"},
            {"protocol", kLiveProtocolVersion},
            {"source", source},
            {"label", label},
            {"attribution",
             "OpenSky Network (opensky-network.org); M. Schäfer, M. Strohmeier, V. Lenders, I. Martinovic, "
             "M. Wilhelm, \"Bringing Up OpenSky: A Large-scale ADS-B Sensor Network for Research\", IPSN 2014"}};
}

}  // namespace sema
