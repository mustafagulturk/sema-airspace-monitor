#include <gtest/gtest.h>

#include <algorithm>
#include <cmath>
#include <optional>
#include <string>
#include <vector>

#include "sema/flights.hpp"
#include "sema/tracker.hpp"

using namespace sema;

namespace {

constexpr std::int64_t kT0 = 1'762'549'200;
const LatLon kIst{41.27487, 28.73213};  // LTFM, elevation ~99 m
const LatLon kEsb{40.12810, 32.99509};  // LTAC, elevation ~953 m

AirportIndex test_airports() {
    return AirportIndex({{"LTFM", "IST", "İstanbul Airport", "Istanbul", kIst, 99.0},
                         {"LTAC", "ESB", "Esenboğa International Airport", "Ankara", kEsb, 953.0}});
}

LatLon lerp(LatLon a, LatLon b, double k) { return {a.lat + (b.lat - a.lat) * k, a.lon + (b.lon - a.lon) * k}; }

double bearing_deg(LatLon a, LatLon b) {
    const double d = 3.14159265358979323846 / 180.0;
    const double y = std::sin((b.lon - a.lon) * d) * std::cos(b.lat * d);
    const double x = std::cos(a.lat * d) * std::sin(b.lat * d) - std::sin(a.lat * d) * std::cos(b.lat * d) * std::cos((b.lon - a.lon) * d);
    return std::fmod(std::atan2(y, x) / d + 360.0, 360.0);
}

struct Sample {
    LatLon pos;
    std::optional<double> alt;
    double vrate;
    bool ground;
};

// Runs snapshots `dt` seconds apart through a tracker + flight log, then empty
// snapshots until the track ends. Returns every finished flight.
std::vector<FlightRecord> fly(const std::vector<Sample>& samples, double speed, const AirportIndex& airports,
                              const std::string& icao = "4b8001", int dt = 60) {
    Tracker tracker;
    FlightLog log(&airports);
    std::vector<FlightRecord> out;
    std::int64_t t = kT0;
    for (std::size_t i = 0; i < samples.size(); ++i) {
        const auto& p = samples[i];
        StateVector s;
        s.icao24 = icao;
        s.callsign = "THY7";
        s.origin_country = "Turkey";
        s.time_position = t;
        s.last_contact = t;
        s.latitude = p.pos.lat;
        s.longitude = p.pos.lon;
        if (!p.ground) s.baro_altitude = p.alt;
        s.on_ground = p.ground;
        s.velocity = p.ground ? 8.0 : speed;
        const auto& next = samples[std::min(i + 1, samples.size() - 1)];
        const auto& prev = samples[i == 0 ? 0 : i - 1];
        s.true_track = bearing_deg(i + 1 < samples.size() ? p.pos : prev.pos, i + 1 < samples.size() ? next.pos : p.pos);
        s.vertical_rate = p.vrate;
        StatesSnapshot snap{t, {s}};
        const auto r = tracker.process(snap);
        for (auto& f : log.update(snap, r, tracker)) out.push_back(std::move(f));
        t += dt;
    }
    for (int i = 0; i < 15; ++i) {
        StatesSnapshot snap{t, {}};
        const auto r = tracker.process(snap);
        for (auto& f : log.update(snap, r, tracker)) out.push_back(std::move(f));
        t += dt;
    }
    EXPECT_EQ(log.open_count(), 0U);
    return out;
}

// Istanbul -> Ankara: on the ground at IST, climb, cruise, descend into ESB.
std::vector<Sample> ist_to_esb() {
    std::vector<Sample> v;
    const int n = 32;
    for (int i = 0; i <= n; ++i) {
        const double k = static_cast<double>(i) / n;
        Sample s{lerp(kIst, kEsb, k), std::nullopt, 0.0, false};
        if (i == 0) {
            s.ground = true;
        } else if (i < 8) {
            s.alt = 99.0 + 1400.0 * i;
            s.vrate = 20.0;
        } else if (i > n - 8) {
            s.alt = 953.0 + 1300.0 * (n - i) + 100.0;
            s.vrate = -20.0;
        } else {
            s.alt = 11000.0;
        }
        v.push_back(s);
    }
    return v;
}

}  // namespace

