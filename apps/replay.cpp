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

#include <cstdio>
#include <cstdlib>
#include <functional>
#include <optional>
#include <exception>
#include <filesystem>
#include <iostream>
#include <memory>
#include <string>
#include <string_view>

#include <nlohmann/json.hpp>

#include "sema/geo.hpp"
#include "sema/snapshot_io.hpp"
#include "sema/tracker.hpp"

namespace {

struct Options {
    std::string input;
    std::string data_dir = "data";
    bool use_area = true;
    bool json = false;
    bool snapshots = false;
    std::optional<double> accel_psd;
    std::optional<double> turn_psd;
    std::optional<double> ct_accel_psd;
};

[[noreturn]] void usage(int code) {
    std::cerr << "usage: sema_replay <recording.jsonl> [--data-dir DIR] [--no-area] [--accel-psd Q] [--ct-accel-psd Q] [--turn-psd Q] [--json] "
                 "[--snapshots]\n";
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
        } else if (a == "--json") {
            o.json = true;
        } else if (a == "--snapshots") {
            o.snapshots = true;
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
    sema::SnapshotReader reader(o.input);
    while (auto snap = reader.next()) {
        const auto r = tracker.process(*snap);
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
    if (o.json) {
        std::cout << tracker.metrics().to_json().dump(2) << '\n';
    } else {
        print_table(tracker.metrics());
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
