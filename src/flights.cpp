#include "sema/flights.hpp"

#include <algorithm>
#include <cmath>
#include <fstream>
#include <limits>
#include <stdexcept>
#include <utility>

namespace sema {

AirportIndex::AirportIndex(std::vector<Airport> airports) : airports_(std::move(airports)) {}

AirportIndex AirportIndex::from_json(const nlohmann::json& doc) {
    const auto& list = doc.contains("airports") ? doc.at("airports") : doc;
    if (!list.is_array()) throw std::runtime_error("airports: expected an array");
    std::vector<Airport> out;
    out.reserve(list.size());
    for (const auto& a : list) {
        Airport ap;
        ap.icao = a.at("icao").get<std::string>();
        ap.iata = a.value("iata", "");
        ap.name = a.value("name", "");
        ap.city = a.value("city", "");
        ap.pos = {a.at("lat").get<double>(), a.at("lon").get<double>()};
        ap.elevation_m = a.value("elevation_m", 0.0);
        out.push_back(std::move(ap));
    }
    return AirportIndex(std::move(out));
}

AirportIndex AirportIndex::load(const std::filesystem::path& path) {
    std::ifstream in(path);
    if (!in) throw std::runtime_error("cannot open " + path.string());
    return from_json(nlohmann::json::parse(in));
}

const Airport* AirportIndex::nearest(LatLon p, double max_m) const {
    const Airport* best = nullptr;
    double best_d = max_m;
    for (const auto& a : airports_) {
        // Cheap reject before the haversine: 0.5 deg is ~55 km.
        if (std::abs(a.pos.lat - p.lat) > 0.5 || std::abs(a.pos.lon - p.lon) > 0.7) continue;
        const double d = haversine_m(a.pos, p);
        if (d <= best_d) {
            best_d = d;
            best = &a;
        }
    }
    return best;
}

const Airport* AirportIndex::find(const std::string& icao) const {
    for (const auto& a : airports_) {
        if (a.icao == icao) return &a;
    }
    return nullptr;
}

namespace {
constexpr std::uint32_t kCols = 1440;  // 360 / 0.25
constexpr std::uint32_t kRows = 720;   // 180 / 0.25
}  // namespace

std::uint32_t corridor_cell(LatLon p) {
    const auto row = static_cast<std::uint32_t>(
        std::clamp(std::floor((p.lat + 90.0) / kCorridorCellDeg), 0.0, static_cast<double>(kRows - 1)));
    const auto col = static_cast<std::uint32_t>(
        std::clamp(std::floor((p.lon + 180.0) / kCorridorCellDeg), 0.0, static_cast<double>(kCols - 1)));
    return row * kCols + col;
}

LatLon corridor_cell_origin(std::uint32_t cell) {
    const std::uint32_t row = cell / kCols;  // integer division: row index
    const std::uint32_t col = cell % kCols;
    return {static_cast<double>(row) * kCorridorCellDeg - 90.0, static_cast<double>(col) * kCorridorCellDeg - 180.0};
}

nlohmann::json FlightRecord::to_json() const {
    auto opt = [](const std::optional<double>& v) -> nlohmann::json {
        return v ? nlohmann::json(std::round(*v)) : nlohmann::json(nullptr);
    };
    return {{"track_id", track_id},
            {"icao24", icao24},
            {"callsign", callsign},
            {"origin_country", origin_country},
            {"first_seen", first_seen},
            {"last_seen", last_seen},
            {"first_pos", {first_pos.lat, first_pos.lon}},
            {"last_pos", {last_pos.lat, last_pos.lon}},
            {"min_altitude_m", opt(min_altitude_m)},
            {"max_altitude_m", opt(max_altitude_m)},
            {"fixes", fixes},
            {"airborne", airborne},
            {"departure", departure},
            {"arrival", arrival},
            {"end_reason", end_reason},
            {"cells", cells}};
}

FlightRecord FlightRecord::from_json(const nlohmann::json& j) {
    FlightRecord r;
    r.track_id = j.value("track_id", std::uint64_t{0});
    r.icao24 = j.at("icao24").get<std::string>();
    r.callsign = j.value("callsign", "");
    r.origin_country = j.value("origin_country", "");
    r.first_seen = j.at("first_seen").get<double>();
    r.last_seen = j.at("last_seen").get<double>();
    r.first_pos = {j.at("first_pos").at(0).get<double>(), j.at("first_pos").at(1).get<double>()};
    r.last_pos = {j.at("last_pos").at(0).get<double>(), j.at("last_pos").at(1).get<double>()};
    if (j.contains("min_altitude_m") && j["min_altitude_m"].is_number()) r.min_altitude_m = j["min_altitude_m"].get<double>();
    if (j.contains("max_altitude_m") && j["max_altitude_m"].is_number()) r.max_altitude_m = j["max_altitude_m"].get<double>();
    r.fixes = j.value("fixes", 0);
    r.airborne = j.value("airborne", false);
    r.departure = j.value("departure", "");
    r.arrival = j.value("arrival", "");
    r.end_reason = j.value("end_reason", "");
    if (j.contains("cells")) r.cells = j["cells"].get<std::vector<std::uint32_t>>();
    return r;
}

FlightLog::FlightLog(const AirportIndex* airports, FlightLogConfig config) : airports_(airports), cfg_(config) {}

namespace {

std::optional<double> altitude_of(const StateVector& s) {
    if (s.baro_altitude) return s.baro_altitude;
    return s.geo_altitude;
}

}  // namespace

void FlightLog::add_fix(Open& f, const StateVector& s, double t) {
    const LatLon p{s.latitude.value_or(0.0), s.longitude.value_or(0.0)};  // caller checked has_position()
    const auto alt = altitude_of(s);
    auto& r = f.rec;
    if (r.fixes == 0) {
        r.first_seen = t;
        r.first_pos = p;
        f.first_alt = alt;
        f.first_vrate = s.vertical_rate;
        f.first_ground = s.on_ground;
        r.cells.push_back(corridor_cell(p));
    } else {
        // Sample the segment from the previous fix so fast aircraft do not
        // skip cells between two ~65 s apart reports.
        const double d = haversine_m(r.last_pos, p);
        const int steps = static_cast<int>(std::min(200.0, std::ceil(d / cfg_.cell_step_m)));
        for (int i = 1; i <= steps; ++i) {
            const double k = static_cast<double>(i) / steps;
            r.cells.push_back(corridor_cell({r.last_pos.lat + (p.lat - r.last_pos.lat) * k,
                                             r.last_pos.lon + (p.lon - r.last_pos.lon) * k}));
        }
    }
    r.last_seen = t;
    r.last_pos = p;
    f.last_alt = alt;
    f.last_vrate = s.vertical_rate;
    f.last_ground = s.on_ground;
    if (!s.callsign.empty()) r.callsign = s.callsign;
    if (r.origin_country.empty()) r.origin_country = s.origin_country;
    if (!s.on_ground) r.airborne = true;
    if (alt && !s.on_ground) {
        r.min_altitude_m = r.min_altitude_m ? std::min(*r.min_altitude_m, *alt) : *alt;
        r.max_altitude_m = r.max_altitude_m ? std::max(*r.max_altitude_m, *alt) : *alt;
    }
    ++r.fixes;
    // Keep the cell list small while the flight is open.
    if (r.cells.size() > 512) {
        std::sort(r.cells.begin(), r.cells.end());
        r.cells.erase(std::unique(r.cells.begin(), r.cells.end()), r.cells.end());
    }
}

FlightRecord FlightLog::finish(Open&& f, const std::string& reason) const {
    FlightRecord r = std::move(f.rec);
    r.end_reason = reason;
    std::sort(r.cells.begin(), r.cells.end());
    r.cells.erase(std::unique(r.cells.begin(), r.cells.end()), r.cells.end());
    if (airports_ != nullptr && r.airborne && r.fixes >= 2) {
        auto low = [&](const Airport& a, bool ground, const std::optional<double>& alt) {
            return ground || (alt && *alt <= a.elevation_m + cfg_.airport_height_m);
        };
        if (const auto* a = airports_->nearest(r.first_pos, cfg_.airport_radius_m);
            a != nullptr && low(*a, f.first_ground, f.first_alt) &&
            !(f.first_vrate && *f.first_vrate < -cfg_.vertical_rate_threshold_ms)) {
            r.departure = a->icao;
        }
        if (const auto* a = airports_->nearest(r.last_pos, cfg_.airport_radius_m);
            reason != "left_area" && a != nullptr && low(*a, f.last_ground, f.last_alt) &&
            !(f.last_vrate && *f.last_vrate > cfg_.vertical_rate_threshold_ms)) {
            r.arrival = a->icao;
        }
    }
    return r;
}

std::vector<FlightRecord> FlightLog::update(const StatesSnapshot& snapshot, const SnapshotReport& report,
                                            const Tracker& tracker) {
    std::vector<FlightRecord> done;
    for (const auto& e : report.ended) {
        auto it = open_.find(e.icao24);
        if (it == open_.end() || it->second.rec.track_id != e.id) continue;
        done.push_back(finish(std::move(it->second), to_string(e.reason)));
        open_.erase(it);
    }
    const auto t = static_cast<double>(snapshot.time);
    for (const auto& s : snapshot.states) {
        if (!s.has_position()) continue;
        const auto view = tracker.track_at(s.icao24, t);
        if (!view) continue;
        auto it = open_.find(s.icao24);
        if (it != open_.end() && it->second.rec.track_id != view->id) {
            // The tracker replaced the track without reporting it (should not
            // happen; be safe and close the old flight).
            done.push_back(finish(std::move(it->second), "restarted"));
            open_.erase(it);
            it = open_.end();
        }
        if (it == open_.end()) {
            Open f;
            f.rec.track_id = view->id;
            f.rec.icao24 = s.icao24;
            it = open_.emplace(s.icao24, std::move(f)).first;
        }
        // Only fixes the tracker accepted (its newest fix moved forward).
        if (it->second.rec.fixes == 0 || view->last_fix_time > it->second.rec.last_seen) {
            add_fix(it->second, s, view->last_fix_time);
        }
    }
    return done;
}

std::vector<FlightRecord> FlightLog::flush() {
    std::vector<FlightRecord> done;
    done.reserve(open_.size());
    for (auto& [_, f] : open_) done.push_back(finish(std::move(f), "open"));
    open_.clear();
    std::sort(done.begin(), done.end(), [](const auto& a, const auto& b) { return a.first_seen < b.first_seen; });
    return done;
}

std::vector<FlightRecord> FlightLog::open_flights() const {
    std::vector<FlightRecord> out;
    out.reserve(open_.size());
    for (const auto& [_, f] : open_) {
        Open copy = f;
        out.push_back(finish(std::move(copy), "open"));
    }
    return out;
}

}  // namespace sema
