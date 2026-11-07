#include <gtest/gtest.h>

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <unistd.h>

#include "sema/snapshot_io.hpp"

using namespace sema;

namespace {

StatesSnapshot sample() {
    StatesSnapshot snap;
    snap.time = 1762549200;
    StateVector a;
    a.icao24 = "4b8001";
    a.callsign = "SMX101";
    a.origin_country = "Turkey";
    a.time_position = 1762549198;
    a.last_contact = 1762549199;
    a.latitude = 41.01;
    a.longitude = 28.95;
    a.baro_altitude = 3048.0;
    a.velocity = 180.5;
    a.true_track = 92.3;
    a.vertical_rate = -5.2;
    a.geo_altitude = 3100.0;
    a.squawk = "4521";
    StateVector b;
    b.icao24 = "4b8002";
    b.last_contact = 1762549150;
    b.on_ground = true;
    b.position_source = 2;
    snap.states = {a, b};
    return snap;
}

}  // namespace

TEST(SnapshotIo, OpenSkyJsonRoundTrips) {
    const auto snap = sample();
    const auto r = parse_states(to_opensky_json(snap));
    EXPECT_EQ(r.skipped_rows, 0U);
    ASSERT_EQ(r.snapshot.states.size(), 2U);
    EXPECT_EQ(r.snapshot.time, snap.time);
    const auto& a = r.snapshot.states[0];
    EXPECT_EQ(a.icao24, "4b8001");
    EXPECT_EQ(a.callsign, "SMX101");
    EXPECT_EQ(a.time_position, 1762549198);
    EXPECT_DOUBLE_EQ(*a.latitude, 41.01);
    EXPECT_DOUBLE_EQ(*a.vertical_rate, -5.2);
    EXPECT_EQ(a.squawk, "4521");
    const auto& b = r.snapshot.states[1];
    EXPECT_FALSE(b.has_position());
    EXPECT_FALSE(b.time_position.has_value());
    EXPECT_TRUE(b.on_ground);
    EXPECT_EQ(b.position_source, 2);
}

TEST(SnapshotIo, WriterAppendsAndReaderReadsBack) {
    const auto path = std::filesystem::temp_directory_path() / ("sema_snapio_" + std::to_string(::getpid()) + ".jsonl");
    std::filesystem::remove(path);
    {
        SnapshotWriter w(path);
        w.write(sample());
    }
    {
        SnapshotWriter w(path);  // appends
        auto s = sample();
        s.time += 65;
        w.write(s);
    }
    {
        std::ofstream(path, std::ios::app) << "\n";  // blank lines are ignored
    }
    SnapshotReader r(path);
    auto first = r.next();
    auto second = r.next();
    ASSERT_TRUE(first && second);
    EXPECT_EQ(first->time, 1762549200);
    EXPECT_EQ(second->time, 1762549265);
    EXPECT_FALSE(r.next().has_value());
    std::filesystem::remove(path);
}

TEST(SnapshotIo, ReaderNamesTheBadLine) {
    const auto path = std::filesystem::temp_directory_path() / ("sema_snapio_bad_" + std::to_string(::getpid()) + ".jsonl");
    std::ofstream(path) << to_opensky_json(sample()) << "\n{not json\n";
    SnapshotReader r(path);
    EXPECT_TRUE(r.next().has_value());
    try {
        (void)r.next();
        FAIL() << "expected an exception";
    } catch (const std::runtime_error& e) {
        EXPECT_NE(std::string(e.what()).find("line 2"), std::string::npos);
    }
    std::filesystem::remove(path);
}
