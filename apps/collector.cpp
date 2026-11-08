// Semâ collector: polls OpenSky for the Turkish airspace, drops military and
// government aircraft, keeps aircraft inside Turkey's land + territorial sea and
// stores them in PostgreSQL/PostGIS.
//
// Configuration (environment):
//   OPENSKY_CLIENT_ID, OPENSKY_CLIENT_SECRET  OpenSky API client credentials (required)
//   SEMA_DATABASE_URL       libpq connection string (required unless --dry-run)
//   SEMA_DATA_DIR           directory with turkey_boundary.geojson, military_rules.json (default ./data)
//   SEMA_AIRCRAFT_DB        optional path to the OpenSky aircraft database CSV
//   SEMA_MIN_INTERVAL_S     minimum seconds between requests (default 60)
//   SEMA_DAILY_CREDITS      daily OpenSky credit quota (default 4000)
//   SEMA_RETENTION_HOURS    raw positions older than this are summarised and deleted (default 72)
//   SEMA_RECORD_FILE        same as --record
//   SEMA_LIVE_PORT          same as --serve
//   SEMA_LIVE_ADDR          address the live map server listens on (default 127.0.0.1)
//   SEMA_WEB_ROOT           built web map to serve (default ./web/dist)
// Flags:
//   --once           run a single cycle and exit
//   --dry-run        do not touch the database; print a summary instead
//   --record FILE    append every filtered snapshot (civil aircraft inside the
//                    area only) to FILE as JSON Lines, for sema_replay
//   --serve PORT     serve the live map (HTTP + WebSocket on /ws) on PORT
//
// Every cycle also runs the tracker (Kalman filters, track lifecycle, data
// quality checks) and logs its counters; full tracker metrics are logged once
// an hour as a "tracker_metrics" event.

#include <pthread.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <csignal>
#include <cstdlib>
#include <exception>
#include <filesystem>
#include <iostream>
#include <memory>
#include <nlohmann/json.hpp>
#include <optional>
#include <string>
#include <string_view>
#include <thread>

#include "sema/aircraft_registry.hpp"
#include "sema/clock.hpp"
#include "sema/geo.hpp"
#include "sema/http.hpp"
#include "sema/live.hpp"
#include "sema/live_server.hpp"
#include "sema/military_filter.hpp"
#include "sema/opensky_client.hpp"
#include "sema/pg_store.hpp"
#include "sema/pipeline.hpp"
#include "sema/snapshot_io.hpp"
#include "sema/tracker.hpp"

