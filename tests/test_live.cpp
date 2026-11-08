#include <gtest/gtest.h>

#include <cmath>
#include <string>

#include "sema/geo.hpp"
#include "sema/live.hpp"
#include "sema/tracker.hpp"

using namespace sema;

namespace {

constexpr std::int64_t kT0 = 1'762'549'200;  // on an hour boundary
constexpr double kPi = 3.14159265358979323846;

// 38..40 N, 30..33 E.
MultiPolygon square() {
    return MultiPolygon::from_geojson(
        R"({"type":"Polygon","coordinates":[[[30,38],[33,38],[33,40],[30,40],[30,38]]]})");
}

StateVector flight(const std::string& icao, std::int64_t t, LatLon start, double track_deg = 90.0,
                   double speed = 200.0) {
    const double d = speed * static_cast<double>(t - kT0);
    const double a = track_deg * kPi / 180.0;
    const LatLon p = from_local(start, {d * std::sin(a), d * std::cos(a)});
    StateVector s;
    s.icao24 = icao;
    s.callsign = "SMX" + icao.substr(3);
    s.origin_country = "Turkey";
    s.latitude = p.lat;
    s.longitude = p.lon;
    s.time_position = t;
    s.last_contact = t;
    s.baro_altitude = 9000.0;
    s.velocity = speed;
    s.true_track = track_deg;
    s.vertical_rate = 0.0;
    return s;
}

}  // namespace

TEST(CoverageGrid, KeepsOnlyCellsOverlappingTheArea) {
    const CoverageGrid g(square(), 0.5, 24);
    EXPECT_EQ(g.cell_count(), 4U * 6U);  // 2 deg x 3 deg in 0.5 deg cells

    // A triangle covers fewer cells than its bounding box.
    const auto tri = MultiPolygon::from_geojson(
        R"({"type":"Polygon","coordinates":[[[30,38],[33,38],[30,40],[30,38]]]})");
    const CoverageGrid t(tri, 0.5, 24);
    EXPECT_LT(t.cell_count(), 24U);
    EXPECT_GT(t.cell_count(), 12U);
}

TEST(CoverageGrid, CountsReportsInARollingWindow) {
    CoverageGrid g(square(), 0.5, 3);
    const LatLon ist{39.2, 31.2};
    const double t = static_cast<double>(kT0);
    g.add(ist, t);
    g.add(ist, t + 10);
    g.add({39.3, 31.4}, t + 20);    // same cell
    g.add({38.2, 30.2}, t + 30);    // other cell
    g.add({45.0, 31.0}, t + 40);    // outside the area: ignored
    EXPECT_EQ(g.count_at(ist, t + 60), 3U);
    EXPECT_EQ(g.count_at({38.2, 30.2}, t + 60), 1U);

    g.add(ist, t + 3600);           // next hour
    EXPECT_EQ(g.count_at(ist, t + 3600), 4U);
    // Three hours later the first hour has left the 3-hour window.
    EXPECT_EQ(g.count_at(ist, t + 3 * 3600), 1U);
    // And its ring slot is reused, not added to.
    g.add(ist, t + 3 * 3600);
    EXPECT_EQ(g.count_at(ist, t + 3 * 3600), 2U);
    // A report older than the window is ignored.
    g.add(ist, t);
    EXPECT_EQ(g.count_at(ist, t + 3 * 3600), 2U);
}

TEST(CoverageGrid, JsonListsEveryAreaCellIncludingGaps) {
    CoverageGrid g(square(), 1.0, 24);
    g.add({39.5, 31.5}, static_cast<double>(kT0));
    const auto j = g.to_json(static_cast<double>(kT0));
    EXPECT_EQ(j["type"], "coverage");
    EXPECT_EQ(j["cells"].size(), 6U);
    EXPECT_EQ(j["max"], 1);
    int with_data = 0;
    for (const auto& c : j["cells"]) {
        ASSERT_EQ(c.size(), 3U);
        if (c[2].get<int>() > 0) {
            ++with_data;
            EXPECT_DOUBLE_EQ(c[0].get<double>(), 39.0);
            EXPECT_DOUBLE_EQ(c[1].get<double>(), 31.0);
        }
    }
    EXPECT_EQ(with_data, 1);
}

TEST(CoverageGrid, TurkeyBoundaryHasAPlausibleNumberOfCells) {
    const auto area = MultiPolygon::load_geojson(std::string(SEMA_DATA_DIR) + "/turkey_boundary.geojson");
    const CoverageGrid g(area, 0.5, 24);
    // Turkey is ~780,000 km^2 plus sea; a 0.5 deg cell is ~2,400 km^2 here.
    EXPECT_GT(g.cell_count(), 300U);
    EXPECT_LT(g.cell_count(), 500U);
}

