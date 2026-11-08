#include "sema/anomaly.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <numbers>
#include <stdexcept>
#include <unordered_set>

namespace sema {
namespace {

using json = nlohmann::json;

double round_to(double x, int decimals) {
    const double f = std::pow(10.0, decimals);
    return std::round(x * f) / f;
}

// Bearing of the straight line a -> b, degrees clockwise from north.
double bearing_deg(LatLon a, LatLon b) {
    const auto d = to_local(a, b);
    double deg = std::atan2(d.east, d.north) * 180.0 / std::numbers::pi;
    if (deg < 0.0) deg += 360.0;
    return deg;
}

// Smallest absolute difference between two bearings, 0..180.
double bearing_diff(double a, double b) {
    double d = std::fmod(std::abs(a - b), 360.0);
    return d > 180.0 ? 360.0 - d : d;
}

const char* squawk_meaning(const std::string& code) {
    if (code == "7500") return "yasa dışı müdahale";
    if (code == "7600") return "telsiz arızası";
    if (code == "7700") return "genel acil durum";
    return "";
}

bool is_emergency_squawk(const std::string& code) { return code == "7500" || code == "7600" || code == "7700"; }

std::string fmt0(double x) {
    char buf[32];
    (void)std::snprintf(buf, sizeof buf, "%.0f", x);
    return buf;
}

std::string fmt1(double x) {
    char buf[32];
    (void)std::snprintf(buf, sizeof buf, "%.1f", x);
    return buf;
}

double num(const json& d, const char* key) {
    const auto it = d.find(key);
    return it != d.end() && it->is_number() ? it->get<double>() : 0.0;
}

// One-line Turkish description from the event's details, rebuilt whenever
// they change so it always matches the latest state.
void summarize(AnomalyEvent& e) {
    const auto& d = e.details;
    switch (e.kind) {
        case EventKind::Emergency: {
            const std::string code = d.value("squawk", "");
            e.summary = "Acil durum kodu " + code + " (" + squawk_meaning(code) + ")";
            break;
        }
        case EventKind::RapidDescent:
            e.summary = "Ani irtifa kaybı: " + fmt0(num(d, "duration_s") / 60.0) + " dk içinde " +
                        fmt0(num(d, "drop_m")) + " m (ortalama " + fmt1(num(d, "rate_ms")) + " m/sn)";
            break;
        case EventKind::Holding:
            e.summary = "Bekleme / daire çizme: " + fmt0(num(d, "duration_s") / 60.0) + " dk boyunca " +
                        fmt1(num(d, "radius_m") / 1000.0) + " km yarıçap içinde";
            break;
        case EventKind::RouteDeviation:
            e.summary = "Rota sapması: seyir irtifasında " + fmt0(num(d, "turn_deg")) + "° dönüş (" +
                        fmt0(num(d, "old_course_deg")) + "° → " + fmt0(num(d, "new_course_deg")) + "°)";
            break;
        case EventKind::SignalLoss:
            e.summary = "Uzun sinyal kaybı: kapsaması iyi bir bölgede " + fmt0(num(d, "gap_s") / 60.0) +
                        " dk sinyal yok" + (d.value("reacquired", false) ? ", sonra yeniden alındı" : "");
            break;
    }
}

constexpr std::size_t idx(EventKind k) { return static_cast<std::size_t>(k); }

// The same anomaly is not raised again for an aircraft within this long of
// the previous one ending (a descent or a hold that briefly stops meeting the
// rule is one event, not several).
constexpr double kCooldownS = 900.0;
// Consecutive fixes further apart than this are a gap; geometric rules need
// continuous data.
constexpr double kMaxFixGapS = 200.0;

}  // namespace

const char* to_string(EventKind k) {
    switch (k) {
        case EventKind::Emergency: return "emergency";
        case EventKind::RapidDescent: return "rapid_descent";
        case EventKind::Holding: return "holding";
        case EventKind::RouteDeviation: return "route_deviation";
        case EventKind::SignalLoss: return "signal_loss";
    }
    return "unknown";
}

std::string AnomalyEvent::key() const {
    return std::string(to_string(kind)) + ":" + icao24 + ":" + std::to_string(static_cast<std::int64_t>(start_time));
}

json AnomalyEvent::to_json(bool with_track) const {
    json j{{"id", id},
           {"key", key()},
           {"kind", to_string(kind)},
           {"icao24", icao24},
           {"callsign", callsign},
           {"country", origin_country},
           {"track_id", track_id},
           {"start", start_time},
           {"detect", detect_time},
           {"end", end_time},
           {"active", active},
           {"lat", round_to(pos.lat, 5)},
           {"lon", round_to(pos.lon, 5)},
           {"alt", nullptr},
           {"summary", summary},
           {"details", details}};
    if (altitude_m) j["alt"] = round_to(*altitude_m, 0);
    if (with_track) {
        json t = json::array();
        for (const auto& p : track) {
            t.push_back(json::array({round_to(p.pos.lon, 5), round_to(p.pos.lat, 5),
                                     p.altitude_m ? json(round_to(*p.altitude_m, 0)) : json(nullptr), p.time}));
        }
        j["track"] = std::move(t);
    }
    return j;
}

std::uint64_t AnomalyMetrics::events() const {
    std::uint64_t n = 0;
    for (auto c : events_by_kind) n += c;
    return n;
}

json AnomalyMetrics::to_json() const {
    json by_kind = json::object();
    json per_1000h = json::object();
    for (std::size_t i = 0; i < kEventKinds; ++i) {
        const auto k = to_string(static_cast<EventKind>(i));
        by_kind[k] = events_by_kind[i];
        per_1000h[k] = airborne_aircraft_hours > 0.0
                           ? round_to(1000.0 * static_cast<double>(events_by_kind[i]) / airborne_aircraft_hours, 2)
                           : 0.0;
    }
    return {{"snapshots", snapshots},
            {"events", events()},
            {"events_by_kind", by_kind},
            {"events_per_1000_aircraft_hours", per_1000h},
            {"airborne_aircraft_hours", round_to(airborne_aircraft_hours, 1)},
            {"duration_s", duration_s.to_json(0)}};
}

AnomalyDetector::AnomalyDetector(AnomalyConfig config, std::shared_ptr<const MultiPolygon> area)
    : cfg_(config), area_(std::move(area)) {
    if (area_) coverage_.emplace(*area_, cfg_.coverage_cell_deg, cfg_.coverage_window_h);
}

void AnomalyDetector::append_track(AnomalyEvent& e, const Fix& f) const {
    if (!e.track.empty() && f.time <= e.track.back().time) return;
    e.track.push_back({f.time, f.pos, f.altitude});
    if (e.track.size() > cfg_.event_track_points) {
        e.track.erase(e.track.begin(),
                      e.track.begin() + static_cast<std::ptrdiff_t>(e.track.size() - cfg_.event_track_points));
    }
}

void AnomalyDetector::add_fix(Aircraft& a, const Fix& f) {
    a.fixes.push_back(f);
    a.last_fix_time = f.time;
    while (!a.fixes.empty() && a.fixes.front().time < f.time - cfg_.history_s) a.fixes.pop_front();
    for (const auto& id : a.open) {
        if (!id) continue;
        if (auto it = active_.find(*id); it != active_.end()) append_track(it->second, f);
    }
}

AnomalyEvent& AnomalyDetector::open_event(EventKind kind, const std::string& icao, Aircraft& a, double start,
                                          double now, LatLon pos, std::optional<double> alt, AnomalyReport& out) {
    AnomalyEvent e;
    e.id = next_id_++;
    e.kind = kind;
    e.icao24 = icao;
    e.callsign = a.callsign;
    e.origin_country = a.origin_country;
    e.track_id = a.track_id;
    e.start_time = start;
    e.detect_time = now;
    e.end_time = std::max(start, a.last_fix_time);
    e.pos = pos;
    e.altitude_m = alt;
    for (const auto& f : a.fixes) {
        if (f.time >= start - cfg_.event_track_before_s) append_track(e, f);
    }
    ++metrics_.events_by_kind[idx(kind)];
    a.open[idx(kind)] = e.id;
    auto [it, _] = active_.emplace(e.id, std::move(e));
    out.opened.push_back(it->second);
    return it->second;
}

void AnomalyDetector::touch_event(Aircraft& a, EventKind kind, double end, const json& details, AnomalyReport& out) {
    const auto& id = a.open[idx(kind)];
    if (!id) return;
    auto it = active_.find(*id);
    if (it == active_.end()) return;
    auto& e = it->second;
    if (end <= e.end_time && details == e.details) return;
    e.end_time = std::max(e.end_time, end);
    e.details = details;
    summarize(e);
    // Already reported as opened in this snapshot: that copy is stale now, refresh it.
    for (auto& o : out.opened) {
        if (o.id == e.id) {
            o = e;
            return;
        }
    }
    for (auto& u : out.updated) {
        if (u.id == e.id) {
            u = e;
            return;
        }
    }
    out.updated.push_back(e);
}

void AnomalyDetector::close_event(Aircraft& a, EventKind kind, double end, AnomalyReport& out) {
    auto& id = a.open[idx(kind)];
    if (!id) return;
    auto it = active_.find(*id);
    id.reset();
    if (it == active_.end()) return;
    AnomalyEvent e = std::move(it->second);
    active_.erase(it);
    e.active = false;
    e.end_time = std::max(e.end_time, end);
    if (e.kind == EventKind::Holding) e.details["duration_s"] = round_to(e.end_time - e.start_time, 0);
    summarize(e);
    metrics_.duration_s.add(e.end_time - e.start_time);
    std::erase_if(out.updated, [&e](const AnomalyEvent& u) { return u.id == e.id; });
    for (auto& o : out.opened) {
        if (o.id == e.id) o = e;  // opened and closed in the same snapshot
    }
    out.closed.push_back(e);
    a.closed_at[idx(kind)] = e.end_time;
    closed_.push_front(std::move(e));
    while (closed_.size() > cfg_.recent_events) closed_.pop_back();
}

void AnomalyDetector::close_all(Aircraft& a, double end, AnomalyReport& out) {
    for (std::size_t k = 0; k < kEventKinds; ++k) close_event(a, static_cast<EventKind>(k), end, out);
}

void AnomalyDetector::check_emergency(const std::string& icao, Aircraft& a, const StateVector& s, double now,
                                      AnomalyReport& out) {
    const std::string& code = s.squawk;
    const auto report_time = static_cast<double>(s.last_contact);
    if (code == a.squawk_candidate) {
        ++a.squawk_count;
    } else {
        a.squawk_candidate = code;
        a.squawk_count = 1;
        a.squawk_first_time = report_time;
    }

    const auto& open_id = a.open[idx(EventKind::Emergency)];
    std::string open_code;
    if (open_id) {
        if (auto it = active_.find(*open_id); it != active_.end()) open_code = it->second.details.value("squawk", "");
    }

    if (!open_code.empty()) {
        if (code == open_code) {
            touch_event(a, EventKind::Emergency, report_time, {{"squawk", code}, {"meaning", squawk_meaning(code)}}, out);
            return;
        }
        // Another code: close once it has been seen enough times (a new
        // emergency code is handled below and opens its own event).
        if (a.squawk_count < cfg_.squawk_confirm_reports) return;
        close_event(a, EventKind::Emergency, report_time, out);
    }

    if (!is_emergency_squawk(code) || s.on_ground || a.squawk_count < cfg_.squawk_confirm_reports) return;
    if (a.open[idx(EventKind::Emergency)]) return;
    if (!s.has_position() && a.fixes.empty()) return;
    const LatLon pos = s.has_position() ? LatLon{*s.latitude, *s.longitude} : a.fixes.back().pos;
    std::optional<double> alt = s.baro_altitude ? s.baro_altitude : s.geo_altitude;
    auto& e = open_event(EventKind::Emergency, icao, a, a.squawk_first_time, now, pos, alt, out);
    e.details = {{"squawk", code}, {"meaning", squawk_meaning(code)}};
    summarize(e);
    out.opened.back() = e;
}

void AnomalyDetector::check_descent(const std::string& icao, Aircraft& a, double now, AnomalyReport& out) {
    const auto& f = a.fixes;
    const bool is_open = a.open[idx(EventKind::RapidDescent)].has_value();
    // Walk back from the newest fix while every step goes down.
    std::optional<std::size_t> start;
    if (!f.empty() && f.back().altitude && !f.back().on_ground) {
        std::size_t j = f.size() - 1;
        while (j > 0) {
            const auto& prev = f[j - 1];
            const auto& cur = f[j];
            if (!prev.altitude || prev.on_ground || !(*prev.altitude > *cur.altitude)) break;
            if (cur.time - prev.time > kMaxFixGapS) break;
            if (f.back().time - prev.time > cfg_.descent_window_s && !is_open) break;
            --j;
        }
        start = j;
    }
    bool holds = false;
    double drop = 0.0;
    double rate = 0.0;
    if (start) {
        const auto& s0 = f[*start];
        const auto& s1 = f.back();
        const auto n = static_cast<int>(f.size() - *start);
        const double dt = s1.time - s0.time;
        drop = *s0.altitude - *s1.altitude;
        rate = dt > 0.0 ? drop / dt : 0.0;
        // While open, the event continues as long as the aircraft keeps
        // going down; the opening thresholds only apply to raising it.
        holds = is_open ? n >= 2
                        : (n >= cfg_.descent_min_fixes && drop >= cfg_.descent_min_drop_m &&
                           rate >= cfg_.descent_min_rate_ms && *s0.altitude >= cfg_.descent_min_start_alt_m);
    }
    if (is_open) {
        const auto it = active_.find(*a.open[idx(EventKind::RapidDescent)]);
        if (holds && it != active_.end()) {
            json d = it->second.details;
            const double from = d.value("from_alt_m", 0.0);
            const double to = *f.back().altitude;
            const double dur = f.back().time - it->second.start_time;
            d["to_alt_m"] = round_to(to, 0);
            d["drop_m"] = round_to(from - to, 0);
            d["duration_s"] = round_to(dur, 0);
            d["rate_ms"] = dur > 0.0 ? round_to((from - to) / dur, 1) : 0.0;
            touch_event(a, EventKind::RapidDescent, f.back().time, d, out);
        } else if (!holds) {
            close_event(a, EventKind::RapidDescent, f.empty() ? now : f.back().time, out);
        }
        return;
    }
    if (!holds || f.back().time - a.closed_at[idx(EventKind::RapidDescent)] < kCooldownS) return;
    const auto& s0 = f[*start];
    auto& e = open_event(EventKind::RapidDescent, icao, a, s0.time, now, f.back().pos, f.back().altitude, out);
    e.details = {{"from_alt_m", round_to(*s0.altitude, 0)},
                 {"to_alt_m", round_to(*f.back().altitude, 0)},
                 {"drop_m", round_to(drop, 0)},
                 {"duration_s", round_to(f.back().time - s0.time, 0)},
                 {"rate_ms", round_to(rate, 1)}};
    summarize(e);
    out.opened.back() = e;
}

void AnomalyDetector::check_holding(const std::string& icao, Aircraft& a, double now, AnomalyReport& out) {
    const auto& f = a.fixes;
    const bool is_open = a.open[idx(EventKind::Holding)].has_value();
    if (f.empty()) return;
    const double t_end = f.back().time;
    // Window: every fix in the last `holding_min_duration_s`, plus the one
    // just before it so the window spans at least that long.
    std::size_t j = f.size() - 1;
    while (j > 0 && t_end - f[j].time < cfg_.holding_min_duration_s) --j;
    bool holds = t_end - f[j].time >= cfg_.holding_min_duration_s &&
                 static_cast<int>(f.size() - j) >= cfg_.holding_min_fixes;
    double radius = 0.0;
    LatLon centre{};
    if (holds) {
        double speed_sum = 0.0;
        int speed_n = 0;
        double e_sum = 0.0;
        double n_sum = 0.0;
        const LatLon ref = f[j].pos;
        for (std::size_t i = j; i < f.size() && holds; ++i) {
            if (f[i].on_ground) holds = false;
            if (i > j && f[i].time - f[i - 1].time > kMaxFixGapS) holds = false;
            if (f[i].speed) {
                speed_sum += *f[i].speed;
                ++speed_n;
            }
            const auto d = to_local(ref, f[i].pos);
            e_sum += d.east;
            n_sum += d.north;
        }
        const auto n = static_cast<double>(f.size() - j);
        centre = from_local(ref, {e_sum / n, n_sum / n});
        if (holds) {
            holds = speed_n > 0 && speed_sum / speed_n >= cfg_.holding_min_speed_ms;
        }
        for (std::size_t i = j; i < f.size() && holds; ++i) radius = std::max(radius, haversine_m(centre, f[i].pos));
        holds = holds && radius <= cfg_.holding_max_radius_m;
    }
    if (is_open) {
        auto it = active_.find(*a.open[idx(EventKind::Holding)]);
        if (holds && it != active_.end()) {
            // Path flown since the hold started.
            double path = 0.0;
            for (std::size_t i = 1; i < f.size(); ++i) {
                if (f[i - 1].time >= it->second.start_time) path += haversine_m(f[i - 1].pos, f[i].pos);
            }
            json d = it->second.details;
            d["duration_s"] = round_to(t_end - it->second.start_time, 0);
            d["radius_m"] = std::max(d.value("radius_m", 0.0), round_to(radius, 0));
            d["path_km"] = round_to(path / 1000.0, 1);
            touch_event(a, EventKind::Holding, t_end, d, out);
        } else if (!holds) {
            close_event(a, EventKind::Holding, a.last_fix_time, out);
        }
        return;
    }
    if (!holds || t_end - a.closed_at[idx(EventKind::Holding)] < kCooldownS) return;
    double path = 0.0;
    for (std::size_t i = j + 1; i < f.size(); ++i) path += haversine_m(f[i - 1].pos, f[i].pos);
    auto& e = open_event(EventKind::Holding, icao, a, f[j].time, now, centre, f.back().altitude, out);
    e.details = {{"duration_s", round_to(t_end - f[j].time, 0)},
                 {"radius_m", round_to(radius, 0)},
                 {"path_km", round_to(path / 1000.0, 1)},
                 {"centre", {round_to(centre.lat, 4), round_to(centre.lon, 4)}}};
    summarize(e);
    out.opened.back() = e;
}

void AnomalyDetector::check_deviation(const std::string& icao, Aircraft& a, double now, AnomalyReport& out) {
    const auto& f = a.fixes;
    if (a.open[idx(EventKind::Holding)]) return;  // loops in a hold are not a route change
    const auto course_pts = static_cast<std::size_t>(std::max(2, cfg_.deviation_course_fixes));
    // New course: the last 3 fixes (2 legs). Between the old and the new
    // course there may be one turn leg, which is ignored.
    for (std::size_t turn_legs = 0; turn_legs <= 1; ++turn_legs) {
        const std::size_t need = course_pts + turn_legs + 3;
        if (f.size() < need) return;
        const std::size_t b = f.size() - need;  // first fix of the old course
        bool ok = true;
        for (std::size_t i = b; i < f.size() && ok; ++i) {
            if (f[i].on_ground || !f[i].altitude || *f[i].altitude < cfg_.deviation_min_alt_m) ok = false;
            if (i > b && f[i].time - f[i - 1].time > kMaxFixGapS) ok = false;
        }
        if (!ok) continue;
        auto leg_ok = [&](std::size_t i) { return haversine_m(f[i].pos, f[i + 1].pos) >= cfg_.deviation_min_leg_m; };
        const std::size_t oe = b + course_pts - 1;  // last fix of the old course
        const double old_course = bearing_deg(f[b].pos, f[oe].pos);
        for (std::size_t i = b; i < oe && ok; ++i) {
            ok = leg_ok(i) && bearing_diff(bearing_deg(f[i].pos, f[i + 1].pos), old_course) <= cfg_.deviation_straight_tol_deg;
        }
        if (!ok) continue;
        const std::size_t ns = f.size() - 3;  // first fix of the new course
        const double new_course = bearing_deg(f[ns].pos, f.back().pos);
        for (std::size_t i = ns; i + 1 < f.size() && ok; ++i) {
            ok = leg_ok(i) &&
                 bearing_diff(bearing_deg(f[i].pos, f[i + 1].pos), new_course) <= cfg_.deviation_new_course_tol_deg;
        }
        if (!ok) continue;
        const double turn = bearing_diff(old_course, new_course);
        if (turn < cfg_.deviation_min_turn_deg) continue;
        if (f.back().time - a.closed_at[idx(EventKind::RouteDeviation)] < kCooldownS) return;
        // A course change is a moment, not a state: the event opens and
        // closes at once, spanning the turn.
        auto& e = open_event(EventKind::RouteDeviation, icao, a, f[oe].time, now, f[oe].pos, f[oe].altitude, out);
        e.details = {{"old_course_deg", round_to(old_course, 0)},
                     {"new_course_deg", round_to(new_course, 0)},
                     {"turn_deg", round_to(turn, 0)}};
        summarize(e);
        out.opened.back() = e;
        close_event(a, EventKind::RouteDeviation, f.back().time, out);
        return;
    }
}

void AnomalyDetector::check_signal_loss(const Tracker& tracker, double now, AnomalyReport& out) {
    if (!area_ || !coverage_) return;
    for (const auto& v : tracker.tracks_at(now)) {
        auto it = aircraft_.find(v.icao24);
        if (it == aircraft_.end()) continue;
        Aircraft& a = it->second;
        const double gap = now - v.last_fix_time;
        const bool is_open = a.open[idx(EventKind::SignalLoss)].has_value();
        if (is_open) {
            touch_event(a, EventKind::SignalLoss, now, {{"gap_s", round_to(gap, 0)}, {"reacquired", false}}, out);
            continue;
        }
        if (v.state != TrackState::Coasting || v.updates < 3 || gap < cfg_.signal_loss_after_s) continue;
        if (a.fixes.empty()) continue;
        const Fix& last = a.fixes.back();
        if (last.on_ground || !last.altitude || *last.altitude < cfg_.signal_loss_min_alt_m) continue;
        if (!area_->contains(v.pos)) continue;
        const auto cov = coverage_->count_at(last.pos, now);
        if (cov < cfg_.signal_loss_min_coverage) continue;
        if (last.time - a.closed_at[idx(EventKind::SignalLoss)] < kCooldownS) continue;
        auto& e = open_event(EventKind::SignalLoss, v.icao24, a, last.time, now, last.pos, last.altitude, out);
        e.details = {{"gap_s", round_to(gap, 0)}, {"reacquired", false}, {"coverage_reports", cov}};
        summarize(e);
        out.opened.back() = e;
    }
}

AnomalyReport AnomalyDetector::process(const StatesSnapshot& snapshot, const Tracker& tracker,
                                       const SnapshotReport& report) {
    AnomalyReport out;
    const auto now = static_cast<double>(snapshot.time);
    ++metrics_.snapshots;

    // Tracks that ended (or restarted) in this snapshot: their open events end.
    for (const auto& ended : report.ended) {
        auto it = aircraft_.find(ended.icao24);
        if (it == aircraft_.end()) continue;
        Aircraft& a = it->second;
        if (const auto& id = a.open[idx(EventKind::SignalLoss)]) {
            if (auto ev = active_.find(*id); ev != active_.end()) {
                ev->second.details["reacquired"] = false;
                ev->second.details["gap_s"] = round_to(now - ended.last_fix_time, 0);
                ev->second.details["track_end"] = to_string(ended.reason);
            }
        }
        // A restarted track had inconsistent positions; its geometry is not trustworthy.
        close_all(a, ended.reason == EndReason::SignalLost ? now : ended.last_fix_time, out);
        a.fixes.clear();
        a.last_fix_time = -1.0;
    }

    // New fixes, from the tracker (filtered position and altitude, after the
    // quality checks, so a rejected or corrupt report never feeds a rule).
    std::unordered_set<std::string> fresh;
    const auto views = tracker.tracks_at(now);
    double airborne = 0.0;
    for (const auto& v : views) {
        if (v.state != TrackState::Tentative && !v.on_ground && now - v.last_fix_time < 180.0) airborne += 1.0;
        auto& a = aircraft_[v.icao24];
        a.last_seen = now;
        if (!v.callsign.empty()) a.callsign = v.callsign;
        if (a.track_id != v.id) {
            a.track_id = v.id;
            a.fixes.clear();
            a.last_fix_time = -1.0;
        }
        if (v.updates == 0 || v.last_fix_time <= a.last_fix_time) continue;
        const auto at_fix = tracker.track_at(v.icao24, v.last_fix_time);
        if (!at_fix) continue;
        // Signal back: close the loss event before the new fix is added.
        if (const auto& id = a.open[idx(EventKind::SignalLoss)]) {
            if (auto ev = active_.find(*id); ev != active_.end()) {
                ev->second.details["reacquired"] = true;
                ev->second.details["gap_s"] = round_to(v.last_fix_time - a.last_fix_time, 0);
            }
            close_event(a, EventKind::SignalLoss, v.last_fix_time, out);
        }
        const Fix fix{v.last_fix_time, at_fix->pos, at_fix->altitude_m, at_fix->speed_ms, at_fix->on_ground};
        add_fix(a, fix);
        if (coverage_) coverage_->add(fix.pos, fix.time);
        fresh.insert(v.icao24);
    }
    if (last_time_ > 0.0 && now > last_time_ && now - last_time_ < 600.0) {
        metrics_.airborne_aircraft_hours += airborne * (now - last_time_) / 3600.0;
    }
    last_time_ = now;

    // Squawk works on every fresh report, with or without a new position.
    for (const auto& s : snapshot.states) {
        auto it = aircraft_.find(s.icao24);
        if (it == aircraft_.end()) continue;
        Aircraft& a = it->second;
        if (!s.origin_country.empty()) a.origin_country = s.origin_country;
        if (static_cast<double>(s.last_contact) <= a.last_contact) continue;
        a.last_contact = static_cast<double>(s.last_contact);
        check_emergency(s.icao24, a, s, now, out);
    }

    for (const auto& icao : fresh) {
        Aircraft& a = aircraft_[icao];
        check_descent(icao, a, now, out);
        check_holding(icao, a, now, out);
        check_deviation(icao, a, now, out);
    }
    check_signal_loss(tracker, now, out);

    // Forget aircraft not seen for a while.
    for (auto it = aircraft_.begin(); it != aircraft_.end();) {
        if (now - it->second.last_seen > cfg_.history_s) {
            close_all(it->second, it->second.last_fix_time > 0 ? it->second.last_fix_time : now, out);
            it = aircraft_.erase(it);
        } else {
            ++it;
        }
    }
    return out;
}

std::vector<AnomalyEvent> AnomalyDetector::active() const {
    std::vector<AnomalyEvent> out;
    out.reserve(active_.size());
    for (const auto& [_, e] : active_) out.push_back(e);
    std::sort(out.begin(), out.end(), [](const AnomalyEvent& x, const AnomalyEvent& y) {
        return x.start_time != y.start_time ? x.start_time > y.start_time : x.id > y.id;
    });
    return out;
}

std::vector<AnomalyEvent> AnomalyDetector::recent() const {
    auto out = active();
    for (const auto& e : closed_) {
        if (out.size() >= cfg_.recent_events) break;
        out.push_back(e);
    }
    if (out.size() > cfg_.recent_events) out.resize(cfg_.recent_events);
    return out;
}

json AnomalyDetector::events_json() const {
    json events = json::array();
    for (const auto& e : recent()) events.push_back(e.to_json(true));
    json counts = json::object();
    for (std::size_t i = 0; i < kEventKinds; ++i) {
        counts[to_string(static_cast<EventKind>(i))] = metrics_.events_by_kind[i];
    }
    return {{"type", "events"},
            {"protocol", kLiveProtocolVersion},
            {"time", last_time_},
            {"counts", counts},
            {"active", active_.size()},
            {"events", std::move(events)}};
}

EventLog::EventLog(const std::filesystem::path& path) : out_(path, std::ios::app) {
    if (!out_) throw std::runtime_error("cannot open event log " + path.string());
}

void EventLog::write(const AnomalyReport& report) {
    auto line = [this](const char* change, const AnomalyEvent& e) {
        json j = e.to_json(true);
        j["change"] = change;
        out_ << j.dump() << '\n';
    };
    for (const auto& e : report.opened) line("opened", e);
    for (const auto& e : report.closed) line("closed", e);
    out_.flush();
    if (!out_) throw std::runtime_error("write to event log failed");
}

}  // namespace sema
