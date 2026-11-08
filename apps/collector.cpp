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
//   SEMA_EVENTS_FILE        same as --events
//   SEMA_MAX_REPLAY_HOURS   longest range the map may ask to replay (default 4)
//   SEMA_IDENTIFIERS        what the public map/API shows: hidden (default), airline or full
//                           (see include/sema/privacy.hpp and README, Etik ve hukuki sınırlar)
//   SEMA_OPTOUT_FILE        aircraft never stored or shown (default DATA_DIR/optout.txt)
//   SEMA_IDENTITY_DAYS      flights, hourly tracks and events keep ICAO24/callsign this many
//                           days, then are anonymised (default 30)
//   SEMA_OPERATOR_NAME, SEMA_CONTACT_EMAIL  who runs the site; shown on the privacy page
//                           (/gizlilik.html) for removal requests and as the legal notice
// Flags:
//   --once           run a single cycle and exit
//   --dry-run        do not touch the database; print a summary instead
//   --record FILE    append every filtered snapshot (civil aircraft inside the
//                    area only) to FILE as JSON Lines, for sema_replay
//   --serve PORT     serve the live map (HTTP + WebSocket on /ws) on PORT
//   --events FILE    append anomaly events to FILE as JSON Lines
//
// Every cycle also runs the tracker (Kalman filters, track lifecycle, data
// quality checks) and the anomaly detector (emergency squawks, rapid descent,
// holding, route deviation, signal loss) and logs their counters; every new
// event is logged as an "anomaly" line and stored in anomaly_events. Full
// tracker and detector metrics are logged once an hour.
// Finished tracks become flight records (with departure/arrival airport when
// seen) in the `flights` table.
//
// With --serve the same server also answers /api/status, /api/replay and
// /api/stats (history from PostgreSQL, or from memory with --dry-run) and
// /metrics in Prometheus format.

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
#include "sema/anomaly.hpp"
#include "sema/clock.hpp"
#include "sema/flights.hpp"
#include "sema/geo.hpp"
#include "sema/history.hpp"
#include "sema/http.hpp"
#include "sema/live.hpp"
#include "sema/live_server.hpp"
#include "sema/military_filter.hpp"
#include "sema/opensky_client.hpp"
#include "sema/pg_history.hpp"
#include "sema/pg_store.hpp"
#include "sema/pipeline.hpp"
#include "sema/privacy.hpp"
#include "sema/prom_metrics.hpp"
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
    std::string events_file;
    long serve_port = -1;  // -1: no live map server
    std::string serve_addr = "127.0.0.1";
    std::string web_root = "web/dist";
    long max_replay_hours = 4;
    std::string identifiers = "hidden";
    std::string opt_out_file;
    long identity_days = 30;
    std::string operator_name;
    std::string contact_email;
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
        } else if (a == "--events") {
            if (i + 1 >= argc) throw std::runtime_error("--events needs a file name");
            o.events_file = argv[++i];
        } else if (a == "--serve") {
            if (i + 1 >= argc) throw std::runtime_error("--serve needs a port");
            o.serve_port = parse_port(argv[++i]);
        } else if (a == "-h" || a == "--help") {
            std::cout << "usage: sema_collector [--once] [--dry-run] [--record FILE] [--events FILE] [--serve PORT]\n"
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
    if (o.events_file.empty()) o.events_file = env_or("SEMA_EVENTS_FILE", "");
    if (const char* port = std::getenv("SEMA_LIVE_PORT"); o.serve_port < 0 && port != nullptr && *port != '\0') {
        o.serve_port = parse_port(port);
    }
    o.serve_addr = env_or("SEMA_LIVE_ADDR", o.serve_addr);
    o.web_root = env_or("SEMA_WEB_ROOT", o.web_root);
    o.min_interval_s = env_long("SEMA_MIN_INTERVAL_S", o.min_interval_s, 5);
    o.daily_credits = env_long("SEMA_DAILY_CREDITS", o.daily_credits, 1);
    o.retention_hours = env_long("SEMA_RETENTION_HOURS", o.retention_hours, 1);
    o.max_replay_hours = env_long("SEMA_MAX_REPLAY_HOURS", o.max_replay_hours, 1);
    o.identifiers = env_or("SEMA_IDENTIFIERS", o.identifiers);
    (void)sema::parse_identifier_mode(o.identifiers);  // fail early on a typo
    o.opt_out_file = env_or("SEMA_OPTOUT_FILE", "");
    o.identity_days = env_long("SEMA_IDENTITY_DAYS", o.identity_days, 1);
    o.operator_name = env_or("SEMA_OPERATOR_NAME", "");
    o.contact_email = env_or("SEMA_CONTACT_EMAIL", "");
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

double unix_now() { return std::chrono::duration<double>(Clock::now().time_since_epoch()).count(); }

std::shared_ptr<const sema::AirportIndex> load_airports(const std::filesystem::path& path) {
    if (!std::filesystem::exists(path)) return std::make_shared<const sema::AirportIndex>();
    return std::make_shared<const sema::AirportIndex>(sema::AirportIndex::load(path));
}

// Prometheus metrics updated once per cycle.
void record_cycle_metrics(sema::PromRegistry& prom, const sema::FetchResult& r) {
    prom.add_counter("sema_opensky_requests_total", "OpenSky /states/all requests by result", 1.0,
                     {{"status", sema::to_string(r.status)}});
    prom.add_counter("sema_opensky_request_seconds_total", "Total time spent in OpenSky requests (with retries)",
                     std::chrono::duration<double>(r.latency).count());
    prom.add_counter("sema_opensky_attempts_total", "HTTP attempts made, including retries", r.attempts);
    if (r.status == sema::FetchResult::Status::Ok) {
        prom.add_counter("sema_opensky_credits_used_total", "OpenSky credits spent", r.credits_cost);
    }
    if (r.credits_remaining) {
        prom.set_gauge("sema_opensky_credits_remaining", "OpenSky credits left today (X-Rate-Limit-Remaining)",
                       *r.credits_remaining);
    }
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
    const auto opt_out =
        sema::OptOutList::load(opt.opt_out_file.empty() ? data_dir / "optout.txt" : std::filesystem::path(opt.opt_out_file));
    // Everything published (WebSocket feed, /api/replay) goes through the masker.
    const sema::IdentityMasker masker(sema::parse_identifier_mode(opt.identifiers));
    auto public_json = [&masker](nlohmann::json j) {
        masker.apply(j);
        return j.dump();
    };

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
    // Anomaly detection (phase 4) on the same filtered snapshots.
    sema::AnomalyDetector detector({}, std::make_shared<const sema::MultiPolygon>(area));
    std::unique_ptr<sema::EventLog> event_log;
    if (!opt.events_file.empty()) event_log = std::make_unique<sema::EventLog>(opt.events_file);

    // Flight log and history (replay + statistics) for the web map.
    const auto airports = load_airports(data_dir / "airports_tr.json");
    sema::FlightLog flight_log(airports.get());
    std::shared_ptr<sema::HistorySource> history;
    if (store) {
        history = std::make_shared<sema::PgHistory>(opt.database_url);
    } else {
        history = std::make_shared<sema::MemoryHistory>();
    }
    sema::PromRegistry prom;
    prom.set_gauge("sema_build_info", "Build information", 1.0, {{"version", "0.5.0"}});
    prom.set_gauge("sema_start_time_seconds", "Collector start time (Unix seconds)", unix_now());
    prom.set_gauge("sema_dry_run", "1 when running without a database", opt.dry_run ? 1.0 : 0.0);

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
        sema::HistoryApiConfig hc;
        hc.mode = "live";
        hc.label = opt.dry_run ? "veritabanısız deneme" : "";
        hc.max_replay_s = static_cast<double>(opt.max_replay_hours) * 3600.0;
        hc.mask_identifiers = [&masker](nlohmann::json& j) { masker.apply(j); };
        hc.site = {{"operator", opt.operator_name},
                   {"contact", opt.contact_email},
                   {"identifiers", opt.identifiers},
                   {"identity_days", opt.identity_days},
                   {"retention_hours", opt.retention_hours}};
        auto api = std::make_shared<sema::HistoryApi>(history, airports, hc);
        sc.api = [api, history, &prom](const sema::ApiRequest& req) -> std::optional<sema::ApiResponse> {
            if (req.path == "/metrics") {
                prom.set_gauge("sema_process_resident_memory_bytes", "Resident memory of the collector",
                               static_cast<double>(sema::process_rss_bytes()));
                try {
                    const auto streak = history->collection_streak();
                    prom.set_gauge("sema_collection_streak_seconds",
                                   "Length of the current unbroken stretch of data collection (no gap over 15 min)",
                                   streak ? streak->to - streak->from : 0.0);
                } catch (const std::exception&) {
                    // Database down: keep the last value, count the error.
                    prom.add_counter("sema_db_errors_total", "Database write errors", 1.0);
                }
                return sema::ApiResponse{200, "text/plain; version=0.0.4; charset=utf-8", prom.render(), 0};
            }
            return api->handle(req);
        };
        server = std::make_unique<sema::LiveServer>(sc);
        server->start();
        server->publish("hello", sema::live_hello_json("live", "", opt.identifiers).dump());
        server->publish("coverage", live.coverage_json().dump());
        server->publish("events", public_json(detector.events_json()));
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
              {"opt_out_size", opt_out.size()},
              {"identifiers", opt.identifiers},
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
            auto kept = sema::filter_states(r.snapshot->states, area, military, stats, &opt_out);
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
            const auto ev = detector.process(filtered, tracker, tr);
            for (const auto& e : ev.opened) {
                log_line({{"event", "anomaly"}, {"kind", sema::to_string(e.kind)}, {"icao24", e.icao24},
                          {"callsign", e.callsign}, {"summary", e.summary}, {"details", e.details}});
            }
            if (!ev.empty()) {
                if (event_log) {
                    try {
                        event_log->write(ev);
                    } catch (const std::exception& e) {
                        line["events_error"] = e.what();
                    }
                }
                if (store) {
                    std::vector<sema::AnomalyEvent> changed = ev.opened;
                    changed.insert(changed.end(), ev.updated.begin(), ev.updated.end());
                    changed.insert(changed.end(), ev.closed.begin(), ev.closed.end());
                    try {
                        store->upsert_events(changed);
                    } catch (const std::exception& e) {
                        line["db_error"] = e.what();
                    }
                }
                if (server) server->publish("events", public_json(detector.events_json()));
            }
            line["anomalies"] = {{"opened", ev.opened.size()}, {"closed", ev.closed.size()},
                                 {"active", detector.active().size()}};
            const auto ended_flights = flight_log.update(filtered, tr, tracker);
            history->record(filtered, ended_flights);
            if (server) history->set_open_flights(flight_log.open_flights());
            if (store && !ended_flights.empty()) {
                try {
                    store->insert_flights(ended_flights);
                } catch (const std::exception& e) {
                    line["db_error"] = e.what();
                    prom.add_counter("sema_db_errors_total", "Database write errors", 1.0);
                }
            }
            prom.add_counter("sema_flights_completed_total", "Flights whose track ended", ended_flights.size());
            prom.set_gauge("sema_flights_open", "Flights in progress", flight_log.open_count());
            prom.set_gauge("sema_last_data_time_seconds", "OpenSky time of the newest snapshot", r.snapshot->time);
            prom.set_gauge("sema_states_in_box", "State vectors in the query box (before filtering)", stats.total);
            prom.set_gauge("sema_aircraft_kept", "Civil aircraft inside the area after filtering", stats.kept);
            prom.add_counter("sema_dropped_military_total", "Military/government aircraft dropped (never stored)",
                             stats.dropped_military);
            prom.set_gauge("sema_tracks", "Tracks by state", tr.tracked, {{"state", "tracked"}});
            prom.set_gauge("sema_tracks", "Tracks by state", tr.confirmed, {{"state", "confirmed"}});
            prom.set_gauge("sema_tracks", "Tracks by state", tr.coasting, {{"state", "coasting"}});
            prom.set_gauge("sema_tracker_processing_seconds", "Tracker time for the last snapshot", tr.processing_us * 1e-6);
            prom.add_counter("sema_quality_rejects_total", "Position reports rejected by data quality checks", tr.rejected);
            if (server) {
                live.update(filtered, tracker);
                const double now_s =
                    std::chrono::duration<double>(Clock::now().time_since_epoch()).count();
                server->publish("coverage", live.coverage_json().dump());
                server->publish("snapshot", public_json(live.snapshot_json({now_s, 1.0})));
                line["live_clients"] = server->client_count();
                prom.set_gauge("sema_live_clients", "Connected WebSocket clients", server->client_count());
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
                         {"dropped_opt_out", stats.dropped_opt_out},
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
                    prom.add_counter("sema_pruned_rows_total", "Raw position rows rolled up and deleted", pruned);
                    const auto anonymized = store->anonymize_identities(std::chrono::days(opt.identity_days));
                    line["anonymized_rows"] = anonymized;
                    prom.add_counter("sema_anonymized_rows_total",
                                     "Rows whose aircraft identifiers were removed after SEMA_IDENTITY_DAYS", anonymized);
                    last_rollup = started;
                }
            } catch (const std::exception& e) {
                line["db_error"] = e.what();
                prom.add_counter("sema_db_errors_total", "Database write errors", 1.0);
            }
        }
        record_cycle_metrics(prom, r);

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
        prom.set_gauge("sema_next_request_in_seconds", "Planned wait before the next OpenSky request", wait.count());
        log_line(line);
        if (Clock::now() - last_metrics_log >= 1h) {
            log_line({{"event", "tracker_metrics"}, {"metrics", tracker.metrics().to_json()}});
            log_line({{"event", "anomaly_metrics"}, {"metrics", detector.metrics().to_json()}});
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
    log_line({{"event", "anomaly_metrics"}, {"metrics", detector.metrics().to_json()}});
    // Flights still in the air are stored as "open" so a restart loses nothing.
    if (store) {
        try {
            store->insert_flights(flight_log.flush());
        } catch (const std::exception& e) {
            log_line({{"event", "db_error"}, {"error", e.what()}});
        }
    }
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
