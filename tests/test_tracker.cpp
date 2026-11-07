#include <gtest/gtest.h>

#include <cmath>
#include <string>

#include "sema/geo.hpp"
#include "sema/snapshot_io.hpp"
#include "sema/tracker.hpp"

using namespace sema;

namespace {

constexpr double kPi = 3.14159265358979323846;
constexpr std::int64_t kT0 = 1'762'549'200;

// A straight, level flight at constant speed: position at time t. The truth is
// a straight line in the tangent plane at `start`, which drifts from what an
// aircraft holding a constant track over the curved Earth does by ~10 m per
// 30 km; tolerances below allow for that.
struct Flight {
    std::string icao24 = "4b8001";
    LatLon start{39.0, 32.0};
    double speed = 230.0;
    double track_deg = 60.0;
    double altitude = 10000.0;

    [[nodiscard]] LatLon pos(double t) const {
        const double d = speed * (t - static_cast<double>(kT0));
        const double a = track_deg * kPi / 180.0;
        return from_local(start, {d * std::sin(a), d * std::cos(a)});
    }

    [[nodiscard]] StateVector at(std::int64_t t) const {
        StateVector s;
        s.icao24 = icao24;
        s.callsign = "SMX101";
        const LatLon p = pos(static_cast<double>(t));
        s.latitude = p.lat;
        s.longitude = p.lon;
        s.time_position = t;
        s.last_contact = t;
        s.baro_altitude = altitude;
        s.velocity = speed;
        s.true_track = track_deg;
        s.vertical_rate = 0.0;
        return s;
    }
};

StatesSnapshot snap(std::int64_t t, std::vector<StateVector> states) { return {t, std::move(states)}; }

}  // namespace

TEST(Tracker, CreatesConfirmsAndPredicts) {
    Tracker tr;
    const Flight f;
    auto r = tr.process(snap(kT0, {f.at(kT0)}));
    EXPECT_EQ(r.created, 1U);
    EXPECT_EQ(r.tracked, 1U);
    EXPECT_EQ(r.confirmed, 0U);  // tentative after one fix
    r = tr.process(snap(kT0 + 60, {f.at(kT0 + 60)}));
    EXPECT_EQ(r.updates, 1U);
    EXPECT_EQ(r.confirmed, 1U);
    tr.process(snap(kT0 + 120, {f.at(kT0 + 120)}));

    const auto v = tr.track_at("4b8001", static_cast<double>(kT0 + 150));
    ASSERT_TRUE(v.has_value());
    EXPECT_EQ(v->state, TrackState::Confirmed);
    EXPECT_EQ(v->callsign, "SMX101");
    EXPECT_EQ(v->updates, 3);
    EXPECT_LT(haversine_m(v->pos, f.pos(kT0 + 150)), 20.0);
    EXPECT_NEAR(v->speed_ms, 230.0, 0.5);
    EXPECT_NEAR(v->track_deg, 60.0, 0.5);
    ASSERT_TRUE(v->altitude_m.has_value());
    EXPECT_NEAR(*v->altitude_m, 10000.0, 1.0);
    ASSERT_TRUE(v->turn_rate_dps.has_value());
    EXPECT_NEAR(*v->turn_rate_dps, 0.0, 0.05);

    const auto& m = tr.metrics();
    EXPECT_EQ(m.tracks_created, 1U);
    EXPECT_EQ(m.tracks_confirmed, 1U);
    // The third fix is the first one predicted by a confirmed track.
    EXPECT_EQ(m.cv_error_m.count(), 1U);
    EXPECT_LT(m.cv_error_m.max(), 20.0);
    EXPECT_LT(m.ct_error_m.max(), 20.0);
    EXPECT_LT(m.dead_reckoning_error_m.max(), 20.0);
    EXPECT_NEAR(m.hold_error_m.max(), 230.0 * 60.0, 0.003 * 230.0 * 60.0);
    EXPECT_EQ(m.processing_us_per_snapshot.count(), 3U);
}

