#include <gtest/gtest.h>

#include <nlohmann/json.hpp>

#include "sema/geo.hpp"
#include "sema/live.hpp"
#include "sema/pipeline.hpp"
#include "sema/privacy.hpp"

using namespace sema;

namespace {

constexpr std::array<std::uint64_t, 2> kKey{0x0123456789abcdefULL, 0xfedcba9876543210ULL};
constexpr double kNoon = 1762596000.0;  // 2025-11-08 13:00 Istanbul

StateVector sv(std::string icao, std::string callsign) {
    StateVector s;
    s.icao24 = std::move(icao);
    s.callsign = std::move(callsign);
    s.latitude = 39.9;
    s.longitude = 32.8;
    return s;
}

}  // namespace

TEST(IdentifierMode, ParsesTheThreeModes) {
    EXPECT_EQ(parse_identifier_mode("hidden"), IdentifierMode::Hidden);
    EXPECT_EQ(parse_identifier_mode("airline"), IdentifierMode::Airline);
    EXPECT_EQ(parse_identifier_mode("full"), IdentifierMode::Full);
    EXPECT_THROW((void)parse_identifier_mode("Full"), std::invalid_argument);
    EXPECT_THROW((void)parse_identifier_mode(""), std::invalid_argument);
}

TEST(AirlineCallsign, AcceptsFlightNumbersRejectsRegistrations) {
    EXPECT_TRUE(is_airline_callsign("THY7"));
    EXPECT_TRUE(is_airline_callsign("PGT2AB "));
    EXPECT_TRUE(is_airline_callsign("ajx1234"));
    EXPECT_FALSE(is_airline_callsign("TCABC"));   // Turkish registration
    EXPECT_FALSE(is_airline_callsign("N123AB"));  // US registration
    EXPECT_FALSE(is_airline_callsign("DEABC"));
    EXPECT_FALSE(is_airline_callsign("THY"));
    EXPECT_FALSE(is_airline_callsign(""));
    EXPECT_FALSE(is_airline_callsign("THY12345"));
}

TEST(IdentityMasker, PseudonymIsStableForADayAndHidesTheAddress) {
    const IdentityMasker m(IdentifierMode::Hidden, kKey);
    const auto a = m.icao24("4bb1a2", kNoon);
    EXPECT_EQ(a.size(), 9u);
    EXPECT_EQ(a[0], kPseudonymPrefix);
    EXPECT_EQ(a.find("4bb1a2"), std::string::npos);
    EXPECT_EQ(m.icao24("4BB1A2", kNoon + 3600), a);     // same local day, case does not matter
    EXPECT_NE(m.icao24("4bb1a2", kNoon + 86400), a);    // next day: unlinkable
    EXPECT_NE(m.icao24("4bb1a3", kNoon), a);
    const IdentityMasker other(IdentifierMode::Hidden, {1, 2});
    EXPECT_NE(other.icao24("4bb1a2", kNoon), a);       // another start-up key
}

TEST(IdentityMasker, CallsignsPerMode) {
    EXPECT_EQ(IdentityMasker(IdentifierMode::Hidden, kKey).callsign("THY7"), "");
    EXPECT_EQ(IdentityMasker(IdentifierMode::Airline, kKey).callsign("THY7"), "THY7");
    EXPECT_EQ(IdentityMasker(IdentifierMode::Airline, kKey).callsign("TCABC"), "");
    EXPECT_EQ(IdentityMasker(IdentifierMode::Full, kKey).callsign("TCABC"), "TCABC");
    EXPECT_EQ(IdentityMasker(IdentifierMode::Full, kKey).icao24("4bb1a2", kNoon), "4bb1a2");
}