TEST(AirportIndex, NearestWithinRadius) {
    const auto a = test_airports();
    ASSERT_NE(a.nearest({41.28, 28.75}, 12000.0), nullptr);
    EXPECT_EQ(a.nearest({41.28, 28.75}, 12000.0)->icao, "LTFM");
    EXPECT_EQ(a.nearest({39.0, 30.0}, 12000.0), nullptr);
    EXPECT_EQ(a.find("LTAC")->iata, "ESB");
    EXPECT_EQ(a.find("XXXX"), nullptr);
}

TEST(AirportIndex, LoadsShippedListWithoutAirBases) {
    const auto a = AirportIndex::load(std::string(SEMA_DATA_DIR) + "/airports_tr.json");
    EXPECT_GE(a.size(), 40U);
    ASSERT_NE(a.find("LTFM"), nullptr);
    ASSERT_NE(a.find("LTFJ"), nullptr);
    ASSERT_NE(a.find("LTAI"), nullptr);
    // Air bases are never in the list (ethics rule: no base monitoring).
    for (const char* base : {"LTAG", "LTAE", "LTBI", "LTAD", "LTBL", "LTBX"}) EXPECT_EQ(a.find(base), nullptr) << base;
    for (const auto& ap : a.all()) {
        EXPECT_EQ(ap.name.find("Air Base"), std::string::npos) << ap.name;
        EXPECT_GT(ap.pos.lat, 35.0);
        EXPECT_LT(ap.pos.lat, 43.0);
    }
}

TEST(CorridorCell, RoundTripsTheSouthWestCorner) {
    const auto c = corridor_cell({41.27, 28.73});
    const auto o = corridor_cell_origin(c);
    EXPECT_DOUBLE_EQ(o.lat, 41.25);
    EXPECT_DOUBLE_EQ(o.lon, 28.5);
    EXPECT_NE(corridor_cell({41.24, 28.73}), c);
}

TEST(FlightLog, DetectsDepartureAndArrival) {
    const auto airports = test_airports();
    const auto flights = fly(ist_to_esb(), 200.0, airports);
    ASSERT_EQ(flights.size(), 1U);
    const auto& f = flights[0];
    EXPECT_EQ(f.icao24, "4b8001");
    EXPECT_EQ(f.callsign, "THY7");
    EXPECT_EQ(f.departure, "LTFM");
    EXPECT_EQ(f.arrival, "LTAC");
    EXPECT_EQ(f.end_reason, "landed");
    EXPECT_EQ(f.fixes, 33);
    EXPECT_TRUE(f.airborne);
    EXPECT_TRUE(f.counts());
    EXPECT_NEAR(f.max_altitude_m.value_or(0), 11000.0, 1.0);
    EXPECT_DOUBLE_EQ(f.first_seen, static_cast<double>(kT0));
    EXPECT_DOUBLE_EQ(f.last_seen, static_cast<double>(kT0 + 32 * 60));
    // The route's cells are all there, without gaps between ~60 s fixes.
    EXPECT_TRUE(std::is_sorted(f.cells.begin(), f.cells.end()));
    EXPECT_TRUE(std::binary_search(f.cells.begin(), f.cells.end(), corridor_cell(lerp(kIst, kEsb, 0.5))));
    EXPECT_TRUE(std::binary_search(f.cells.begin(), f.cells.end(), corridor_cell(kEsb)));
    EXPECT_GE(f.cells.size(), 18U);  // ~4.3 deg of longitude at 0.25 deg
}

TEST(FlightLog, OverflightHasNoAirports) {
    const auto airports = test_airports();
    std::vector<Sample> v;
    for (int i = 0; i <= 20; ++i) v.push_back({lerp({41.6, 27.5}, {41.6, 31.5}, i / 20.0), 11000.0, 0.0, false});
    const auto flights = fly(v, 230.0, airports);
    ASSERT_EQ(flights.size(), 1U);
    EXPECT_EQ(flights[0].departure, "");
    EXPECT_EQ(flights[0].arrival, "");
    EXPECT_EQ(flights[0].end_reason, "signal_lost");
}

TEST(FlightLog, LowDescendingFirstFixIsAnApproachNotADeparture) {
    const auto airports = test_airports();
    // Picked up already on final approach into ESB.
    std::vector<Sample> v;
    for (int i = 0; i < 4; ++i) {
        v.push_back({lerp({40.05, 32.70}, kEsb, i / 3.0), 953.0 + 1200.0 - 350.0 * i, -6.0, false});
    }
    const auto flights = fly(v, 80.0, airports);
    ASSERT_EQ(flights.size(), 1U);
    EXPECT_EQ(flights[0].departure, "");
    EXPECT_EQ(flights[0].arrival, "LTAC");
}

