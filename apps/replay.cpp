// Semâ replay: runs the tracker over a recorded snapshot file (JSON Lines,
// one OpenSky /states/all document per line, as written by
// `sema_collector --record`) and prints tracking metrics.
//
// usage: sema_replay <recording.jsonl> [--data-dir DIR] [--no-area]
//                    [--accel-psd Q] [--ct-accel-psd Q] [--turn-psd Q] [--json] [--snapshots]
//   --data-dir DIR  directory with turkey_boundary.geojson (default ./data)
//   --no-area       do not load the boundary (no "left area" end reason)
//   --accel-psd Q   constant-velocity KF process noise, m^2/s^3
//   --turn-psd Q    turn EKF turn-rate process noise, rad^2/s^3
//   --ct-accel-psd Q turn EKF along-track acceleration noise, m^2/s^3
//   --json          print the full metrics as JSON instead of a table
//   --snapshots     also print one JSON line per snapshot
//   --serve PORT    also serve the live map on PORT, playing the recording in
//                   (scaled) real time; after the last snapshot the map is
//                   frozen until Ctrl-C, then the metrics are printed
//   --speed X       playback speed for --serve (default 10)
//   --label TEXT    shown in the map's replay banner (e.g. "sentetik veri")
//   --addr ADDR     listen address for --serve (default 127.0.0.1)
//   --web-root DIR  built web map (default ./web/dist)
//   --identifiers M what the map shows about each aircraft: hidden, airline or
//                   full (default: SEMA_IDENTIFIERS, else hidden; see privacy.hpp)
//   --events        also list every anomaly event and the detector metrics
//                   (event counts and rates, for tuning thresholds)
//   --loss-min-coverage N  signal-loss rule: reports needed in the last
//                   fix's coverage cell (default 200 over 24 h; short
//                   recordings need a lower value)
//   --anomaly NAME=V set any detector threshold (repeatable), e.g.
//                   --anomaly descent_min_rate_ms=18; --anomaly list prints the names
//   --stats         print traffic statistics (flights, busiest hours, airports,
//                   routes, corridors) for the whole recording as JSON
//   --report FILE   write a Markdown measurement report (recording, tracking,
//                   anomaly and traffic tables in the README's format) to FILE
//   --events-csv FILE  write every anomaly event to FILE with an empty
//                   "etiket" column to label by hand (gerçek / yanlış)
//   --labels FILE   a labelled events CSV; the report then shows the false-alarm
//                   rate per kind, and --events-csv keeps the labels
//
// With --serve the whole recording is also loaded into memory first, so the
// map's "Geçmiş" (replay any range) and "İstatistik" views work at once.

#include <pthread.h>
#include <signal.h>

#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <exception>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iostream>
#include <memory>
#include <optional>
#include <vector>
#include <algorithm>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <nlohmann/json.hpp>

#include "sema/anomaly.hpp"
#include "sema/flights.hpp"
#include "sema/geo.hpp"
#include "sema/history.hpp"
#include "sema/live.hpp"
#include "sema/live_server.hpp"
#include "sema/privacy.hpp"
#include "sema/report.hpp"
#include "sema/snapshot_io.hpp"
#include "sema/tracker.hpp"
#include "sema/traffic_stats.hpp"