TEST(LiveState, SnapshotCarriesTrackEstimatesAndTrails) {
    const auto area = square();
    Tracker tr;
    LiveState live(area);
    const LatLon start{39.0, 30.5};
    for (int i = 0; i < 5; ++i) {
        const std::int64_t t = kT0 + 60 * i;
        const StatesSnapshot s{t, {flight("4b8001", t, start)}};
        tr.process(s);
        live.update(s, tr);
    }
    const auto j = live.snapshot_json({static_cast<double>(kT0 + 240) + 3.0, 1.0});
    EXPECT_EQ(j["type"], "snapshot");
    EXPECT_EQ(j["protocol"], kLiveProtocolVersion);
    EXPECT_DOUBLE_EQ(j["clock"]["time"].get<double>(), static_cast<double>(kT0 + 243));
    EXPECT_EQ(j["counts"]["tracked"], 1);
    ASSERT_EQ(j["aircraft"].size(), 1U);
    const auto& a = j["aircraft"][0];
    EXPECT_EQ(a["icao24"], "4b8001");
    EXPECT_EQ(a["callsign"], "SMX001");
    EXPECT_EQ(a["country"], "Turkey");
    EXPECT_EQ(a["state"], "confirmed");
    EXPECT_NEAR(a["speed"].get<double>(), 200.0, 2.0);
    EXPECT_NEAR(a["track"].get<double>(), 90.0, 1.0);
    EXPECT_NEAR(a["alt"].get<double>(), 9000.0, 20.0);
    EXPECT_FALSE(a["ground"].get<bool>());
    // One trail point per fix, oldest first, as [lon, lat], moving east.
    ASSERT_EQ(a["trail"].size(), 5U);
    for (std::size_t i = 1; i < 5; ++i) {
        EXPECT_GT(a["trail"][i][0].get<double>(), a["trail"][i - 1][0].get<double>());
        EXPECT_NEAR(a["trail"][i][1].get<double>(), 39.0, 0.01);
    }
    // The newest trail point is the filtered position at the last fix.
    EXPECT_NEAR(a["trail"][4][0].get<double>(), a["lon"].get<double>(), 1e-4);
}

TEST(LiveState, TrailsAreBoundedByLengthAndAge) {
    const auto area = square();
    Tracker tr;
    LiveConfig cfg;
    cfg.trail_points = 4;
    cfg.trail_max_age_s = 150.0;
    LiveState live(area, cfg);
    for (int i = 0; i < 8; ++i) {
        const std::int64_t t = kT0 + 60 * i;
        const StatesSnapshot s{t, {flight("4b8001", t, {39.0, 30.5})}};
        tr.process(s);
        live.update(s, tr);
    }
    // 150 s of history at 60 s spacing: fixes at t-120, t-60, t (t-180 is too old).
    EXPECT_EQ(live.snapshot_json({})["aircraft"][0]["trail"].size(), 3U);
}

TEST(LiveState, StaleSnapshotsDoNotDuplicateTrailPoints) {
    const auto area = square();
    Tracker tr;
    LiveState live(area);
    auto s0 = flight("4b8001", kT0, {39.0, 30.5});
    tr.process({kT0, {s0}});
    live.update({kT0, {s0}}, tr);
    // OpenSky repeats the last report until a new one arrives.
    tr.process({kT0 + 60, {s0}});
    live.update({kT0 + 60, {s0}}, tr);
    EXPECT_EQ(live.snapshot_json({})["aircraft"][0]["trail"].size(), 1U);
}

TEST(LiveState, DropsEndedTracksAndCountsCoverage) {
    const auto area = square();
    TrackerConfig tc;
    tc.terminate_after_s = 120.0;
    Tracker tr(tc);
    LiveState live(area);
    const LatLon a{39.2, 30.2};
    const LatLon b{38.2, 32.2};
    for (int i = 0; i < 3; ++i) {
        const std::int64_t t = kT0 + 60 * i;
        const StatesSnapshot s{t, {flight("4b8001", t, a), flight("4b8002", t, b)}};
        tr.process(s);
        live.update(s, tr);
    }
    EXPECT_EQ(live.aircraft_count(), 2U);
    // 4b8002 goes silent and is terminated after 120 s.
    for (int i = 3; i < 7; ++i) {
        const std::int64_t t = kT0 + 60 * i;
        const StatesSnapshot s{t, {flight("4b8001", t, a)}};
        tr.process(s);
        live.update(s, tr);
    }
    EXPECT_EQ(live.aircraft_count(), 1U);
    const auto j = live.snapshot_json({});
    ASSERT_EQ(j["aircraft"].size(), 1U);
    EXPECT_EQ(j["aircraft"][0]["icao24"], "4b8001");

    const auto cov = live.coverage_json();
    std::uint64_t total = 0;
    for (const auto& c : cov["cells"]) total += c[2].get<std::uint64_t>();
    EXPECT_EQ(total, 7U + 3U);  // one count per accepted fix
}

TEST(LiveHello, NamesSourceAndCitesOpenSky) {
    const auto j = live_hello_json("replay", "x10");
    EXPECT_EQ(j["type"], "hello");
    EXPECT_EQ(j["source"], "replay");
    EXPECT_EQ(j["label"], "x10");
    const auto attribution = j["attribution"].get<std::string>();
    EXPECT_NE(attribution.find("OpenSky Network"), std::string::npos);
    EXPECT_NE(attribution.find("IPSN 2014"), std::string::npos);
}
