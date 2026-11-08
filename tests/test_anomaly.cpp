#include <gtest/gtest.h>

#include <cmath>
#include <algorithm>
#include <functional>
#include <map>
#include <optional>
#include <memory>
#include <string>
#include <vector>

#include "sema/anomaly.hpp"
#include "sema/geo.hpp"
#include "sema/pipeline.hpp"
#include "sema/snapshot_io.hpp"
#include "sema/tracker.hpp"

using namespace sema;

namespace {

constexpr double kPi = 3.14159265358979323846;
constexpr std::int64_t kT0 = 1'762'549'200;

// What the aircraft is commanded to do at time t (seconds since kT0).
struct Control {
    double turn_dps = 0.0;
    double vrate_ms = 0.0;
    std::string squawk = "2000";
    bool transmitting = true;
};

// Integrates simple kinematics at 1 s and samples it the way OpenSky reports
// it, one fix every `interval` seconds. Pure, deterministic flight paths.
struct Sim {
    std::string icao24 = "4b8001";
    std::string callsign = "SMX101";
    LatLon start{39.0, 32.5};
    double speed = 230.0;
    double track_deg = 90.0;
    double altitude = 11000.0;
    bool on_ground = false;
    std::function<Control(double)> control = [](double) { return Control{}; };

    // Snapshot times -> state (or nothing while not transmitting).
    std::vector<std::pair<std::int64_t, std::optional<StateVector>>> run(double duration_s, int interval = 60) {
        std::vector<std::pair<std::int64_t, std::optional<StateVector>>> out;
        LatLon pos = start;
        double trk = track_deg;
        double alt = altitude;
        Control c = control(0.0);
        for (int t = 0; t <= static_cast<int>(duration_s); ++t) {
            if (t % interval == 0) {
                std::optional<StateVector> s;
                if (c.transmitting) {
                    StateVector v;
                    v.icao24 = icao24;
                    v.callsign = callsign;
                    v.origin_country = "Turkey";
                    v.latitude = pos.lat;
                    v.longitude = pos.lon;
                    v.time_position = kT0 + t;
                    v.last_contact = kT0 + t;
                    v.baro_altitude = alt;
                    v.velocity = speed;
                    v.true_track = std::fmod(trk + 360.0, 360.0);
                    v.vertical_rate = c.vrate_ms;
                    v.squawk = c.squawk;
                    v.on_ground = on_ground;
                    s = v;
                }
                out.emplace_back(kT0 + t, s);
            }
            c = control(static_cast<double>(t));
            trk += c.turn_dps;
            alt = std::max(0.0, alt + c.vrate_ms);
            const double a = trk * kPi / 180.0;
            pos = from_local(pos, {speed * std::sin(a), speed * std::cos(a)});
        }
        return out;
    }
};

// Square 36..42 N, 26..40 E: inside it, every test aircraft is "in the area".
std::shared_ptr<const MultiPolygon> big_area() {
    MultiPolygon::Polygon p;
    p.outer = {{36, 26}, {36, 40}, {42, 40}, {42, 26}, {36, 26}};
    return std::make_shared<const MultiPolygon>(std::vector<MultiPolygon::Polygon>{p});
}

struct Harness {
    Tracker tracker;
    AnomalyDetector detector;
    std::vector<AnomalyEvent> opened;
    std::vector<AnomalyEvent> closed;
    std::size_t updated = 0;

    explicit Harness(AnomalyConfig cfg = {}, std::shared_ptr<const MultiPolygon> area = nullptr)
        : tracker({}, area ? std::function<bool(LatLon)>([area](LatLon p) { return area->contains(p); })
                           : std::function<bool(LatLon)>()),
          detector(cfg, std::move(area)) {}

    void feed(const StatesSnapshot& s) {
        const auto r = tracker.process(s);
        const auto a = detector.process(s, tracker, r);
        opened.insert(opened.end(), a.opened.begin(), a.opened.end());
        closed.insert(closed.end(), a.closed.begin(), a.closed.end());
        updated += a.updated.size();
    }