TEST(IdentityMasker, MasksSnapshotsReplaysAndEvents) {
    const IdentityMasker m(IdentifierMode::Airline, kKey);
    nlohmann::json snap{{"type", "snapshot"},
                        {"data_time", kNoon},
                        {"aircraft", {{{"icao24", "4bb1a2"}, {"callsign", "TCABC"}, {"lat", 1}}}}};
    m.apply(snap);
    EXPECT_EQ(snap["aircraft"][0]["icao24"], m.icao24("4bb1a2", kNoon));
    EXPECT_EQ(snap["aircraft"][0]["callsign"], "");
    EXPECT_EQ(snap["aircraft"][0]["lat"], 1);

    nlohmann::json replay{{"from", kNoon}, {"aircraft", {{{"icao24", "4bb1a2"}, {"callsign", "THY7"}}}}};
    m.apply(replay);
    EXPECT_EQ(replay["aircraft"][0]["icao24"], snap["aircraft"][0]["icao24"]);  // same day, same pseudonym
    EXPECT_EQ(replay["aircraft"][0]["callsign"], "THY7");

    nlohmann::json events{{"type", "events"},
                          {"time", kNoon},
                          {"events",
                           {{{"key", "holding:4bb1a2:1762590000"},
                             {"kind", "holding"},
                             {"icao24", "4bb1a2"},
                             {"callsign", "TCABC"},
                             {"start", 1762590000.0}}}}};
    m.apply(events);
    const auto& e = events["events"][0];
    EXPECT_EQ(e["icao24"], snap["aircraft"][0]["icao24"]);
    EXPECT_EQ(e["callsign"], "");
    EXPECT_EQ(e["key"], "holding:" + e["icao24"].get<std::string>() + ":1762590000");
    EXPECT_EQ(events.dump().find("4bb1a2"), std::string::npos);
}

TEST(IdentityMasker, FullModeLeavesMessagesAlone) {
    const IdentityMasker m(IdentifierMode::Full, kKey);
    nlohmann::json snap{{"data_time", kNoon}, {"aircraft", {{{"icao24", "4bb1a2"}, {"callsign", "TCABC"}}}}};
    const auto before = snap;
    m.apply(snap);
    EXPECT_EQ(snap, before);
}

TEST(LiveHello, AnnouncesTheIdentifierMode) {
    EXPECT_EQ(live_hello_json("live", "", "hidden")["identifiers"], "hidden");
    EXPECT_EQ(live_hello_json("replay", "x")["identifiers"], "full");
}

TEST(OptOutList, ParsesAddressesCallsignsAndComments) {
    const auto list = OptOutList::parse(
        "# owners who asked not to be shown\n"
        "4BB1A2   # request 2026-01-02\n"
        "\n"
        "tcabc\n"
        "CALLSIGN:ABC123\n");
    EXPECT_EQ(list.size(), 3u);
    EXPECT_TRUE(list.contains(sv("4bb1a2", "")));
    EXPECT_TRUE(list.contains(sv("4b0000", "TCABC ")));
    EXPECT_TRUE(list.contains(sv("4b0000", "abc123")));
    EXPECT_FALSE(list.contains(sv("4b0000", "THY7")));
    EXPECT_FALSE(list.contains(sv("abc123", "")));  // CALLSIGN: entry is not an address
}

TEST(OptOutList, MissingFileIsEmpty) {
    EXPECT_EQ(OptOutList::load("/nonexistent/optout.txt").size(), 0u);
}

TEST(OptOutList, ShippedFileLoads) {
    EXPECT_EQ(OptOutList::load(std::filesystem::path(SEMA_DATA_DIR) / "optout.txt").size(), 0u);
}

TEST(FilterStates, DropsOptedOutAircraftFirst) {
    const auto area = MultiPolygon::from_geojson(
        R"({"type":"Polygon","coordinates":[[[26,36],[45,36],[45,42],[26,42],[26,36]]]})");
    const MilitaryFilter military(MilitaryRules::from_json("{}"), nullptr);
    const auto opt_out = OptOutList::parse("4bb1a2\n");
    FilterStats stats;
    const auto kept =
        filter_states({sv("4bb1a2", "TCABC"), sv("4b0001", "THY7")}, area, military, stats, &opt_out);
    ASSERT_EQ(kept.size(), 1u);
    EXPECT_EQ(kept[0].icao24, "4b0001");
    EXPECT_EQ(stats.dropped_opt_out, 1u);
    EXPECT_EQ(stats.dropped_military, 0u);
}