TEST(Tracker, RepeatedTimestampIsStaleAndStartsCoasting) {
    Tracker tr;
    const Flight f;
    tr.process(snap(kT0, {f.at(kT0)}));
    tr.process(snap(kT0 + 60, {f.at(kT0 + 60)}));
    const auto r = tr.process(snap(kT0 + 120, {f.at(kT0 + 60)}));  // OpenSky repeats the old fix
    EXPECT_EQ(r.stale, 1U);
    EXPECT_EQ(r.updates, 0U);
    EXPECT_EQ(r.coasting, 1U);
    EXPECT_EQ(tr.metrics().coasting_started, 1U);
    EXPECT_EQ(tr.metrics().signal_losses(), 0U);  // not yet known to be a loss
}

TEST(Tracker, BridgesShortSignalLoss) {
    Tracker tr;
    const Flight f;
    tr.process(snap(kT0, {f.at(kT0)}));
    tr.process(snap(kT0 + 60, {f.at(kT0 + 60)}));
    const auto id = tr.track_at("4b8001", static_cast<double>(kT0 + 60))->id;
    for (int k = 2; k <= 5; ++k) tr.process(snap(kT0 + 60 * k, {}));  // 4 empty snapshots
    EXPECT_EQ(tr.track_at("4b8001", static_cast<double>(kT0 + 300))->state, TrackState::Coasting);
    const auto r = tr.process(snap(kT0 + 360, {f.at(kT0 + 360)}));
    EXPECT_EQ(r.reacquired, 1U);
    EXPECT_EQ(tr.track_at("4b8001", static_cast<double>(kT0 + 360))->id, id);
    EXPECT_EQ(tr.track_at("4b8001", static_cast<double>(kT0 + 360))->state, TrackState::Confirmed);
    const auto& m = tr.metrics();
    EXPECT_EQ(m.reacquisitions, 1U);
    EXPECT_EQ(m.signal_losses(), 1U);
    EXPECT_EQ(m.track_breaks, 0U);
    EXPECT_DOUBLE_EQ(m.reacquire_gap_s.max(), 300.0);
    // A 300 s straight-line prediction lands in the 180-600 s bucket.
    EXPECT_EQ(m.cv_error_by_horizon_m[horizon_bucket(300.0)].count(), 1U);
    EXPECT_LT(m.cv_error_m.max(), 100.0);  // 69 km of extrapolation
}

TEST(Tracker, LongSignalLossEndsTrackAndCountsBreak) {
    Tracker tr;
    const Flight f;
    tr.process(snap(kT0, {f.at(kT0)}));
    tr.process(snap(kT0 + 60, {f.at(kT0 + 60)}));
    const auto first_id = tr.track_at("4b8001", static_cast<double>(kT0))->id;
    SnapshotReport r;
    std::int64_t t = kT0 + 120;
    for (; t <= kT0 + 60 + 660; t += 60) {
        r = tr.process(snap(t, {}));
        if (!r.ended.empty()) break;
    }
    ASSERT_EQ(r.ended.size(), 1U);
    EXPECT_EQ(r.ended[0].reason, EndReason::SignalLost);
    EXPECT_EQ(tr.size(), 0U);

    r = tr.process(snap(t + 600, {f.at(t + 600)}));
    EXPECT_EQ(r.created, 1U);
    EXPECT_NE(tr.track_at("4b8001", static_cast<double>(t + 600))->id, first_id);
    const auto& m = tr.metrics();
    EXPECT_EQ(m.track_breaks, 1U);
    EXPECT_EQ(m.signal_losses(), 1U);
    EXPECT_DOUBLE_EQ(m.break_rate(), 0.5);  // 1 break / 2 tracks created
}

TEST(Tracker, ReturnAfterBreakWindowIsNotABreak) {
    TrackerConfig cfg;
    cfg.terminate_after_s = 120;
    cfg.break_window_s = 300;
    Tracker tr(cfg);
    const Flight f;
    tr.process(snap(kT0, {f.at(kT0)}));
    tr.process(snap(kT0 + 60, {f.at(kT0 + 60)}));
    for (int k = 2; k <= 4; ++k) tr.process(snap(kT0 + 60 * k, {}));
    EXPECT_EQ(tr.size(), 0U);
    for (int k = 5; k <= 12; ++k) tr.process(snap(kT0 + 60 * k, {}));
    tr.process(snap(kT0 + 780, {f.at(kT0 + 780)}));
    EXPECT_EQ(tr.metrics().track_breaks, 0U);
}