    // Merges several simulated aircraft by snapshot time.
    void feed_all(std::vector<std::vector<std::pair<std::int64_t, std::optional<StateVector>>>> runs) {
        std::map<std::int64_t, std::vector<StateVector>> by_time;
        for (auto& r : runs) {
            for (auto& [t, s] : r) {
                auto& v = by_time[t];
                if (s) v.push_back(*s);
            }
        }
        for (auto& [t, v] : by_time) feed({t, v});
    }

    [[nodiscard]] std::size_t count(EventKind k) const {
        std::size_t n = 0;
        for (const auto& e : opened) n += e.kind == k ? 1 : 0;
        return n;
    }
};

}  // namespace

TEST(Anomaly, StraightCruiseRaisesNothing) {
    Harness h({}, big_area());
    Sim a;
    Sim b;
    b.icao24 = "4b8002";
    b.start = {40.5, 28.0};
    b.track_deg = 135.0;
    b.altitude = 9000.0;
    h.feed_all({a.run(3600), b.run(3600)});
    EXPECT_TRUE(h.opened.empty()) << h.opened.front().summary;
    EXPECT_GT(h.detector.metrics().airborne_aircraft_hours, 1.5);
}

TEST(Anomaly, NormalDescentIsNotRapid) {
    Harness h;
    Sim s;
    s.control = [](double t) { return Control{0.0, (t > 300 && t < 1100) ? -10.0 : 0.0}; };  // ~2,000 ft/min
    h.feed_all({s.run(1800)});
    EXPECT_EQ(h.count(EventKind::RapidDescent), 0U);
}

TEST(Anomaly, EmergencyDescentRaisesOneEventThatEnds) {
    Harness h;
    Sim s;
    // 35 m/s (~6,900 ft/min) from 11,000 m to ~4,000 m, then level.
    s.control = [](double t) { return Control{0.0, (t > 300 && t <= 500) ? -35.0 : 0.0}; };
    h.feed_all({s.run(1800)});
    ASSERT_EQ(h.count(EventKind::RapidDescent), 1U);
    ASSERT_EQ(h.closed.size(), 1U);
    const auto& e = h.closed.front();
    EXPECT_EQ(e.kind, EventKind::RapidDescent);
    EXPECT_FALSE(e.active);
    EXPECT_EQ(e.callsign, "SMX101");
    EXPECT_GE(e.details.at("drop_m").get<double>(), 5000.0);
    EXPECT_GE(e.details.at("rate_ms").get<double>(), 25.0);
    EXPECT_LE(e.start_time, kT0 + 360);
    EXPECT_GE(e.end_time, kT0 + 480);
    EXPECT_FALSE(e.track.empty());
}

TEST(Anomaly, SingleEmergencySquawkIsIgnored) {
    Harness h;
    Sim s;
    s.control = [](double t) { return Control{0.0, 0.0, (t >= 600 && t < 660) ? "7700" : "2000"}; };
    h.feed_all({s.run(1800)});
    EXPECT_EQ(h.count(EventKind::Emergency), 0U);
}

TEST(Anomaly, EmergencySquawkOpensAndClears) {
    Harness h;
    Sim s;
    s.control = [](double t) { return Control{0.0, 0.0, (t >= 600 && t < 900) ? "7700" : "2000"}; };
    h.feed_all({s.run(1800)});
    ASSERT_EQ(h.count(EventKind::Emergency), 1U);
    const auto& o = h.opened.front();
    EXPECT_EQ(o.details.at("squawk"), "7700");
    EXPECT_EQ(o.details.at("meaning"), "genel acil durum");
    EXPECT_EQ(o.start_time, static_cast<double>(kT0 + 660));  // first report carrying it
    ASSERT_EQ(h.closed.size(), 1U);
    EXPECT_GE(h.closed.front().end_time, static_cast<double>(kT0 + 900));
    EXPECT_TRUE(h.detector.active().empty());
}

TEST(Anomaly, EmergencySquawkOnGroundIsIgnored) {
    Harness h;
    Sim s;
    s.speed = 5.0;
    s.altitude = 0.0;
    s.on_ground = true;
    s.control = [](double) { return Control{0.0, 0.0, "7700"}; };
    h.feed_all({s.run(900)});
    EXPECT_EQ(h.count(EventKind::Emergency), 0U);
}