namespace {

struct Options {
    std::string input;
    std::string data_dir = "data";
    bool use_area = true;
    bool json = false;
    bool snapshots = false;
    bool events = false;
    std::optional<long> loss_min_coverage;
    std::vector<std::pair<std::string, double>> anomaly_params;
    bool stats = false;
    std::string report;
    std::string events_csv;
    std::string labels;
    std::optional<double> accel_psd;
    std::optional<double> turn_psd;
    std::optional<double> ct_accel_psd;
    std::optional<long> serve_port;
    double speed = 10.0;
    std::string label;
    std::string addr = "127.0.0.1";
    std::string web_root = "web/dist";
    std::string identifiers;
};

[[noreturn]] void usage(int code) {
    std::cerr << "usage: sema_replay <recording.jsonl> [--data-dir DIR] [--no-area] [--accel-psd Q] [--ct-accel-psd Q] [--turn-psd Q] [--json] "
                 "[--snapshots] [--stats] [--report FILE] [--events-csv FILE] [--labels FILE] [--events [--loss-min-coverage N] [--anomaly NAME=V]...] [--serve PORT [--speed X] [--label TEXT] [--addr ADDR] [--identifiers MODE] "
                 "[--web-root DIR]]\n";
    std::exit(code);
}

Options parse(int argc, char** argv) {
    Options o;
    for (int i = 1; i < argc; ++i) {
        const std::string_view a = argv[i];
        auto value = [&]() -> std::string {
            if (i + 1 >= argc) usage(2);
            return argv[++i];
        };
        if (a == "--data-dir") {
            o.data_dir = value();
        } else if (a == "--no-area") {
            o.use_area = false;
        } else if (a == "--accel-psd") {
            o.accel_psd = std::stod(value());
        } else if (a == "--ct-accel-psd") {
            o.ct_accel_psd = std::stod(value());
        } else if (a == "--turn-psd") {
            o.turn_psd = std::stod(value());
        } else if (a == "--serve") {
            o.serve_port = std::stol(value());
            if (*o.serve_port < 0 || *o.serve_port > 65535) usage(2);
        } else if (a == "--speed") {
            o.speed = std::stod(value());
            if (!(o.speed > 0.0)) usage(2);
        } else if (a == "--label") {
            o.label = value();
        } else if (a == "--addr") {
            o.addr = value();
        } else if (a == "--web-root") {
            o.web_root = value();
        } else if (a == "--identifiers") {
            o.identifiers = value();
            (void)sema::parse_identifier_mode(o.identifiers);
        } else if (a == "--json") {
            o.json = true;
        } else if (a == "--events") {
            o.events = true;
        } else if (a == "--loss-min-coverage") {
            o.loss_min_coverage = std::stol(value());
            if (*o.loss_min_coverage < 0) usage(2);
        } else if (a == "--anomaly") {
            const std::string kv = value();
            if (kv == "list") {
                for (const auto& n : sema::anomaly_param_names()) std::cout << n << '\n';
                std::exit(0);
            }
            const auto eq = kv.find('=');
            if (eq == std::string::npos) usage(2);
            o.anomaly_params.emplace_back(kv.substr(0, eq), std::stod(kv.substr(eq + 1)));
            o.events = true;
        } else if (a == "--snapshots") {
            o.snapshots = true;
        } else if (a == "--stats") {
            o.stats = true;
        } else if (a == "--report") {
            o.report = value();
        } else if (a == "--events-csv") {
            o.events_csv = value();
        } else if (a == "--labels") {
            o.labels = value();
        } else if (a == "-h" || a == "--help") {
            usage(0);
        } else if (!a.empty() && a[0] != '-' && o.input.empty()) {
            o.input = a;
        } else {
            usage(2);
        }
    }
    if (o.input.empty()) usage(2);
    return o;
}

std::string fmt(double x, int decimals = 0) {
    char buf[64];
    (void)std::snprintf(buf, sizeof buf, "%.*f", decimals, x);
    return buf;
}

void print_table(const sema::TrackerMetrics& m) {
    auto row = [](const std::string& name, const sema::Distribution& d, int dec = 0) {
        std::cout << "  " << name << std::string(name.size() < 28 ? 28 - name.size() : 1, ' ')
                  << "n=" << d.count() << "  p50=" << fmt(d.percentile(50), dec) << "  p90=" << fmt(d.percentile(90), dec)
                  << "  p95=" << fmt(d.percentile(95), dec) << "  max=" << fmt(d.max(), dec) << '\n';
    };
    std::cout << "snapshots " << m.snapshots << ", measurements " << m.measurements << ", updates " << m.updates
              << ", stale " << m.stale << "\n\n";
    std::cout << "prediction error before each new fix (metres)\n";
    row("coordinated-turn EKF", m.ct_error_m);
    row("constant-velocity KF", m.cv_error_m);
    row("dead reckoning (baseline)", m.dead_reckoning_error_m);
    row("hold last pos. (baseline)", m.hold_error_m);
    for (std::size_t i = 0; i < sema::kHorizonBuckets; ++i) {
        if (m.ct_error_by_horizon_m[i].count() == 0) continue;
        row("ct ekf @ " + sema::horizon_label(i), m.ct_error_by_horizon_m[i]);
        row("cv kf @ " + sema::horizon_label(i), m.cv_error_by_horizon_m[i]);
        row("dead reck. @ " + sema::horizon_label(i), m.dead_reckoning_error_by_horizon_m[i]);
    }
    row("altitude (vertical KF)", m.altitude_error_m);
    std::cout << "\ntracks\n";
    std::cout << "  created " << m.tracks_created << ", confirmed " << m.tracks_confirmed << ", signal losses "
              << m.signal_losses() << ", reacquired " << m.reacquisitions << ", breaks " << m.track_breaks
              << " (break rate " << fmt(m.break_rate() * 100.0, 2) << "%)\n";
    std::cout << "  ended: signal_lost " << m.ended_by_reason[0] << ", left_area " << m.ended_by_reason[1]
              << ", landed " << m.ended_by_reason[2] << ", restarted " << m.ended_by_reason[3] << '\n';
    row("tracked per snapshot", m.tracked_per_snapshot);
    row("fix interval (s)", m.fix_interval_s);
    row("reacquire gap (s)", m.reacquire_gap_s);
    std::cout << "\nquality rejects\n  invalid_position " << m.invalid_position << ", impossible_speed "
              << m.impossible_speed << ", teleport " << m.teleport << ", bad_altitude " << m.bad_altitude
              << ", bad_vertical_rate " << m.bad_vertical_rate << "\n\nprocessing time (microseconds)\n";
    row("per snapshot", m.processing_us_per_snapshot);
    row("per measurement", m.processing_us_per_measurement, 2);
}

std::string utc(double t) {
    const auto tt = static_cast<std::time_t>(t);
    std::tm tm{};
    gmtime_r(&tt, &tm);
    char buf[32];
    (void)std::strftime(buf, sizeof buf, "%Y-%m-%d %H:%M:%S", &tm);
    return buf;
}

void print_events(const sema::AnomalyMetrics& m, const std::vector<sema::AnomalyEvent>& events) {
    const auto j = m.to_json();
    std::cout << "\nanomaly events (" << m.events() << " over " << fmt(m.airborne_aircraft_hours, 1)
              << " airborne aircraft-hours)\n";
    for (std::size_t i = 0; i < sema::kEventKinds; ++i) {
        const auto k = sema::to_string(static_cast<sema::EventKind>(i));
        std::cout << "  " << k << std::string(18 - std::string(k).size(), ' ') << m.events_by_kind[i] << "  ("
                  << fmt(j["events_per_1000_aircraft_hours"][k].get<double>(), 2) << " per 1000 h)\n";
    }
    std::cout << "  detector time per snapshot: p50=" << fmt(m.processing_us_per_snapshot.percentile(50), 0)
              << " us  max=" << fmt(m.processing_us_per_snapshot.max(), 0) << " us\n";
    for (const auto& e : events) {
        std::cout << "  " << utc(e.start_time) << "  " << sema::to_string(e.kind) << "  " << e.icao24 << ' '
                  << (e.callsign.empty() ? "-" : e.callsign) << "  " << fmt((e.end_time - e.start_time) / 60.0, 1)
                  << " min" << (e.active ? " (open)" : "") << "  " << e.summary << '\n';
    }
}

std::shared_ptr<const sema::AirportIndex> load_airports(const std::filesystem::path& path) {
    if (!std::filesystem::exists(path)) return std::make_shared<const sema::AirportIndex>();
    return std::make_shared<const sema::AirportIndex>(sema::AirportIndex::load(path));
}

// Runs a separate tracker + flight log over the whole recording and keeps
// everything in memory, for the map's history and statistics views.
std::shared_ptr<sema::MemoryHistory> preload_history(const Options& o, const sema::TrackerConfig& cfg,
                                                     const std::function<bool(sema::LatLon)>& in_area,
                                                     const sema::AirportIndex& airports) {
    sema::MemoryHistory::Limits limits;
    limits.keep_fixes_s = 400.0 * 86400.0;  // keep the whole recording (max_fixes still caps memory)
    limits.keep_flights_s = 400.0 * 86400.0;
    auto history = std::make_shared<sema::MemoryHistory>(limits);
    sema::Tracker tracker(cfg, in_area);
    sema::FlightLog log(&airports);
    sema::SnapshotReader reader(o.input);
    while (auto snap = reader.next()) {
        const auto r = tracker.process(*snap);
        history->record(*snap, log.update(*snap, r, tracker));
    }
    history->record(sema::StatesSnapshot{0, {}}, log.flush());
    return history;
}

int run(const Options& o) {
    sema::TrackerConfig cfg;
    if (o.accel_psd) cfg.accel_psd = *o.accel_psd;
    if (o.turn_psd) cfg.turn_noise.turn_accel_psd = *o.turn_psd;
    if (o.ct_accel_psd) cfg.turn_noise.accel_psd = *o.ct_accel_psd;

    std::function<bool(sema::LatLon)> in_area;
    std::shared_ptr<sema::MultiPolygon> area;
    if (o.use_area) {
        area = std::make_shared<sema::MultiPolygon>(
            sema::MultiPolygon::load_geojson(std::filesystem::path(o.data_dir) / "turkey_boundary.geojson"));
        in_area = [area](sema::LatLon p) { return area->contains(p); };
    }

    sema::Tracker tracker(cfg, in_area);
    sema::AnomalyConfig acfg;
    if (o.loss_min_coverage) acfg.signal_loss_min_coverage = static_cast<std::uint64_t>(*o.loss_min_coverage);
    for (const auto& [name, v] : o.anomaly_params) {
        if (!sema::set_anomaly_param(acfg, name, v)) throw std::runtime_error("unknown or invalid --anomaly " + name);
    }
    sema::AnomalyDetector detector(acfg, area);
    std::vector<sema::AnomalyEvent> finished;  // closed events, in order
    sema::SnapshotReader reader(o.input);
    const auto airports = load_airports(std::filesystem::path(o.data_dir) / "airports_tr.json");
    sema::FlightLog flight_log(airports.get());
    std::vector<sema::FlightRecord> flights;
    sema::RecordingSummary recording;
    std::optional<sema::EventLabels> labels;
    if (!o.labels.empty()) {
        std::ifstream in(o.labels, std::ios::binary);
        if (!in) throw std::runtime_error("cannot open " + o.labels);
        labels = sema::read_event_labels_csv(in);
    }

    // Live map. Signals are blocked before the server thread starts and are
    // then waited for with sigtimedwait, which doubles as the pacing sleep.
    sigset_t signals;
    sigemptyset(&signals);
    sigaddset(&signals, SIGINT);
    sigaddset(&signals, SIGTERM);
    std::unique_ptr<sema::LiveServer> server;
    std::unique_ptr<sema::LiveState> live;
    std::string identifiers = o.identifiers;
    if (identifiers.empty()) {
        const char* env = std::getenv("SEMA_IDENTIFIERS");
        identifiers = env != nullptr && *env != '\0' ? env : "hidden";
    }
    const sema::IdentityMasker masker(sema::parse_identifier_mode(identifiers));
    auto public_json = [&masker](nlohmann::json j) {
        masker.apply(j);
        return j.dump();
    };
    if (o.serve_port) {
        if (!area) throw std::runtime_error("--serve needs the boundary (drop --no-area)");
        pthread_sigmask(SIG_BLOCK, &signals, nullptr);
        sema::LiveServerConfig sc;
        sc.address = o.addr;
        sc.port = static_cast<std::uint16_t>(*o.serve_port);
        sc.web_root = o.web_root;
        sc.files["/boundary.geojson"] = std::filesystem::path(o.data_dir) / "turkey_boundary.geojson";
        sema::HistoryApiConfig hc;
        hc.mode = "replay";
        hc.label = o.label;
        hc.max_replay_s = 24 * 3600.0;  // local use: allow long ranges
        hc.mask_identifiers = [&masker](nlohmann::json& j) { masker.apply(j); };
        auto api = std::make_shared<sema::HistoryApi>(preload_history(o, cfg, in_area, *airports), airports, hc);
        sc.api = [api](const sema::ApiRequest& req) { return api->handle(req); };
        server = std::make_unique<sema::LiveServer>(sc);
        server->start();
        live = std::make_unique<sema::LiveState>(*area);
        std::string label = "x" + fmt(o.speed, o.speed == std::round(o.speed) ? 0 : 1);
        if (!o.label.empty()) label = o.label + ", " + label;
        server->publish("hello", sema::live_hello_json("replay", label, sema::to_string(masker.mode())).dump());
        server->publish("events", public_json(detector.events_json()));
        std::cerr << "sema_replay: serving the map on http://" << o.addr << ':' << server->port() << "/ at x"
                  << o.speed << ", Ctrl-C to stop\n";
    }
    // Waits `seconds` of wall time; false if a stop signal arrived.
    auto wait = [&signals](double seconds) {
        if (seconds <= 0.0) return true;
        timespec ts{};
        ts.tv_sec = static_cast<time_t>(seconds);
        ts.tv_nsec = static_cast<long>((seconds - static_cast<double>(ts.tv_sec)) * 1e9);
        return sigtimedwait(&signals, nullptr, &ts) < 0;
    };

    std::optional<double> first_time;
    const auto wall0 = std::chrono::steady_clock::now();
    bool stopped = false;
    while (auto snap = reader.next()) {
        if (server) {
            const auto t = static_cast<double>(snap->time);
            if (!first_time) first_time = t;
            const double due = (t - first_time.value_or(t)) / o.speed;
            const double elapsed = std::chrono::duration<double>(std::chrono::steady_clock::now() - wall0).count();
            if (!wait(due - elapsed)) {
                stopped = true;
                break;
            }
        }
        recording.add(*snap);
        const auto r = tracker.process(*snap);
        const auto ev = detector.process(*snap, tracker, r);
        finished.insert(finished.end(), ev.closed.begin(), ev.closed.end());
        for (auto& f : flight_log.update(*snap, r, tracker)) flights.push_back(std::move(f));
        if (server) {
            if (!ev.empty()) server->publish("events", public_json(detector.events_json()));
            live->update(*snap, tracker);
            server->publish("coverage", live->coverage_json().dump());
            server->publish("snapshot", public_json(live->snapshot_json({live->data_time(), o.speed})));
        }
        if (o.snapshots) {
            nlohmann::json ended = nlohmann::json::array();
            for (const auto& e : r.ended) ended.push_back({{"icao24", e.icao24}, {"reason", sema::to_string(e.reason)}});
            std::cout << nlohmann::json{{"time", r.time},           {"measurements", r.measurements},
                                        {"updates", r.updates},     {"stale", r.stale},
                                        {"rejected", r.rejected},   {"created", r.created},
                                        {"reacquired", r.reacquired}, {"ended", ended},
                                        {"tracked", r.tracked},     {"coasting", r.coasting},
                                        {"processing_us", static_cast<long>(r.processing_us)}}
                             .dump()
                      << '\n';
        }
    }
    if (server && !stopped) {
        // Freeze the map at the end of the recording until Ctrl-C.
        server->publish("snapshot", public_json(live->snapshot_json({live->data_time(), 0.0})));
        std::cerr << "sema_replay: end of recording, Ctrl-C to stop\n";
        int sig = 0;
        sigwait(&signals, &sig);
    }
    if (server) server->stop();
    for (const auto& e : detector.active()) finished.push_back(e);  // still open at the end
    std::optional<nlohmann::json> stats;
    if (o.stats || !o.report.empty()) {
        for (auto& f : flight_log.flush()) flights.push_back(std::move(f));
        sema::StatsQuery q;
        q.from = flights.empty() ? 0.0 : flights.front().first_seen;
        q.to = 0.0;
        for (const auto& f : flights) {
            q.from = std::min(q.from, f.first_seen);
            q.to = std::max(q.to, f.first_seen + 1.0);
        }
        stats = sema::compute_traffic_stats(flights, *airports, q);
        (*stats)["corridors"]["cells"] = nlohmann::json::array();  // long; the map shows them
    }
    if (!o.events_csv.empty()) {
        std::ofstream out(o.events_csv, std::ios::binary);
        if (!out) throw std::runtime_error("cannot write " + o.events_csv);
        sema::write_events_csv(out, finished, labels ? &*labels : nullptr);
        std::cerr << "sema_replay: " << finished.size() << " events written to " << o.events_csv << '\n';
    }
    if (!o.report.empty()) {
        sema::ReportInput in;
        in.recording_name = std::filesystem::path(o.input).filename().string();
        in.generated_at_utc = utc(static_cast<double>(std::time(nullptr))).substr(0, 16);
        in.recording = &recording;
        in.tracker = &tracker.metrics();
        in.anomalies = &detector.metrics();
        in.events = &finished;
        in.labels = labels ? &*labels : nullptr;
        in.stats = &*stats;
        std::ofstream out(o.report, std::ios::binary);
        if (!out) throw std::runtime_error("cannot write " + o.report);
        out << sema::render_report_markdown(in);
        std::cerr << "sema_replay: report written to " << o.report << '\n';
    }
    if (o.stats) {
        std::cout << stats->dump(2) << '\n';
        return 0;
    }
    // Files only, unless the console output was asked for too.
    if ((!o.report.empty() || !o.events_csv.empty()) && !o.json && !o.events) return 0;
    if (o.json) {
        auto j = tracker.metrics().to_json();
        if (o.events) {
            j["anomalies"] = detector.metrics().to_json();
            nlohmann::json list = nlohmann::json::array();
            for (const auto& e : finished) list.push_back(e.to_json(false));
            j["anomaly_events"] = std::move(list);
        }
        std::cout << j.dump(2) << '\n';
    } else {
        print_table(tracker.metrics());
        if (o.events) print_events(detector.metrics(), finished);
    }
    return 0;
}

}  // namespace

int main(int argc, char** argv) {
    try {
        return run(parse(argc, argv));
    } catch (const std::exception& e) {
        std::cerr << "sema_replay: " << e.what() << '\n';
        return 1;
    }
}