TEST(Tracker, EndReasonLeftAreaAndLanded) {
    // Area: everything south of 39.2 N. The flight heads north-east out of it.
    Tracker tr({}, [](LatLon p) { return p.lat < 39.2; });
    Flight out;
    out.icao24 = "4b8001";
    Flight landed;
    landed.icao24 = "4b8002";
    landed.start = {38.0, 30.0};
    landed.speed = 10.0;

    auto ground = [&](std::int64_t t) {
        auto s = landed.at(t);
        s.on_ground = true;
        s.baro_altitude.reset();
        return s;
    };
    // The flight's last fix is ~0.05 deg south of the edge; 90 s later it is out.
    std::int64_t t = kT0;
    for (; out.pos(static_cast<double>(t + 60)).lat < 39.2; t += 60) {
        tr.process(snap(t, {out.at(t), ground(t)}));
    }
    std::vector<EndedTrack> ended;
    for (std::int64_t k = t + 60; k <= t + 60 * 12; k += 60) {
        const auto r = tr.process(snap(k, {}));
        ended.insert(ended.end(), r.ended.begin(), r.ended.end());
    }
    ASSERT_EQ(ended.size(), 2U);
    for (const auto& e : ended) {
        EXPECT_EQ(e.reason, e.icao24 == "4b8001" ? EndReason::LeftArea : EndReason::Landed) << e.icao24;
    }
    EXPECT_EQ(tr.metrics().signal_losses(), 0U);
}

TEST(Tracker, SingleTeleportIsRejectedAndTrackKept) {
    Tracker tr;
    const Flight f;
    tr.process(snap(kT0, {f.at(kT0)}));
    tr.process(snap(kT0 + 60, {f.at(kT0 + 60)}));
    auto bad = f.at(kT0 + 120);
    *bad.latitude += 1.5;
    auto r = tr.process(snap(kT0 + 120, {bad}));
    EXPECT_EQ(r.rejected, 1U);
    EXPECT_EQ(r.updates, 0U);
    r = tr.process(snap(kT0 + 180, {f.at(kT0 + 180)}));
    EXPECT_EQ(r.updates, 1U);
    EXPECT_EQ(r.reacquired, 1U);
    EXPECT_EQ(tr.metrics().teleport, 1U);
    EXPECT_EQ(tr.metrics().ended_by_reason[static_cast<std::size_t>(EndReason::Restarted)], 0U);
    EXPECT_LT(haversine_m(tr.track_at("4b8001", static_cast<double>(kT0 + 180))->pos, f.pos(kT0 + 180)), 5.0);
}

TEST(Tracker, ConsistentJumpsRestartTheTrack) {
    Tracker tr;
    const Flight f;
    Flight other = f;
    other.start = {37.0, 33.0};  // same address, different aircraft
    tr.process(snap(kT0, {f.at(kT0)}));
    tr.process(snap(kT0 + 60, {f.at(kT0 + 60)}));
    const auto id = tr.track_at("4b8001", static_cast<double>(kT0))->id;
    tr.process(snap(kT0 + 120, {other.at(kT0 + 120)}));
    const auto r = tr.process(snap(kT0 + 180, {other.at(kT0 + 180)}));
    ASSERT_EQ(r.ended.size(), 1U);
    EXPECT_EQ(r.ended[0].reason, EndReason::Restarted);
    EXPECT_EQ(r.created, 1U);
    const auto v = tr.track_at("4b8001", static_cast<double>(kT0 + 180));
    EXPECT_NE(v->id, id);
    EXPECT_LT(haversine_m(v->pos, other.pos(kT0 + 180)), 5.0);
    EXPECT_EQ(tr.metrics().teleport, 2U);
    EXPECT_EQ(tr.metrics().track_breaks, 0U);  // a restart is not a break
}