TEST(Anomaly, HoldingPatternIsDetected) {
    Harness h;
    Sim s;
    s.speed = 120.0;
    s.altitude = 4500.0;
    // 10 min straight, then a racetrack (1 min legs, 180 deg turns at 3 deg/s)
    // for 24 min, then straight out.
    s.control = [](double t) {
        if (t < 600 || t >= 600 + 24 * 60) return Control{};
        const double p = std::fmod(t - 600, 240.0);
        return Control{(p >= 60 && p < 120) || (p >= 180) ? 3.0 : 0.0, 0.0};
    };
    h.feed_all({s.run(3600)});
    ASSERT_EQ(h.count(EventKind::Holding), 1U);
    // A hold is not a route deviation.
    EXPECT_EQ(h.count(EventKind::RouteDeviation), 0U);
    ASSERT_EQ(h.closed.size(), 1U);
    const auto& e = h.closed.front();
    EXPECT_GE(e.end_time - e.start_time, 15 * 60.0);
    EXPECT_LE(e.details.at("radius_m").get<double>(), 15000.0);
    EXPECT_GT(h.updated, 0U);
}

TEST(Anomaly, TurnBackAtCruiseIsARouteDeviation) {
    Harness h;
    Sim s;
    s.control = [](double t) { return Control{(t >= 900 && t < 960) ? 3.0 : 0.0, 0.0}; };  // 180 deg in 1 min
    h.feed_all({s.run(2400)});
    ASSERT_EQ(h.count(EventKind::RouteDeviation), 1U);
    const auto& e = h.opened.front();
    EXPECT_FALSE(e.active);  // a moment, not a state
    EXPECT_NEAR(e.details.at("turn_deg").get<double>(), 180.0, 10.0);
    EXPECT_EQ(h.count(EventKind::Holding), 0U);
}

TEST(Anomaly, AirwayTurnIsNotARouteDeviation) {
    Harness h;
    Sim s;
    s.control = [](double t) { return Control{(t >= 900 && t < 920) ? 3.0 : 0.0, 0.0}; };  // 60 deg
    h.feed_all({s.run(2400)});
    EXPECT_EQ(h.count(EventKind::RouteDeviation), 0U);
}

TEST(Anomaly, TurnBackDuringApproachIsNotADeviation) {
    Harness h;
    Sim s;
    s.altitude = 3000.0;  // below the cruise floor: downwind/base turns are routine
    s.speed = 120.0;
    s.control = [](double t) { return Control{(t >= 900 && t < 960) ? 3.0 : 0.0, 0.0}; };
    h.feed_all({s.run(2400)});
    EXPECT_EQ(h.count(EventKind::RouteDeviation), 0U);
}

namespace {

// Two aircraft on the same airway (to build coverage), the second goes silent
// for `silent_s` from t=1200.
Harness run_signal_loss(double silent_s, std::uint64_t min_coverage) {
    AnomalyConfig cfg;
    cfg.signal_loss_min_coverage = min_coverage;
    Harness h(cfg, big_area());
    std::vector<Sim> sims;
    for (int i = 0; i < 6; ++i) {
        Sim s;
        s.icao24 = "4b81" + std::to_string(10 + i);
        s.callsign = "SMX2" + std::to_string(10 + i);
        s.start = {39.0, 29.0 + 0.02 * i};
        s.speed = 30.0;  // slow so they stay in the same coverage cells
        s.altitude = 9000.0 + 300.0 * i;
        sims.push_back(s);
    }
    sims[0].control = [silent_s](double t) {
        Control c;
        c.transmitting = !(t >= 1200 && t < 1200 + silent_s);
        return c;
    };
    std::vector<std::vector<std::pair<std::int64_t, std::optional<StateVector>>>> runs;
    for (auto& s : sims) runs.push_back(s.run(3000));
    h.feed_all(runs);
    return h;
}

}  // namespace