TEST(FlightLog, ClimbingLastFixIsNotAnArrival) {
    const auto airports = test_airports();
    std::vector<Sample> v;
    for (int i = 0; i < 4; ++i) v.push_back({lerp(kEsb, {40.20, 33.20}, i / 3.0), 953.0 + 200.0 + 300.0 * i, 8.0, false});
    const auto flights = fly(v, 90.0, airports);
    ASSERT_EQ(flights.size(), 1U);
    EXPECT_EQ(flights[0].departure, "LTAC");
    EXPECT_EQ(flights[0].arrival, "");
}

TEST(FlightLog, TaxiingOnlyIsNotAFlight) {
    const auto airports = test_airports();
    std::vector<Sample> v;
    for (int i = 0; i < 5; ++i) v.push_back({{kIst.lat + 0.001 * i, kIst.lon}, std::nullopt, 0.0, true});
    const auto flights = fly(v, 8.0, airports);
    ASSERT_EQ(flights.size(), 1U);
    EXPECT_FALSE(flights[0].airborne);
    EXPECT_FALSE(flights[0].counts());
}

TEST(FlightLog, FlushClosesOpenFlightsAndOpenFlightsPeeks) {
    const auto airports = test_airports();
    Tracker tracker;
    FlightLog log(&airports);
    for (int i = 0; i < 3; ++i) {
        StateVector s;
        s.icao24 = "4b8002";
        s.time_position = kT0 + 60 * i;
        s.last_contact = kT0 + 60 * i;
        s.latitude = 39.0;
        s.longitude = 32.0 + 0.12 * i;
        s.baro_altitude = 10000.0;
        s.velocity = 230.0;
        s.true_track = 90.0;
        StatesSnapshot snap{kT0 + 60 * i, {s}};
        const auto r = tracker.process(snap);
        EXPECT_TRUE(log.update(snap, r, tracker).empty());
    }
    EXPECT_EQ(log.open_count(), 1U);
    const auto peek = log.open_flights();
    ASSERT_EQ(peek.size(), 1U);
    EXPECT_EQ(peek[0].fixes, 3);
    EXPECT_EQ(log.open_count(), 1U);
    const auto done = log.flush();
    ASSERT_EQ(done.size(), 1U);
    EXPECT_EQ(done[0].end_reason, "open");
    EXPECT_EQ(log.open_count(), 0U);
}

TEST(FlightLog, RepeatedReportIsCountedOnce) {
    const auto airports = test_airports();
    Tracker tracker;
    FlightLog log(&airports);
    StateVector s;
    s.icao24 = "4b8003";
    s.time_position = kT0;
    s.last_contact = kT0;
    s.latitude = 39.0;
    s.longitude = 32.0;
    s.baro_altitude = 10000.0;
    for (int i = 0; i < 3; ++i) {
        StatesSnapshot snap{kT0 + 10 * i, {s}};  // OpenSky repeating the same fix
        const auto r = tracker.process(snap);
        log.update(snap, r, tracker);
    }
    EXPECT_EQ(log.open_flights().at(0).fixes, 1);
}

TEST(FlightRecord, JsonRoundTrip) {
    FlightRecord f;
    f.track_id = 7;
    f.icao24 = "4b8001";
    f.callsign = "THY7";
    f.first_seen = 100;
    f.last_seen = 200;
    f.first_pos = {41.0, 29.0};
    f.last_pos = {40.0, 33.0};
    f.max_altitude_m = 11000.0;
    f.fixes = 5;
    f.airborne = true;
    f.departure = "LTFM";
    f.end_reason = "landed";
    f.cells = {1, 2, 3};
    const auto g = FlightRecord::from_json(f.to_json());
    EXPECT_EQ(g.icao24, f.icao24);
    EXPECT_EQ(g.departure, "LTFM");
    EXPECT_EQ(g.arrival, "");
    EXPECT_EQ(g.cells, f.cells);
    EXPECT_FALSE(g.min_altitude_m.has_value());
    EXPECT_DOUBLE_EQ(g.max_altitude_m.value_or(0), 11000.0);
}