TEST(Tracker, BadFieldsAreDroppedButPositionUsed) {
    Tracker tr;
    const Flight f;
    tr.process(snap(kT0, {f.at(kT0)}));
    auto s = f.at(kT0 + 60);
    s.baro_altitude = 30000.0;
    s.velocity = 950.0;
    s.vertical_rate = 400.0;
    const auto r = tr.process(snap(kT0 + 60, {s}));
    EXPECT_EQ(r.updates, 1U);
    const auto& m = tr.metrics();
    EXPECT_EQ(m.bad_altitude, 1U);
    EXPECT_EQ(m.impossible_speed, 1U);
    EXPECT_EQ(m.bad_vertical_rate, 1U);
    EXPECT_NEAR(*tr.track_at("4b8001", static_cast<double>(kT0 + 60))->altitude_m, 10000.0, 1.0);
}

TEST(Tracker, AircraftWithoutPositionIsIgnored) {
    Tracker tr;
    StateVector s;
    s.icao24 = "4b8001";
    s.last_contact = kT0;
    const auto r = tr.process(snap(kT0, {s}));
    EXPECT_EQ(r.measurements, 0U);
    EXPECT_EQ(tr.size(), 0U);
}

TEST(Tracker, TracksAtIsSortedById) {
    Tracker tr;
    Flight a;
    Flight b;
    b.icao24 = "4b8002";
    b.start = {38.0, 30.0};
    tr.process(snap(kT0, {b.at(kT0), a.at(kT0)}));
    const auto v = tr.tracks_at(static_cast<double>(kT0 + 30));
    ASSERT_EQ(v.size(), 2U);
    EXPECT_LT(v[0].id, v[1].id);
    EXPECT_EQ(v[0].icao24, "4b8002");
}

// End-to-end on the synthetic recording (see tools/make_replay_fixture.py),
// which has a known set of injected events.
TEST(Tracker, ReplaySyntheticRecordingFindsInjectedEvents) {
    const auto area = MultiPolygon::load_geojson(std::string(SEMA_DATA_DIR) + "/turkey_boundary.geojson");
    Tracker tr({}, [&area](LatLon p) { return area.contains(p); });
    SnapshotReader reader(std::string(SEMA_FIXTURE_DIR) + "/replay_synthetic.jsonl");
    std::size_t snapshots = 0;
    while (auto s = reader.next()) {
        tr.process(*s);
        ++snapshots;
    }
    const auto& m = tr.metrics();
    EXPECT_EQ(snapshots, 80U);
    EXPECT_EQ(m.teleport, 3U);  // one outlier + two reports that restart a duplicated address
    EXPECT_EQ(m.impossible_speed, 1U);
    EXPECT_EQ(m.bad_altitude, 1U);
    EXPECT_EQ(m.bad_vertical_rate, 1U);
    EXPECT_EQ(m.ended_by_reason[static_cast<std::size_t>(EndReason::Restarted)], 1U);
    EXPECT_EQ(m.ended_by_reason[static_cast<std::size_t>(EndReason::SignalLost)], 1U);
    EXPECT_EQ(m.track_breaks, 1U);
    EXPECT_GE(m.ended_by_reason[static_cast<std::size_t>(EndReason::LeftArea)], 1U);
    EXPECT_GE(m.ended_by_reason[static_cast<std::size_t>(EndReason::Landed)], 10U);
    EXPECT_GE(m.reacquire_gap_s.max(), 390.0);  // the 6.5 min outage was bridged
    // Filters predict far better than assuming the aircraft stood still, and
    // the median error is dominated by OpenSky's one-second timestamps.
    EXPECT_LT(m.cv_error_m.percentile(50), 300.0);
    EXPECT_LT(m.ct_error_m.percentile(50), 300.0);
    EXPECT_GT(m.hold_error_m.percentile(50), 5000.0);
    EXPECT_LT(m.altitude_error_m.percentile(50), 20.0);
}
