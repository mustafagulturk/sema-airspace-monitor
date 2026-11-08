#include "sema/history.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <exception>
#include <utility>

namespace sema {

// ---------------------------------------------------------------- HistorySource

void HistorySource::set_open_flights(std::vector<FlightRecord> open) {
    const std::scoped_lock lock(open_mu_);
    open_ = std::move(open);
}

std::vector<FlightRecord> HistorySource::open_flights(double from, double to) const {
    const std::scoped_lock lock(open_mu_);
    std::vector<FlightRecord> out;
    for (const auto& f : open_) {
        if (f.first_seen >= from && f.first_seen < to) out.push_back(f);
    }
    return out;
}

// ---------------------------------------------------------------- MemoryHistory

MemoryHistory::MemoryHistory(Limits limits) : limits_(limits) {}

void MemoryHistory::record(const StatesSnapshot& snapshot, const std::vector<FlightRecord>& ended) {
    const std::scoped_lock lock(mu_);
    const auto t = static_cast<double>(snapshot.time);
    if (!streak_start_ || t - newest_ > kStreakGapS) streak_start_ = t;
    newest_ = std::max(newest_, t);
    for (const auto& s : snapshot.states) {
        if (!s.has_position()) continue;
        const double ft = static_cast<double>(s.time_position.value_or(s.last_contact));
        auto [it, inserted] = index_.try_emplace(s.icao24, static_cast<std::uint32_t>(aircraft_.size()));
        if (inserted) aircraft_.push_back({s.icao24, s.callsign, s.origin_country, -1.0});
        auto& meta = aircraft_[it->second];
        if (!s.callsign.empty()) meta.callsign = s.callsign;
        // OpenSky repeats the last position until a new one arrives.
        if (ft <= meta.last_t) continue;
        meta.last_t = ft;
        ReplayFix f;
        f.t = ft;
        f.lat = s.latitude.value_or(0.0);  // has_position() checked above
        f.lon = s.longitude.value_or(0.0);
        f.alt_m = s.baro_altitude ? s.baro_altitude : s.geo_altitude;
        f.speed_ms = s.velocity;
        f.track_deg = s.true_track;
        f.on_ground = s.on_ground;
        fixes_.push_back({it->second, f});
    }
    for (const auto& f : ended) flights_.push_back(f);
    prune_locked();
}

void MemoryHistory::prune_locked() {
    while (!fixes_.empty() &&
           (fixes_.size() > limits_.max_fixes || fixes_.front().fix.t < newest_ - limits_.keep_fixes_s)) {
        fixes_.pop_front();
    }
    while (!flights_.empty() &&
           (flights_.size() > limits_.max_flights || flights_.front().last_seen < newest_ - limits_.keep_flights_s)) {
        flights_.pop_front();
    }
}

std::optional<TimeRange> MemoryHistory::available() {
    const std::scoped_lock lock(mu_);
    if (fixes_.empty()) return std::nullopt;
    double lo = fixes_.front().fix.t;
    // Arrival order is snapshot order; a report can be a little older than the
    // snapshot it came in, so look at the first few for the true minimum.
    for (std::size_t i = 0; i < std::min<std::size_t>(fixes_.size(), 2000); ++i) lo = std::min(lo, fixes_[i].fix.t);
    return TimeRange{lo, newest_};
}

std::vector<ReplayAircraft> MemoryHistory::replay(double from, double to) {
    const std::scoped_lock lock(mu_);
    std::unordered_map<std::uint32_t, std::size_t> slot;
    std::vector<ReplayAircraft> out;
    for (const auto& s : fixes_) {
        if (s.fix.t < from || s.fix.t > to) continue;
        auto [it, inserted] = slot.try_emplace(s.aircraft, out.size());
        if (inserted) {
            const auto& m = aircraft_[s.aircraft];
            out.push_back({m.icao24, m.callsign, m.origin_country, {}});
        }
        out[it->second].fixes.push_back(s.fix);
    }
    for (auto& a : out) {
        std::stable_sort(a.fixes.begin(), a.fixes.end(), [](const auto& x, const auto& y) { return x.t < y.t; });
    }
    return out;
}

std::vector<FlightRecord> MemoryHistory::flights(double from, double to) {
    const std::scoped_lock lock(mu_);
    std::vector<FlightRecord> out;
    for (const auto& f : flights_) {
        if (f.first_seen >= from && f.first_seen < to) out.push_back(f);
    }
    return out;
}

std::optional<TimeRange> MemoryHistory::collection_streak() {
    const std::scoped_lock lock(mu_);
    if (!streak_start_) return std::nullopt;
    return TimeRange{*streak_start_, newest_};
}

std::size_t MemoryHistory::fix_count() const {
    const std::scoped_lock lock(mu_);
    return fixes_.size();
}

std::size_t MemoryHistory::flight_count() const {
    const std::scoped_lock lock(mu_);
    return flights_.size();
}

// ---------------------------------------------------------------- query parsing

namespace {

int hex_value(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

std::string url_decode(const std::string& s) {
    std::string out;
    out.reserve(s.size());
    for (std::size_t i = 0; i < s.size(); ++i) {
        if (s[i] == '+') {
            out.push_back(' ');
        } else if (s[i] == '%' && i + 2 < s.size() && hex_value(s[i + 1]) >= 0 && hex_value(s[i + 2]) >= 0) {
            out.push_back(static_cast<char>(hex_value(s[i + 1]) * 16 + hex_value(s[i + 2])));
            i += 2;
        } else {
            out.push_back(s[i]);
        }
    }
    return out;
}

std::optional<double> number_param(const ApiRequest& req, const std::string& key) {
    const auto it = req.query.find(key);
    if (it == req.query.end() || it->second.empty()) return std::nullopt;
    std::size_t used = 0;
    double v = 0.0;
    try {
        v = std::stod(it->second, &used);
    } catch (const std::exception&) {
        throw std::invalid_argument("bad '" + key + "'");
    }
    if (used != it->second.size() || !std::isfinite(v)) throw std::invalid_argument("bad '" + key + "'");
    return v;
}

ApiResponse json_response(const nlohmann::json& j, int status = 200, int max_age_s = 0) {
    ApiResponse r;
    r.status = status;
    r.body = j.dump(-1, ' ', false, nlohmann::json::error_handler_t::replace);
    r.max_age_s = max_age_s;
    return r;
}

ApiResponse error_response(int status, const std::string& message) {
    return json_response({{"error", message}}, status);
}

double round_to(double x, double step) { return std::round(x / step) * step; }

nlohmann::json opt_num(const std::optional<double>& v, double step) {
    return v ? nlohmann::json(round_to(*v, step)) : nlohmann::json(nullptr);
}

}  // namespace

std::map<std::string, std::string> parse_query(const std::string& query) {
    std::map<std::string, std::string> out;
    std::size_t pos = 0;
    while (pos <= query.size()) {
        const auto amp = query.find('&', pos);
        const auto part = query.substr(pos, amp == std::string::npos ? std::string::npos : amp - pos);
        if (!part.empty()) {
            const auto eq = part.find('=');
            if (eq == std::string::npos) {
                out[url_decode(part)] = "";
            } else {
                out[url_decode(part.substr(0, eq))] = url_decode(part.substr(eq + 1));
            }
        }
        if (amp == std::string::npos) break;
        pos = amp + 1;
    }
    return out;
}

nlohmann::json replay_to_json(const std::vector<ReplayAircraft>& aircraft, double from, double to) {
    auto list = nlohmann::json::array();
    std::size_t fixes = 0;
    for (const auto& a : aircraft) {
        auto fx = nlohmann::json::array();
        for (const auto& f : a.fixes) {
            fx.push_back({f.t, round_to(f.lat, 1e-5), round_to(f.lon, 1e-5), opt_num(f.alt_m, 1.0),
                          opt_num(f.speed_ms, 0.1), opt_num(f.track_deg, 0.1), f.on_ground ? 1 : 0});
        }
        fixes += a.fixes.size();
        list.push_back({{"icao24", a.icao24}, {"callsign", a.callsign}, {"country", a.origin_country}, {"fixes", std::move(fx)}});
    }
    return {{"from", from}, {"to", to}, {"fixes", fixes}, {"aircraft", std::move(list)}};
}

// ---------------------------------------------------------------- HistoryApi

HistoryApi::HistoryApi(std::shared_ptr<HistorySource> source, std::shared_ptr<const AirportIndex> airports,
                       HistoryApiConfig config, std::function<double()> now)
    : source_(std::move(source)), airports_(std::move(airports)), cfg_(std::move(config)), now_(std::move(now)) {
    if (!now_) {
        now_ = [] { return std::chrono::duration<double>(std::chrono::system_clock::now().time_since_epoch()).count(); };
    }
    if (!airports_) airports_ = std::make_shared<const AirportIndex>();
    started_at_ = now_();
}

nlohmann::json HistoryApi::status_json() {
    const double now = now_();
    nlohmann::json j{{"mode", cfg_.mode},
                     {"label", cfg_.label},
                     {"now", std::round(now)},
                     {"started_at", std::round(started_at_)},
                     {"uptime_s", std::round(now - started_at_)},
                     {"max_replay_s", cfg_.max_replay_s},
                     {"max_stats_s", cfg_.max_stats_s},
                     {"utc_offset_s", cfg_.utc_offset_s},
                     {"airports", airports_->size()}};
    if (const auto r = source_->available()) {
        j["history"] = {{"from", r->from}, {"to", r->to}};
    } else {
        j["history"] = nullptr;
    }
    if (const auto s = source_->collection_streak()) {
        j["streak"] = {{"from", s->from}, {"to", s->to}, {"seconds", std::round(s->to - s->from)}};
    } else {
        j["streak"] = nullptr;
    }
    return j;
}

std::optional<ApiResponse> HistoryApi::handle(const ApiRequest& req) {
    if (!req.path.starts_with("/api/")) return std::nullopt;
    try {
        if (req.path == "/api/status") return json_response(status_json());
        if (req.path == "/api/replay") return replay(req);
        if (req.path == "/api/stats") return stats(req);
    } catch (const std::invalid_argument& e) {
        return error_response(400, e.what());
    } catch (const std::exception& e) {
        return error_response(503, std::string("history unavailable: ") + e.what());
    }
    return error_response(404, "unknown endpoint");
}

ApiResponse HistoryApi::replay(const ApiRequest& req) {
    const auto from = number_param(req, "from");
    const auto to = number_param(req, "to");
    if (!from || !to) throw std::invalid_argument("'from' and 'to' are required");
    if (!(*to > *from)) throw std::invalid_argument("'to' must be after 'from'");
    if (*to - *from > cfg_.max_replay_s + 1.0) {
        throw std::invalid_argument("range longer than " + std::to_string(static_cast<long>(cfg_.max_replay_s)) + " s");
    }
    const auto j = replay_to_json(source_->replay(*from, *to), *from, *to);
    // A range that ended a while ago no longer changes.
    const bool settled = *to < now_() - 600.0 || cfg_.mode == "replay";
    return json_response(j, 200, settled ? 3600 : 0);
}

ApiResponse HistoryApi::stats(const ApiRequest& req) {
    const auto to_param = number_param(req, "to");
    const auto from_param = number_param(req, "from");
    double to = 0.0;
    if (to_param) {
        to = *to_param;
    } else {
        // Default: up to the newest data (the recording's end when replaying).
        const auto r = source_->available();
        to = cfg_.mode == "replay" && r ? r->to + 1.0 : now_();
    }
    const double from = from_param ? *from_param : to - cfg_.default_stats_s;
    if (!(to > from)) throw std::invalid_argument("'to' must be after 'from'");
    if (to - from > cfg_.max_stats_s + 1.0) throw std::invalid_argument("range too long");

    // Cache per minute-rounded range: the numbers move slowly and the query
    // can scan a month of flights.
    const std::pair<std::int64_t, std::int64_t> key{static_cast<std::int64_t>(from / 60.0),
                                                    static_cast<std::int64_t>(to / 60.0)};
    const double now = now_();
    {
        const std::scoped_lock lock(cache_mu_);
        if (auto it = stats_cache_.find(key); it != stats_cache_.end() && now - it->second.first < cfg_.stats_cache_s) {
            ApiResponse r;
            r.body = it->second.second;
            r.max_age_s = static_cast<int>(cfg_.stats_cache_s);
            return r;
        }
    }
    auto flights = source_->flights(from, to);
    auto open = source_->open_flights(from, to);
    const std::size_t completed = flights.size();
    flights.insert(flights.end(), std::make_move_iterator(open.begin()), std::make_move_iterator(open.end()));

    StatsQuery q;
    q.from = from;
    q.to = to;
    q.utc_offset_s = cfg_.utc_offset_s;
    auto j = compute_traffic_stats(flights, *airports_, q);
    j["completed_flights"] = completed;
    j["open_flights"] = flights.size() - completed;
    j["generated_at"] = std::round(now);
    ApiResponse r = json_response(j, 200, static_cast<int>(cfg_.stats_cache_s));
    {
        const std::scoped_lock lock(cache_mu_);
        if (stats_cache_.size() >= 32) stats_cache_.clear();
        stats_cache_[key] = {now, r.body};
    }
    return r;
}

}  // namespace sema
