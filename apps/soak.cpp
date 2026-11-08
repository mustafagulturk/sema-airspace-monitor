// Semâ soak test: plays a recording over and over, shifted forward in time,
// through the same in-memory pipeline the collector runs (tracker, anomaly
// detector, flight log, history, live map state, Prometheus registry), and
// watches the process's resident memory. A memory leak or an unbounded
// container shows up as RSS that keeps climbing after every rolling window
// (24 h coverage and replay, flight history) has filled.
//
// usage: sema_soak <recording.jsonl> [--days D] [--data-dir DIR]
//                  [--warmup-days W] [--max-growth-mb M] [--keep-flights-days K]
//                  [--csv FILE] [--quiet]
//   --days D            simulated days to run (default 14, the project's
//                       "two weeks without interruption" criterion)
//   --warmup-days W     RSS baseline is the peak up to this simulated day
//                       (default: keep-flights-days + 1, when every window is full)
//   --max-growth-mb M   fail (exit 3) if the peak RSS after warm-up exceeds the
//                       baseline by more than M MB (default 16)
//   --keep-flights-days K  flight history window (default 31, as the collector);
//                       a shorter one reaches steady state sooner
//   --csv FILE          one row per simulated hour: sim_hours, rss_mb, tracks,
//                       live_aircraft, history_fixes, history_flights, events
//   --quiet             no progress lines
//
// Each pass ends with a 30 minute gap, so every track ends by signal loss
// before the recording starts again. Build with the asan preset to also get
// LeakSanitizer's report at exit.

#include <unistd.h>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <exception>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "sema/anomaly.hpp"
#include "sema/flights.hpp"
#include "sema/geo.hpp"
#include "sema/history.hpp"
#include "sema/live.hpp"
#include "sema/prom_metrics.hpp"
#include "sema/snapshot_io.hpp"
#include "sema/tracker.hpp"
#include "sema/traffic_stats.hpp"

