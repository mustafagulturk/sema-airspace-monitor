#include "sema/pg_history.hpp"

#include <charconv>
#include <optional>
#include <string_view>
#include <utility>

#include "pqxx_compat.hpp"

namespace sema {

struct PgHistory::Impl {
    explicit Impl(const std::string& conninfo) : conn(conninfo) {}
    pqxx::connection conn;
};

PgHistory::PgHistory(std::string conninfo) : conninfo_(std::move(conninfo)) {}
PgHistory::~PgHistory() = default;

namespace {

// Runs `fn(tx)` in a read-only transaction, reconnecting once if the
// connection was lost (e.g. the database restarted).
template <class Fn>
auto with_tx(std::unique_ptr<PgHistory::Impl>& impl, const std::string& conninfo, Fn&& fn) {
    for (int attempt = 0;; ++attempt) {
        try {
            if (!impl) impl = std::make_unique<PgHistory::Impl>(conninfo);
            pqxx::read_transaction tx(impl->conn);
            auto out = fn(tx);
            tx.commit();
            return out;
        } catch (const pqxx::broken_connection&) {
            impl.reset();
            if (attempt >= 1) throw;
        }
    }
}

std::optional<double> opt_double(const pqxx::field& f) {
    if (f.is_null()) return std::nullopt;
    return f.as<double>();
}

std::vector<std::uint32_t> parse_cells(std::string_view s) {
    std::vector<std::uint32_t> out;
    while (!s.empty()) {
        const auto comma = s.find(',');
        const auto part = s.substr(0, comma);
        std::uint32_t v = 0;
        if (std::from_chars(part.data(), part.data() + part.size(), v).ec == std::errc{}) out.push_back(v);
        if (comma == std::string_view::npos) break;
        s.remove_prefix(comma + 1);
    }
    return out;
}

}  // namespace

std::optional<TimeRange> PgHistory::available() {
    const std::scoped_lock lock(mu_);
    return with_tx(impl_, conninfo_, [](pqxx::read_transaction& tx) -> std::optional<TimeRange> {
        const auto r = tx.exec1(
            "SELECT extract(epoch FROM min(fetched_at))::float8, extract(epoch FROM max(fetched_at))::float8 "
            "FROM positions");
        if (r[0].is_null() || r[1].is_null()) return std::nullopt;
        return TimeRange{r[0].as<double>(), r[1].as<double>()};
    });
}

std::vector<ReplayAircraft> PgHistory::replay(double from, double to) {
    const std::scoped_lock lock(mu_);
    return with_tx(impl_, conninfo_, [from, to](pqxx::read_transaction& tx) {
        // Rows are filtered on fetched_at (indexed) with a margin, then on the
        // report's own time.
        const auto rows = tx.exec_params(
            "SELECT icao24, coalesce(callsign, ''), coalesce(origin_country, ''), "
            "       extract(epoch FROM coalesce(time_position, last_contact))::float8 AS t, "
            "       ST_Y(geom::geometry), ST_X(geom::geometry), coalesce(baro_altitude_m, geo_altitude_m), "
            "       velocity_ms, true_track_deg, on_ground "
            "FROM positions "
            "WHERE fetched_at >= to_timestamp($1) - interval '5 minutes' "
            "  AND fetched_at <= to_timestamp($2) + interval '5 minutes' "
            "ORDER BY icao24, t",
            from, to);
        std::vector<ReplayAircraft> out;
        for (const auto& row : rows) {
            const double t = row[3].as<double>();
            if (t < from || t > to) continue;
            const auto icao = row[0].as<std::string>();
            if (out.empty() || out.back().icao24 != icao) {
                out.push_back({icao, row[1].as<std::string>(), row[2].as<std::string>(), {}});
            }
            auto& a = out.back();
            if (const auto cs = row[1].as<std::string>(); !cs.empty()) a.callsign = cs;
            // OpenSky repeats a position until a new one arrives.
            if (!a.fixes.empty() && a.fixes.back().t >= t) continue;
            ReplayFix f;
            f.t = t;
            f.lat = row[4].as<double>();
            f.lon = row[5].as<double>();
            f.alt_m = opt_double(row[6]);
            f.speed_ms = opt_double(row[7]);
            f.track_deg = opt_double(row[8]);
            f.on_ground = row[9].as<bool>();
            a.fixes.push_back(f);
        }
        return out;
    });
}

std::vector<FlightRecord> PgHistory::flights(double from, double to) {
    const std::scoped_lock lock(mu_);
    return with_tx(impl_, conninfo_, [from, to](pqxx::read_transaction& tx) {
        const auto rows = tx.exec_params(
            "SELECT track_id, icao24, coalesce(callsign, ''), coalesce(origin_country, ''), "
            "       extract(epoch FROM first_seen)::float8, extract(epoch FROM last_seen)::float8, "
            "       ST_Y(first_pos::geometry), ST_X(first_pos::geometry), "
            "       ST_Y(last_pos::geometry), ST_X(last_pos::geometry), min_altitude_m, max_altitude_m, fixes, "
            "       airborne, coalesce(departure, ''), coalesce(arrival, ''), end_reason, "
            "       array_to_string(cells, ',') "
            "FROM flights WHERE first_seen >= to_timestamp($1) AND first_seen < to_timestamp($2) "
            "ORDER BY first_seen",
            from, to);
        std::vector<FlightRecord> out;
        out.reserve(rows.size());
        for (const auto& row : rows) {
            FlightRecord f;
            f.track_id = row[0].as<std::uint64_t>();
            f.icao24 = row[1].as<std::string>();
            f.callsign = row[2].as<std::string>();
            f.origin_country = row[3].as<std::string>();
            f.first_seen = row[4].as<double>();
            f.last_seen = row[5].as<double>();
            f.first_pos = {row[6].as<double>(), row[7].as<double>()};
            f.last_pos = {row[8].as<double>(), row[9].as<double>()};
            f.min_altitude_m = opt_double(row[10]);
            f.max_altitude_m = opt_double(row[11]);
            f.fixes = row[12].as<int>();
            f.airborne = row[13].as<bool>();
            f.departure = row[14].as<std::string>();
            f.arrival = row[15].as<std::string>();
            f.end_reason = row[16].as<std::string>();
            f.cells = parse_cells(row[17].as<std::string>());
            out.push_back(std::move(f));
        }
        return out;
    });
}

std::optional<TimeRange> PgHistory::collection_streak() {
    const std::scoped_lock lock(mu_);
    return with_tx(impl_, conninfo_, [](pqxx::read_transaction& tx) -> std::optional<TimeRange> {
        // Successful requests with no gap over 15 minutes, ending now.
        const auto r = tx.exec1(
            "WITH ok AS ("
            "  SELECT started_at, lag(started_at) OVER (ORDER BY started_at) AS prev "
            "  FROM fetch_log WHERE http_status = 200 AND started_at > now() - interval '400 days') "
            "SELECT extract(epoch FROM max(started_at) FILTER ("
            "         WHERE prev IS NULL OR started_at - prev > interval '15 minutes'))::float8, "
            "       extract(epoch FROM max(started_at))::float8, "
            "       (max(started_at) > now() - interval '15 minutes') "
            "FROM ok");
        if (r[0].is_null() || r[1].is_null() || !r[2].as<bool>()) return std::nullopt;
        return TimeRange{r[0].as<double>(), r[1].as<double>()};
    });
}

}  // namespace sema
