#include <gtest/gtest.h>

#include <cmath>
#include <limits>

#include "sema/quality.hpp"

using namespace sema;

namespace {

StateVector sv(double lat, double lon, std::int64_t t) {
    StateVector s;
    s.icao24 = "4b8001";
    s.latitude = lat;
    s.longitude = lon;
    s.time_position = t;
    s.last_contact = t;
    s.baro_altitude = 10000.0;
    s.velocity = 230.0;
    s.true_track = 90.0;
    s.vertical_rate = 0.0;
    return s;
}

const QualityLimits kLimits;

}  // namespace

TEST(Quality, CleanReportPassesAllChecks) {
    const PreviousFix prev{1000.0, {39.0, 32.0}, 10000.0, 1000.0};
    // 60 s at 230 m/s due east is ~13.8 km, about 0.16 deg of longitude here.
    const auto m = check_measurement(sv(39.0, 32.16, 1060), &prev, kLimits);
    EXPECT_EQ(m.issues, kIssueNone);
    EXPECT_TRUE(m.position_usable());
    EXPECT_DOUBLE_EQ(m.time, 1060.0);
    ASSERT_TRUE(m.speed && m.altitude && m.vertical_rate);
}

TEST(Quality, UsesLastContactWhenTimePositionMissing) {
    auto s = sv(39.0, 32.0, 1000);
    s.time_position.reset();
    s.last_contact = 1005;
    EXPECT_DOUBLE_EQ(check_measurement(s, nullptr, kLimits).time, 1005.0);
}

TEST(Quality, RejectsNonFinitePosition) {
    auto s = sv(39.0, 32.0, 1000);
    s.latitude = std::numeric_limits<double>::quiet_NaN();
    const auto m = check_measurement(s, nullptr, kLimits);
    EXPECT_TRUE(m.issues & kIssueInvalidPosition);
    EXPECT_FALSE(m.position_usable());
}

TEST(Quality, ImpossibleSpeedDropsOnlyTheVelocity) {
    auto s = sv(39.0, 32.0, 1000);
    s.velocity = 950.0;
    const auto m = check_measurement(s, nullptr, kLimits);
    EXPECT_EQ(m.issues, kIssueImpossibleSpeed);
    EXPECT_TRUE(m.position_usable());
    EXPECT_FALSE(m.speed.has_value());
    EXPECT_FALSE(m.track_deg.has_value());
}

TEST(Quality, TeleportIsRejected) {
    const PreviousFix prev{1000.0, {39.0, 32.0}, 10000.0, 1000.0};
    // 1.5 deg of latitude (~167 km) in 60 s would be ~2,800 m/s.
    const auto m = check_measurement(sv(40.5, 32.0, 1060), &prev, kLimits);
    EXPECT_TRUE(m.issues & kIssueTeleport);
    EXPECT_FALSE(m.position_usable());
    // Same distance after a long gap is fine (~167 km in 10 min = 278 m/s).
    EXPECT_FALSE(check_measurement(sv(40.5, 32.0, 1600), &prev, kLimits).issues & kIssueTeleport);
}

TEST(Quality, SameTimestampAllowsOnlyNoise) {
    const PreviousFix prev{1000.0, {39.0, 32.0}, std::nullopt, 0.0};
    EXPECT_FALSE(check_measurement(sv(39.002, 32.0, 1000), &prev, kLimits).issues & kIssueTeleport);  // ~220 m
    EXPECT_TRUE(check_measurement(sv(39.02, 32.0, 1000), &prev, kLimits).issues & kIssueTeleport);    // ~2.2 km
}

TEST(Quality, BadAltitudeOutOfRangeOrJumping) {
    auto s = sv(39.0, 32.0, 1000);
    s.baro_altitude = 30000.0;
    auto m = check_measurement(s, nullptr, kLimits);
    EXPECT_EQ(m.issues, kIssueBadAltitude);
    EXPECT_FALSE(m.altitude.has_value());
    EXPECT_TRUE(m.position_usable());

    s.baro_altitude = -1000.0;
    EXPECT_EQ(check_measurement(s, nullptr, kLimits).issues, kIssueBadAltitude);

    // 10,000 m -> 1,000 m in 60 s is 150 m/s of descent.
    const PreviousFix prev{940.0, {39.0, 31.85}, 10000.0, 940.0};
    s.baro_altitude = 1000.0;
    EXPECT_TRUE(check_measurement(s, &prev, kLimits).issues & kIssueBadAltitude);
    // A normal 15 m/s descent is fine.
    s.baro_altitude = 9100.0;
    EXPECT_FALSE(check_measurement(s, &prev, kLimits).issues & kIssueBadAltitude);
}

TEST(Quality, OnGroundHasNoAltitude) {
    auto s = sv(39.0, 32.0, 1000);
    s.on_ground = true;
    s.baro_altitude = 30000.0;  // ignored on the ground
    const auto m = check_measurement(s, nullptr, kLimits);
    EXPECT_EQ(m.issues, kIssueNone);
    EXPECT_FALSE(m.altitude.has_value());
}

TEST(Quality, BadVerticalRate) {
    auto s = sv(39.0, 32.0, 1000);
    s.vertical_rate = 400.0;
    const auto m = check_measurement(s, nullptr, kLimits);
    EXPECT_EQ(m.issues, kIssueBadVerticalRate);
    EXPECT_FALSE(m.vertical_rate.has_value());
}

TEST(Quality, IssueNames) {
    EXPECT_EQ(issue_names(kIssueTeleport | kIssueBadAltitude), (std::vector<std::string>{"teleport", "bad_altitude"}));
    EXPECT_TRUE(issue_names(kIssueNone).empty());
}