TEST(Anomaly, LongSignalLossInCoveredAreaIsRaisedAndClosedOnReacquire) {
    const auto h = run_signal_loss(480.0, 50);
    ASSERT_EQ(h.count(EventKind::SignalLoss), 1U);
    ASSERT_EQ(h.closed.size(), 1U);
    const auto& e = h.closed.front();
    EXPECT_EQ(e.icao24, "4b8110");
    EXPECT_TRUE(e.details.at("reacquired").get<bool>());
    EXPECT_GE(e.details.at("gap_s").get<double>(), 480.0);
}

TEST(Anomaly, ShortGapIsNotASignalLoss) {
    const auto h = run_signal_loss(180.0, 50);
    EXPECT_EQ(h.count(EventKind::SignalLoss), 0U);
}

TEST(Anomaly, SignalLossInPoorlyCoveredAreaIsIgnored) {
    const auto h = run_signal_loss(480.0, 100000);
    EXPECT_EQ(h.count(EventKind::SignalLoss), 0U);
}

TEST(Anomaly, MilitaryAircraftNeverReachTheDetector) {
    // The collector filters before tracking; a military aircraft squawking
    // 7700 must not produce an event.
    const auto area = big_area();
    const MilitaryFilter military(MilitaryRules::from_json(R"({"callsign_patterns": ["TUAF[0-9]+[A-Z]*"]})"), nullptr);
    Harness h;
    Sim s;
    s.callsign = "TUAF123";
    s.control = [](double) { return Control{0.0, 0.0, "7700"}; };
    for (auto& [t, st] : s.run(1200)) {
        std::vector<StateVector> raw;
        if (st) raw.push_back(*st);
        FilterStats stats;
        h.feed({t, filter_states(raw, *area, military, stats)});
    }
    EXPECT_TRUE(h.opened.empty());
}

TEST(Anomaly, EventsJsonListsActiveFirstWithTracks) {
    Harness h;
    Sim a;  // squawks 7700 from t=600 to the end: stays active
    a.control = [](double t) { return Control{0.0, 0.0, t >= 600 ? "7700" : "2000"}; };
    Sim b;  // squawks 7600 for a while: closed
    b.icao24 = "4b8002";
    b.callsign = "SMX102";
    b.start = {40.0, 30.0};
    b.control = [](double t) { return Control{0.0, 0.0, (t >= 300 && t < 600) ? "7600" : "2000"}; };
    h.feed_all({a.run(1500), b.run(1500)});

    const auto j = h.detector.events_json();
    EXPECT_EQ(j.at("type"), "events");
    EXPECT_EQ(j.at("protocol"), kLiveProtocolVersion);
    EXPECT_EQ(j.at("active"), 1);
    EXPECT_EQ(j.at("counts").at("emergency"), 2);
    ASSERT_EQ(j.at("events").size(), 2U);
    EXPECT_EQ(j.at("events")[0].at("icao24"), "4b8001");
    EXPECT_TRUE(j.at("events")[0].at("active").get<bool>());
    EXPECT_EQ(j.at("events")[1].at("details").at("squawk"), "7600");
    EXPECT_FALSE(j.at("events")[1].at("track").empty());
    const auto& pt = j.at("events")[1].at("track")[0];
    EXPECT_EQ(pt.size(), 4U);  // lon, lat, alt, t
    EXPECT_EQ(j.at("events")[0].at("key"), "emergency:4b8001:" + std::to_string(kT0 + 660));
}

TEST(Anomaly, MetricsReportRatesPerThousandHours) {
    Harness h;
    Sim s;
    s.control = [](double t) { return Control{0.0, 0.0, (t >= 600 && t < 900) ? "7700" : "2000"}; };
    h.feed_all({s.run(3600)});
    const auto j = h.detector.metrics().to_json();
    EXPECT_EQ(j.at("events"), 1);
    EXPECT_NEAR(j.at("airborne_aircraft_hours").get<double>(), 1.0, 0.1);
    EXPECT_NEAR(j.at("events_per_1000_aircraft_hours").at("emergency").get<double>(), 1000.0, 100.0);
}