namespace {

struct Options {
    std::string input;
    std::string data_dir = "data";
    double days = 14.0;
    std::optional<double> warmup_days;
    double max_growth_mb = 16.0;
    double keep_flights_days = 31.0;
    std::string csv;
    bool quiet = false;
};

[[noreturn]] void usage(int code) {
    std::cerr << "usage: sema_soak <recording.jsonl> [--days D] [--data-dir DIR] [--warmup-days W] "
                 "[--max-growth-mb M] [--keep-flights-days K] [--csv FILE] [--quiet]\n";
    std::exit(code);
}

Options parse(int argc, char** argv) {
    Options o;
    for (int i = 1; i < argc; ++i) {
        const std::string_view a = argv[i];
        auto number = [&]() -> double {
            if (i + 1 >= argc) usage(2);
            const double v = std::stod(argv[++i]);
            if (!(v >= 0.0)) usage(2);
            return v;
        };
        if (a == "--days") {
            o.days = number();
        } else if (a == "--data-dir") {
            if (i + 1 >= argc) usage(2);
            o.data_dir = argv[++i];
        } else if (a == "--warmup-days") {
            o.warmup_days = number();
        } else if (a == "--max-growth-mb") {
            o.max_growth_mb = number();
        } else if (a == "--keep-flights-days") {
            o.keep_flights_days = number();
        } else if (a == "--csv") {
            if (i + 1 >= argc) usage(2);
            o.csv = argv[++i];
        } else if (a == "--quiet") {
            o.quiet = true;
        } else if (a == "-h" || a == "--help") {
            usage(0);
        } else if (!a.empty() && a[0] != '-' && o.input.empty()) {
            o.input = a;
        } else {
            usage(2);
        }
    }
    if (o.input.empty() || o.days <= 0.0) usage(2);
    return o;
}

double rss_mb() {
    std::ifstream statm("/proc/self/statm");
    long pages_total = 0;
    long pages_resident = 0;
    if (!(statm >> pages_total >> pages_resident)) return 0.0;
    return static_cast<double>(pages_resident) * static_cast<double>(sysconf(_SC_PAGESIZE)) / (1024.0 * 1024.0);
}

std::vector<sema::StatesSnapshot> load(const std::string& path) {
    std::vector<sema::StatesSnapshot> out;
    sema::SnapshotReader reader(path);
    while (auto s = reader.next()) out.push_back(std::move(*s));
    if (out.size() < 2) throw std::runtime_error("recording needs at least two snapshots");
    return out;
}

void shift(sema::StatesSnapshot& s, std::int64_t dt) {
    s.time += dt;
    for (auto& v : s.states) {
        if (v.time_position) *v.time_position += dt;
        v.last_contact += dt;
    }
}

// Least-squares slope of y over x (MB per day).
double slope(const std::vector<std::pair<double, double>>& pts) {
    if (pts.size() < 2) return 0.0;
    double sx = 0, sy = 0, sxx = 0, sxy = 0;
    for (const auto& [x, y] : pts) {
        sx += x;
        sy += y;
        sxx += x * x;
        sxy += x * y;
    }
    const auto n = static_cast<double>(pts.size());
    const double d = n * sxx - sx * sx;
    return d == 0.0 ? 0.0 : (n * sxy - sx * sy) / d;
}

int run(const Options& o) {
    const auto recording = load(o.input);
    const std::filesystem::path data(o.data_dir);
    auto area = std::make_shared<sema::MultiPolygon>(sema::MultiPolygon::load_geojson(data / "turkey_boundary.geojson"));
    const auto airports = sema::AirportIndex::load(data / "airports_tr.json");

    sema::Tracker tracker(sema::TrackerConfig{}, [area](sema::LatLon p) { return area->contains(p); });
    sema::AnomalyDetector detector(sema::AnomalyConfig{}, area);
    sema::FlightLog flight_log(&airports);
    sema::MemoryHistory::Limits limits;
    limits.keep_flights_s = o.keep_flights_days * 86400.0;
    sema::MemoryHistory history(limits);
    sema::LiveState live(*area);
    sema::PromRegistry prom;

    const std::int64_t gap = 1800;
    const std::int64_t pass_s = recording.back().time - recording.front().time + gap;
    const std::int64_t t0 = recording.front().time;
    const double total_s = o.days * 86400.0;
    const double warmup_s = o.warmup_days.value_or(o.keep_flights_days + 1.0) * 86400.0;
    if (warmup_s >= total_s) {
        std::cerr << "sema_soak: warm-up (" << warmup_s / 86400.0 << " d) must be shorter than the run ("
                  << o.days << " d); pass --warmup-days or --keep-flights-days\n";
        return 2;
    }

    std::ofstream csv;
    if (!o.csv.empty()) {
        csv.open(o.csv);
        if (!csv) throw std::runtime_error("cannot write " + o.csv);
        csv << "sim_hours,rss_mb,tracks,live_aircraft,history_fixes,history_flights,events\n";
    }

    const double rss_start = rss_mb();
    double baseline = 0.0;    // peak RSS during warm-up
    double after_peak = 0.0;  // peak RSS after warm-up
    std::vector<std::pair<double, double>> after;  // (sim days, rss) after warm-up
    std::uint64_t snapshots = 0;
    std::uint64_t events = 0;
    std::size_t json_bytes = 0;  // keeps the serialisation from being optimised away
    double next_sample = 0.0;
    const auto wall0 = std::chrono::steady_clock::now();

    for (std::int64_t pass = 0;; ++pass) {
        bool done = false;
        for (const auto& original : recording) {
            sema::StatesSnapshot snap = original;
            shift(snap, pass * pass_s);
            const double sim = static_cast<double>(snap.time - t0);
            if (sim > total_s) {
                done = true;
                break;
            }
            const auto r = tracker.process(snap);
            const auto ev = detector.process(snap, tracker, r);
            events += ev.opened.size();
            const auto ended = flight_log.update(snap, r, tracker);
            history.record(snap, ended);
            history.set_open_flights(flight_log.open_flights());
            live.update(snap, tracker);
            json_bytes += live.snapshot_json({live.data_time(), 1.0}).dump().size();
            if (!ev.empty()) json_bytes += detector.events_json().dump().size();
            prom.set_gauge("sema_tracked_aircraft", "Tracked aircraft", r.tracked);
            prom.add_counter("sema_snapshots_total", "Snapshots", 1);
            ++snapshots;

            if (sim >= next_sample) {
                next_sample += 3600.0;
                // Once an hour, what the site's API and /metrics would do.
                const double t = static_cast<double>(snap.time);
                json_bytes += live.coverage_json().dump().size();
                json_bytes += history.replay(t - 3600.0, t).size();
                sema::StatsQuery q;
                q.from = t - 7 * 86400.0;
                q.to = t;
                json_bytes += sema::compute_traffic_stats(history.flights(q.from, q.to), airports, q).dump().size();
                json_bytes += prom.render().size();

                const double rss = rss_mb();
                if (sim <= warmup_s) {
                    baseline = std::max(baseline, rss);
                } else {
                    after_peak = std::max(after_peak, rss);
                    after.emplace_back(sim / 86400.0, rss);
                }
                if (csv) {
                    csv << sim / 3600.0 << ',' << rss << ',' << r.tracked << ',' << live.aircraft_count() << ','
                        << history.fix_count() << ',' << history.flight_count() << ',' << events << '\n';
                }
                if (!o.quiet && static_cast<long>(sim) % 86400 < 3600) {
                    (void)std::fprintf(stderr, "day %5.1f  rss %7.1f MB  tracks %3zu  fixes %8zu  flights %6zu  events %llu\n",
                                 sim / 86400.0, rss, static_cast<std::size_t>(r.tracked), history.fix_count(),
                                 history.flight_count(), static_cast<unsigned long long>(events));
                }
            }
        }
        if (done) break;
    }

    const double wall = std::chrono::duration<double>(std::chrono::steady_clock::now() - wall0).count();
    const double growth = after_peak - baseline;
    const double mb_per_day = slope(after);
    (void)std::printf(
        "simulated %.1f days (%llu snapshots, %llu events) in %.0f s\n"
        "rss: start %.1f MB, warm-up peak %.1f MB (first %.1f days), later peak %.1f MB, growth %+.1f MB, "
        "trend %+.2f MB/day\n",
        o.days, static_cast<unsigned long long>(snapshots), static_cast<unsigned long long>(events), wall, rss_start,
        baseline, warmup_s / 86400.0, after_peak, growth, mb_per_day);
    if (json_bytes == 0) std::printf("(no output produced)\n");
    if (growth > o.max_growth_mb) {
        (void)std::printf("FAIL: memory grew %.1f MB after warm-up (limit %.1f MB)\n", growth, o.max_growth_mb);
        return 3;
    }
    (void)std::printf("OK: memory flat after warm-up (limit %.1f MB)\n", o.max_growth_mb);
    return 0;
}

}  // namespace

int main(int argc, char** argv) {
    try {
        return run(parse(argc, argv));
    } catch (const std::exception& e) {
        std::cerr << "sema_soak: " << e.what() << '\n';
        return 1;
    }
}
