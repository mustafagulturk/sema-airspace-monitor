#include <fstream>
#include <sstream>
#include <stdexcept>

#include <gtest/gtest.h>

#include "sema/state_vector.hpp"

namespace {

std::string read_fixture(const std::string& name) {
    std::ifstream in(std::string(SEMA_FIXTURE_DIR) + "/" + name);
    std::ostringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

const sema::StateVector* find(const sema::StatesSnapshot& s, const std::string& icao) {
    for (const auto& v : s.states)
        if (v.icao24 == icao) return &v;
    return nullptr;
}

}  // namespace

TEST(ParseStates, ParsesFixture) {
    const auto r = sema::parse_states(read_fixture("states_turkey_sample.json"));
    EXPECT_EQ(r.snapshot.time, 1762549200);
    EXPECT_EQ(r.snapshot.states.size(), 8u);
    EXPECT_EQ(r.skipped_rows, 2u);
}

TEST(ParseStates, MapsFieldsInOpenSkyOrder) {
    const auto r = sema::parse_states(read_fixture("states_turkey_sample.json"));
    const auto* s = find(r.snapshot, "4baa01");
    ASSERT_NE(s, nullptr);
    EXPECT_EQ(s->callsign, "THY1AB");
    EXPECT_EQ(s->origin_country, "Turkey");
    EXPECT_EQ(s->time_position, 1762549198);
    EXPECT_EQ(s->last_contact, 1762549199);
    EXPECT_DOUBLE_EQ(*s->longitude, 28.95);
    EXPECT_DOUBLE_EQ(*s->latitude, 41.01);
    EXPECT_DOUBLE_EQ(*s->baro_altitude, 3048.0);
    EXPECT_FALSE(s->on_ground);
    EXPECT_DOUBLE_EQ(*s->velocity, 180.5);
    EXPECT_DOUBLE_EQ(*s->true_track, 92.3);
    EXPECT_DOUBLE_EQ(*s->vertical_rate, -5.2);
    EXPECT_DOUBLE_EQ(*s->geo_altitude, 3100.0);
    EXPECT_EQ(s->squawk, "4521");
    EXPECT_EQ(s->position_source, 0);
    EXPECT_TRUE(s->has_position());
}

TEST(ParseStates, LowercasesIcaoAndHandlesNulls) {
    const auto r = sema::parse_states(read_fixture("states_turkey_sample.json"));
    const auto* s = find(r.snapshot, "4bac0f");
    ASSERT_NE(s, nullptr);
    EXPECT_TRUE(s->on_ground);
    EXPECT_FALSE(s->baro_altitude.has_value());
    EXPECT_FALSE(s->vertical_rate.has_value());
    EXPECT_TRUE(s->squawk.empty());
}

TEST(ParseStates, KeepsRowWithoutPosition) {
    const auto r = sema::parse_states(read_fixture("states_turkey_sample.json"));
    const auto* s = find(r.snapshot, "4b8e55");
    ASSERT_NE(s, nullptr);
    EXPECT_FALSE(s->has_position());
    EXPECT_TRUE(s->callsign.empty());
    EXPECT_FALSE(s->time_position.has_value());
}

TEST(ParseStates, NullStatesMeansEmpty) {
    const auto r = sema::parse_states(R"({"time": 100, "states": null})");
    EXPECT_EQ(r.snapshot.time, 100);
    EXPECT_TRUE(r.snapshot.states.empty());
    EXPECT_EQ(r.skipped_rows, 0u);
}

TEST(ParseStates, RejectsBadTopLevel) {
    EXPECT_THROW(sema::parse_states("not json"), std::runtime_error);
    EXPECT_THROW(sema::parse_states("[]"), std::runtime_error);
    EXPECT_THROW(sema::parse_states(R"({"states": []})"), std::runtime_error);
    EXPECT_THROW(sema::parse_states(R"({"time": 1, "states": {}})"), std::runtime_error);
}

TEST(ParseStates, SkipsOutOfRangeCoordinates) {
    const auto r = sema::parse_states(
        R"({"time": 1, "states": [["abcdef","X","Y",1,1,200.0,40.0,null,false,null,null,null,null,null,null,false,0]]})");
    EXPECT_TRUE(r.snapshot.states.empty());
    EXPECT_EQ(r.skipped_rows, 1u);
}