namespace {

using namespace std::chrono_literals;
using Clock = std::chrono::system_clock;

struct Options {
    std::string client_id;
    std::string client_secret;
    std::string database_url;
    std::string data_dir = "data";
    std::string aircraft_db;
    long min_interval_s = 60;
    long daily_credits = 4000;
    long retention_hours = 72;
    std::string record_file;
    long serve_port = -1;  // -1: no live map server
    std::string serve_addr = "127.0.0.1";
    std::string web_root = "web/dist";
    bool once = false;
    bool dry_run = false;
};

std::string env_or(const char* name, std::string fallback) {
    const char* v = std::getenv(name);
    return (v != nullptr && *v != '\0') ? std::string(v) : std::move(fallback);
}

long env_long(const char* name, long fallback, long min_value) {
    const char* v = std::getenv(name);
    if (v == nullptr || *v == '\0') return fallback;
    char* end = nullptr;
    const long n = std::strtol(v, &end, 10);
    if (end == v || *end != '\0' || n < min_value) {
        throw std::runtime_error(std::string("invalid value for ") + name);
    }
    return n;
}

long parse_port(const char* v) {
    char* end = nullptr;
    const long n = std::strtol(v, &end, 10);
    if (end == v || *end != '\0' || n < 0 || n > 65535) throw std::runtime_error(std::string("invalid port: ") + v);
    return n;
}

Options parse_options(int argc, char** argv) {
    Options o;
    for (int i = 1; i < argc; ++i) {
        const std::string_view a = argv[i];
        if (a == "--once") {
            o.once = true;
        } else if (a == "--dry-run") {
            o.dry_run = true;
        } else if (a == "--record") {
            if (i + 1 >= argc) throw std::runtime_error("--record needs a file name");
            o.record_file = argv[++i];
        } else if (a == "--serve") {
            if (i + 1 >= argc) throw std::runtime_error("--serve needs a port");
            o.serve_port = parse_port(argv[++i]);
        } else if (a == "-h" || a == "--help") {
            std::cout << "usage: sema_collector [--once] [--dry-run] [--record FILE] [--serve PORT]\n"
                         "See the comment at the top of apps/collector.cpp for environment variables.\n";
            std::exit(0);
        } else {
            throw std::runtime_error("unknown argument: " + std::string(a));
        }
    }
    o.client_id = env_or("OPENSKY_CLIENT_ID", "");
    o.client_secret = env_or("OPENSKY_CLIENT_SECRET", "");
    o.database_url = env_or("SEMA_DATABASE_URL", "");
    o.data_dir = env_or("SEMA_DATA_DIR", o.data_dir);
    o.aircraft_db = env_or("SEMA_AIRCRAFT_DB", "");
    if (o.record_file.empty()) o.record_file = env_or("SEMA_RECORD_FILE", "");
    if (const char* port = std::getenv("SEMA_LIVE_PORT"); o.serve_port < 0 && port != nullptr && *port != '\0') {
        o.serve_port = parse_port(port);
    }
    o.serve_addr = env_or("SEMA_LIVE_ADDR", o.serve_addr);
    o.web_root = env_or("SEMA_WEB_ROOT", o.web_root);
    o.min_interval_s = env_long("SEMA_MIN_INTERVAL_S", o.min_interval_s, 5);
    o.daily_credits = env_long("SEMA_DAILY_CREDITS", o.daily_credits, 1);
    o.retention_hours = env_long("SEMA_RETENTION_HOURS", o.retention_hours, 1);
    if (o.client_id.empty() || o.client_secret.empty()) {
        throw std::runtime_error("OPENSKY_CLIENT_ID and OPENSKY_CLIENT_SECRET must be set");
    }
    if (!o.dry_run && o.database_url.empty()) {
        throw std::runtime_error("SEMA_DATABASE_URL must be set (or use --dry-run)");
    }
    return o;
}

std::chrono::seconds until_next_utc_midnight(Clock::time_point now) {
    const auto day = std::chrono::floor<std::chrono::days>(now);
    return std::chrono::ceil<std::chrono::seconds>(day + std::chrono::days(1) - now);
}

// Spacing that spreads the remaining credits evenly over the rest of the UTC day.
std::chrono::seconds next_interval(const Options& o, int cost, std::optional<int> remaining, Clock::time_point now) {
    long s = std::max(o.min_interval_s, static_cast<long>(std::ceil(86400.0 * cost / static_cast<double>(o.daily_credits))));
    if (remaining && *remaining > 0) {
        const double left = static_cast<double>(until_next_utc_midnight(now).count());
        s = std::max(s, static_cast<long>(std::ceil(left * cost / *remaining)));
    } else if (remaining && *remaining <= 0) {
        s = std::max(s, static_cast<long>(until_next_utc_midnight(now).count()));
    }
    return std::chrono::seconds(s);
}

void log_line(nlohmann::json j) {
    j["ts"] = std::chrono::duration_cast<std::chrono::seconds>(Clock::now().time_since_epoch()).count();
    std::cout << j.dump() << '\n' << std::flush;
}

// Blocks SIGINT/SIGTERM in all threads and handles them in a dedicated
// sigwait thread, so no work happens inside an asynchronous signal handler.
class SignalWatcher {
public:
    explicit SignalWatcher(sema::SystemClock& clock) {
        sigemptyset(&set_);
        sigaddset(&set_, SIGINT);
        sigaddset(&set_, SIGTERM);
        pthread_sigmask(SIG_BLOCK, &set_, nullptr);
        thread_ = std::thread([this, &clock] {
            int sig = 0;
            sigwait(&set_, &sig);
            if (!shutting_down_.load()) {
                log_line({{"event", "signal"}, {"signal", sig}});
            }
            clock.request_stop();
        });
    }
    ~SignalWatcher() {
        shutting_down_.store(true);
        pthread_kill(thread_.native_handle(), SIGTERM);  // NOLINT(bugprone-bad-signal-to-kill-thread): SIGTERM is blocked -> consumed by sigwait
        thread_.join();
    }
    SignalWatcher(const SignalWatcher&) = delete;
    SignalWatcher& operator=(const SignalWatcher&) = delete;

private:
    sigset_t set_{};
    std::atomic<bool> shutting_down_{false};
    std::thread thread_;
};

int run(const Options& opt) {
    const std::filesystem::path data_dir(opt.data_dir);
    const auto area = sema::MultiPolygon::load_geojson(data_dir / "turkey_boundary.geojson");
    const auto box = sema::query_box_for(area);
    const int cost = sema::opensky_credit_cost(box);

    sema::AircraftRegistry registry;
    if (!opt.aircraft_db.empty()) registry = sema::AircraftRegistry::load_csv(opt.aircraft_db);
    const sema::MilitaryFilter military(sema::MilitaryRules::load(data_dir / "military_rules.json"), &registry);

    std::unique_ptr<sema::PgStore> store;
    if (!opt.dry_run) {
        store = std::make_unique<sema::PgStore>(opt.database_url);
        store->migrate();
        store->upsert_boundary("turkey", area.to_geojson_geometry());
    }

    std::unique_ptr<sema::SnapshotWriter> recorder;
    if (!opt.record_file.empty()) recorder = std::make_unique<sema::SnapshotWriter>(opt.record_file);
    sema::Tracker tracker({}, [&area](sema::LatLon p) { return area.contains(p); });
    auto last_metrics_log = Clock::now();

    sema::SystemClock clock;
    SignalWatcher signals(clock);  // before any other thread starts, so they inherit the blocked signals

    // Live map: only filtered (civil, in-area) snapshots ever reach it.
    sema::LiveState live(area);
    std::unique_ptr<sema::LiveServer> server;
    if (opt.serve_port >= 0) {
        sema::LiveServerConfig sc;
        sc.address = opt.serve_addr;
        sc.port = static_cast<std::uint16_t>(opt.serve_port);
        sc.web_root = opt.web_root;
        sc.files["/boundary.geojson"] = data_dir / "turkey_boundary.geojson";
        server = std::make_unique<sema::LiveServer>(sc);
        server->start();
        server->publish("hello", sema::live_hello_json("live", "").dump());
        server->publish("coverage", live.coverage_json().dump());
        log_line({{"event", "live_server"}, {"address", opt.serve_addr}, {"port", server->port()},
                  {"web_root", opt.web_root}});
    }
    sema::CurlTransport http;
    sema::OpenSkyConfig cfg;
    cfg.client_id = opt.client_id;
    cfg.client_secret = opt.client_secret;
    sema::OpenSkyClient client(cfg, http, clock);

    log_line({{"event", "start"},
              {"box", {box.lat_min, box.lon_min, box.lat_max, box.lon_max}},
              {"box_area_sq_deg", box.area_sq_deg()},
              {"credit_cost", cost},
              {"registry_size", registry.size()},
              {"recording", !opt.record_file.empty()},
              {"dry_run", opt.dry_run}});

    auto last_rollup = Clock::now() - 2h;
    int exit_code = 0;

    while (!clock.stop_requested()) {
        const auto started = Clock::now();
        const auto r = client.fetch_states(box);

        sema::FetchLogEntry entry;
        entry.started_at = started;
        entry.latency = r.latency;
        if (r.http_status != 0) entry.http_status = r.http_status;
        entry.attempts = r.attempts;
        entry.credits_cost = r.credits_cost;
        entry.credits_remaining = r.credits_remaining;
        entry.error = r.error;

        nlohmann::json line{{"event", "cycle"},
                            {"status", sema::to_string(r.status)},
                            {"http_status", r.http_status},
                            {"attempts", r.attempts},
                            {"latency_ms", r.latency.count()}};
        if (r.credits_remaining) line["credits_remaining"] = *r.credits_remaining;

        if (r.status == sema::FetchResult::Status::Ok && r.snapshot) {
            entry.data_time = r.snapshot->time;
            sema::FilterStats stats;
            auto kept = sema::filter_states(r.snapshot->states, area, military, stats);
            entry.states_total = stats.total;
            entry.states_kept = stats.kept;
            entry.dropped_outside = stats.dropped_outside + stats.dropped_no_position;
            entry.dropped_military = stats.dropped_military;
            std::size_t written = 0;
            if (store) written = store->insert_positions(started, kept);
            const sema::StatesSnapshot filtered{r.snapshot->time, std::move(kept)};
            if (recorder) {
                try {
                    recorder->write(filtered);
                } catch (const std::exception& e) {
                    line["record_error"] = e.what();
                }
            }
            const auto tr = tracker.process(filtered);
            if (server) {
                live.update(filtered, tracker);
                const double now_s =
                    std::chrono::duration<double>(Clock::now().time_since_epoch()).count();
                server->publish("coverage", live.coverage_json().dump());
                server->publish("snapshot", live.snapshot_json({now_s, 1.0}).dump());
                line["live_clients"] = server->client_count();
            }
            line["tracks"] = {{"tracked", tr.tracked},     {"confirmed", tr.confirmed}, {"coasting", tr.coasting},
                              {"created", tr.created},     {"ended", tr.ended.size()},  {"reacquired", tr.reacquired},
                              {"updates", tr.updates},     {"stale", tr.stale},         {"rejected", tr.rejected},
                              {"processing_us", static_cast<long>(tr.processing_us)}};
            line.update({{"data_time", r.snapshot->time},
                         {"total", stats.total},
                         {"kept", stats.kept},
                         {"dropped_outside", stats.dropped_outside},
                         {"dropped_no_position", stats.dropped_no_position},
                         {"dropped_military", stats.dropped_military},
                         {"skipped_rows", r.skipped_rows},
                         {"written", written}});
        } else if (!r.error.empty()) {
            line["error"] = r.error;
        }

        if (store) {
            try {
                store->log_fetch(entry);
                if (started - last_rollup >= 1h) {
                    const auto pruned = store->rollup_and_prune(std::chrono::hours(opt.retention_hours));
                    line["pruned_rows"] = pruned;
                    last_rollup = started;
                }
            } catch (const std::exception& e) {
                line["db_error"] = e.what();
            }
        }

        std::chrono::seconds wait = next_interval(opt, cost, r.credits_remaining, Clock::now());
        if (r.status == sema::FetchResult::Status::RateLimited) {
            wait = r.retry_after.value_or(until_next_utc_midnight(Clock::now()));
        } else if (r.status == sema::FetchResult::Status::AuthFailed) {
            line["fatal"] = "OpenSky rejected the client credentials";
            log_line(line);
            exit_code = 2;
            break;
        }
        line["next_in_s"] = wait.count();
        log_line(line);
        if (Clock::now() - last_metrics_log >= 1h) {
            log_line({{"event", "tracker_metrics"}, {"metrics", tracker.metrics().to_json()}});
            last_metrics_log = Clock::now();
        }

        if (opt.once) {
            if (r.status != sema::FetchResult::Status::Ok) exit_code = 1;
            break;
        }
        // Account for time spent on this cycle so requests are evenly spaced.
        const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - started);
        const auto sleep = std::max(std::chrono::milliseconds(0), std::chrono::milliseconds(wait) - elapsed);
        clock.sleep_for(sleep);
    }

    log_line({{"event", "tracker_metrics"}, {"metrics", tracker.metrics().to_json()}});
    log_line({{"event", "stop"}, {"exit_code", exit_code}});
    return exit_code;
}

}  // namespace

int main(int argc, char** argv) {
    try {
        return run(parse_options(argc, argv));
    } catch (const std::exception& e) {
        std::cerr << "sema_collector: " << e.what() << '\n';
        return 1;
    }
}