namespace {

std::vector<AnomalyEvent> replay_fixture(const std::string& name, AnomalyConfig cfg,
                                         std::shared_ptr<const MultiPolygon> area) {
    Tracker tracker({}, area ? std::function<bool(LatLon)>([area](LatLon p) { return area->contains(p); })
                             : std::function<bool(LatLon)>());
    AnomalyDetector det(cfg, area);
    SnapshotReader reader(std::string(SEMA_FIXTURE_DIR) + "/" + name);
    std::vector<AnomalyEvent> opened;
    while (auto s = reader.next()) {
        const auto r = tracker.process(*s);
        auto a = det.process(*s, tracker, r);
        opened.insert(opened.end(), a.opened.begin(), a.opened.end());
    }
    return opened;
}

std::shared_ptr<const MultiPolygon> turkey() {
    return std::make_shared<const MultiPolygon>(
        MultiPolygon::load_geojson(std::string(SEMA_DATA_DIR) + "/turkey_boundary.geojson"));
}

}  // namespace

TEST(Anomaly, TrackerFixtureRaisesOnlyTheHolding) {
    // tools/make_replay_fixture.py flies SMX601 through two holding laps; its
    // other flights are normal or carry data glitches that must not raise
    // anything (a teleport, a 30 km altitude, a duplicated address...).
    const auto opened = replay_fixture("replay_synthetic.jsonl", {}, turkey());
    ASSERT_EQ(opened.size(), 1U);
    EXPECT_EQ(opened[0].kind, EventKind::Holding);
    EXPECT_EQ(opened[0].callsign, "SMX601");
}

TEST(Anomaly, AnomalyFixtureRaisesExactlyTheInjectedEvents) {
    // tools/make_anomaly_fixture.py: one scripted aircraft per rule, 30
    // ordinary flights, and a single corrupt 7500 squawk that must be ignored.
    AnomalyConfig cfg;
    cfg.signal_loss_min_coverage = 3;  // 90 minutes of data, not 24 hours
    const auto opened = replay_fixture("anomaly_synthetic.jsonl", cfg, turkey());
    std::map<std::string, std::string> got;
    for (const auto& e : opened) got[e.callsign] = to_string(e.kind);
    const std::map<std::string, std::string> want = {
        {"SMA700", "emergency"},       {"SMA350", "rapid_descent"}, {"SMA180", "route_deviation"},
        {"SMA601", "holding"},         {"SMA090", "signal_loss"},
    };
    EXPECT_EQ(got, want);
    EXPECT_EQ(opened.size(), want.size());
}

TEST(Anomaly, DefaultCoverageThresholdSilencesSignalLossOnShortRecordings) {
    const auto opened = replay_fixture("anomaly_synthetic.jsonl", {}, turkey());
    for (const auto& e : opened) EXPECT_NE(e.kind, EventKind::SignalLoss);
}

TEST(Anomaly, ThresholdsCanBeSetByName) {
    AnomalyConfig cfg;
    EXPECT_TRUE(set_anomaly_param(cfg, "descent_min_rate_ms", 18.5));
    EXPECT_DOUBLE_EQ(cfg.descent_min_rate_ms, 18.5);
    EXPECT_TRUE(set_anomaly_param(cfg, "squawk_confirm_reports", 3));
    EXPECT_EQ(cfg.squawk_confirm_reports, 3);
    EXPECT_TRUE(set_anomaly_param(cfg, "signal_loss_min_coverage", 50));
    EXPECT_EQ(cfg.signal_loss_min_coverage, 50U);
    EXPECT_FALSE(set_anomaly_param(cfg, "no_such_threshold", 1));
    EXPECT_FALSE(set_anomaly_param(cfg, "holding_max_radius_m", -1));
    EXPECT_EQ(anomaly_param_names().size(), 19U);
}

TEST(Anomaly, StricterThresholdSilencesARule) {
    // Tuning check: the injected 35 m/s descent is not raised once the rate
    // threshold is above it.
    AnomalyConfig cfg;
    cfg.signal_loss_min_coverage = 3;
    ASSERT_TRUE(set_anomaly_param(cfg, "descent_min_rate_ms", 40));
    for (const auto& e : replay_fixture("anomaly_synthetic.jsonl", cfg, turkey())) {
        EXPECT_NE(e.kind, EventKind::RapidDescent);
    }
}
