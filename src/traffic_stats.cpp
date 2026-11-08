#include "sema/traffic_stats.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <map>
#include <set>
#include <string>
#include <unordered_map>
#include <utility>

namespace sema {
namespace {

std::int64_t local_day(double t, int offset_s) {
    return static_cast<std::int64_t>(std::floor((t + offset_s) / 86400.0));
}

std::string date_string(std::int64_t day) {
    const std::chrono::year_month_day ymd{std::chrono::sys_days{std::chrono::days{day}}};
    char buf[16];
    (void)std::snprintf(buf, sizeof buf, "%04d-%02u-%02u", static_cast<int>(ymd.year()),
                        static_cast<unsigned>(ymd.month()), static_cast<unsigned>(ymd.day()));
    return buf;
}

struct DayCounts {
    std::uint64_t flights = 0;
    std::uint64_t departures = 0;
    std::uint64_t arrivals = 0;
    std::set<std::string> aircraft;
};

struct AirportCounts {
    std::uint64_t departures = 0;
    std::uint64_t arrivals = 0;
};

}  // namespace

nlohmann::json compute_traffic_stats(const std::vector<FlightRecord>& flights, const AirportIndex& airports,
                                     const StatsQuery& q) {
    const int off = q.utc_offset_s;
    const std::int64_t day0 = local_day(q.from, off);
    const std::int64_t day1 = q.to > q.from ? local_day(q.to - 1e-6, off) : day0;
    // Cap the day list so a silly range cannot build a huge reply.
    const std::int64_t ndays = std::clamp<std::int64_t>(day1 - day0 + 1, 1, 400);

    std::map<std::int64_t, DayCounts> days;
    for (std::int64_t d = day0; d < day0 + ndays; ++d) days[d];
    std::array<std::uint64_t, 24> hours{};
    std::unordered_map<std::string, AirportCounts> by_airport;
    std::map<std::pair<std::string, std::string>, std::uint64_t> routes;
    std::unordered_map<std::uint32_t, std::uint64_t> cells;
    std::set<std::string> aircraft;
    std::uint64_t total = 0;
    std::uint64_t departures = 0;
    std::uint64_t arrivals = 0;

    for (const auto& f : flights) {
        if (!f.counts() || f.first_seen < q.from || f.first_seen >= q.to) continue;
        ++total;
        aircraft.insert(f.icao24);
        auto it = days.find(local_day(f.first_seen, off));
        DayCounts* dc = it != days.end() ? &it->second : nullptr;
        if (dc != nullptr) {
            ++dc->flights;
            dc->aircraft.insert(f.icao24);
        }
        // Every local clock hour the flight was in the air in.
        const auto h0 = static_cast<std::int64_t>(std::floor((f.first_seen + off) / 3600.0));
        const auto h1 = static_cast<std::int64_t>(std::floor((std::max(f.last_seen, f.first_seen) + off) / 3600.0));
        for (std::int64_t h = h0; h <= std::min(h1, h0 + 23); ++h) {
            ++hours[static_cast<std::size_t>(((h % 24) + 24) % 24)];
        }
        if (!f.departure.empty()) {
            ++departures;
            ++by_airport[f.departure].departures;
            if (dc != nullptr) ++dc->departures;
        }
        if (!f.arrival.empty()) {
            ++arrivals;
            ++by_airport[f.arrival].arrivals;
            if (dc != nullptr) ++dc->arrivals;
        }
        if (!f.departure.empty() && !f.arrival.empty() && f.departure != f.arrival) {
            ++routes[{f.departure, f.arrival}];
        }
        for (const auto c : f.cells) ++cells[c];
    }

    nlohmann::json j;
    j["from"] = q.from;
    j["to"] = q.to;
    j["utc_offset_s"] = off;
    j["totals"] = {{"flights", total},
                   {"aircraft", aircraft.size()},
                   {"departures", departures},
                   {"arrivals", arrivals},
                   {"days", ndays}};

    auto daily = nlohmann::json::array();
    std::string busiest_day;
    std::uint64_t busiest = 0;
    for (const auto& [d, c] : days) {
        const auto date = date_string(d);
        daily.push_back({{"date", date},
                         {"flights", c.flights},
                         {"aircraft", c.aircraft.size()},
                         {"departures", c.departures},
                         {"arrivals", c.arrivals}});
        if (c.flights > busiest) {
            busiest = c.flights;
            busiest_day = date;
        }
    }
    j["daily"] = std::move(daily);
    j["busiest_day"] = busiest > 0 ? nlohmann::json(busiest_day) : nlohmann::json(nullptr);

    auto hj = nlohmann::json::array();
    std::size_t peak = 0;
    for (std::size_t h = 0; h < 24; ++h) {
        const double avg = static_cast<double>(hours[h]) / static_cast<double>(ndays);
        hj.push_back({{"hour", h}, {"total", hours[h]}, {"avg", std::round(avg * 10.0) / 10.0}});
        if (hours[h] > hours[peak]) peak = h;
    }
    j["hours"] = std::move(hj);
    j["peak_hour"] = total > 0 ? nlohmann::json(peak) : nlohmann::json(nullptr);

    std::vector<std::pair<std::string, AirportCounts>> ap(by_airport.begin(), by_airport.end());
    std::sort(ap.begin(), ap.end(), [](const auto& a, const auto& b) {
        const auto ta = a.second.departures + a.second.arrivals;
        const auto tb = b.second.departures + b.second.arrivals;
        return ta != tb ? ta > tb : a.first < b.first;
    });
    auto aj = nlohmann::json::array();
    for (const auto& [icao, c] : ap) {
        const Airport* a = airports.find(icao);
        aj.push_back({{"icao", icao},
                      {"iata", a ? a->iata : ""},
                      {"name", a ? a->name : ""},
                      {"city", a ? a->city : ""},
                      {"lat", a ? a->pos.lat : 0.0},
                      {"lon", a ? a->pos.lon : 0.0},
                      {"departures", c.departures},
                      {"arrivals", c.arrivals},
                      {"total", c.departures + c.arrivals}});
    }
    j["airports"] = std::move(aj);

    std::vector<std::pair<std::pair<std::string, std::string>, std::uint64_t>> rv(routes.begin(), routes.end());
    std::stable_sort(rv.begin(), rv.end(), [](const auto& a, const auto& b) { return a.second > b.second; });
    if (rv.size() > q.top_n) rv.resize(q.top_n);
    auto rj = nlohmann::json::array();
    for (const auto& [k, n] : rv) rj.push_back({{"from", k.first}, {"to", k.second}, {"flights", n}});
    j["routes"] = std::move(rj);

    std::vector<std::pair<std::uint32_t, std::uint64_t>> cv(cells.begin(), cells.end());
    std::sort(cv.begin(), cv.end(),
              [](const auto& a, const auto& b) { return a.second != b.second ? a.second > b.second : a.first < b.first; });
    auto cj = nlohmann::json::array();
    for (const auto& [cell, n] : cv) {
        const auto o = corridor_cell_origin(cell);
        cj.push_back({std::round(o.lat * 100.0) / 100.0, std::round(o.lon * 100.0) / 100.0, n});
    }
    j["corridors"] = {{"cell_deg", kCorridorCellDeg},
                      {"max", cv.empty() ? 0 : cv.front().second},
                      {"cells", std::move(cj)}};
    return j;
}

}  // namespace sema
