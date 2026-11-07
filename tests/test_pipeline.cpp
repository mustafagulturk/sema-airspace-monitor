#include <gtest/gtest.h>

#include <algorithm>

#include "sema/pipeline.hpp"

using namespace sema;

namespace {

// Square 38..40 N, 30..32 E with a hole at 38.9..39.1 N, 30.9..31.1 E.
MultiPolygon square_area() {
    MultiPolygon::Polygon p;
    p.outer = {{38, 30}, {38, 32}, {40, 32}, {40, 30}, {38, 30}};
    p.holes = {{{38.9, 30.9}, {38.9, 31.1}, {39.1, 31.1}, {39.1, 30.9}, {38.9, 30.9}}};
    return MultiPolygon({p});
}

MilitaryFilter make_filter() {
    return MilitaryFilter(MilitaryRules::from_json(R"({
        "icao_blocks": [{"from": "adf7c8", "to": "afffff"}],
        "callsign_patterns": ["TUAF[0-9]+[A-Z]*"]
    })"), nullptr);
}

StateVector at(std::string icao, std::string cs, std::optional<double> lat, std::optional<double> lon) {
    StateVector s;
    s.icao24 = std::move(icao);
    s.callsign = std::move(cs);
    s.latitude = lat;
    s.longitude = lon;
    return s;
}

}  // namespace

TEST(Pipeline, KeepsCivilInsideAndCountsEveryDropReason) {
    const auto area = square_area();
    const auto filter = make_filter();
    std::vector<StateVector> in = {
        at("4b8001", "THY1AB", 39.5, 31.5),       // kept
        at("4b8002", "PGT12", 38.5, 30.5),        // kept
        at("4b8003", "AJA4D", 41.0, 31.0),        // outside (north)
        at("4b8004", "RYR12", 39.0, 31.0),        // inside the hole -> outside
        at("4b8005", "SXS1", std::nullopt, 31.0), // no position
        at("ae1234", "RCH1", 39.5, 31.5),         // military (icao block), inside
        at("4b8006", "TUAF12", 39.5, 31.5),       // military (callsign), inside
        at("ae5678", "", std::nullopt, std::nullopt),  // military without position
    };
    FilterStats stats;
    const auto out = filter_states(std::move(in), area, filter, stats);

    EXPECT_EQ(stats.total, 8u);
    EXPECT_EQ(stats.kept, 2u);
    EXPECT_EQ(stats.dropped_outside, 2u);
    EXPECT_EQ(stats.dropped_no_position, 1u);
    EXPECT_EQ(stats.dropped_military, 3u);
    EXPECT_EQ(stats.total, stats.kept + stats.dropped_outside + stats.dropped_no_position + stats.dropped_military);

    ASSERT_EQ(out.size(), 2u);
    EXPECT_EQ(out[0].icao24, "4b8001");
    EXPECT_EQ(out[1].icao24, "4b8002");
}

TEST(Pipeline, MilitaryNeverInOutputEvenWhenInside) {
    const auto area = square_area();
    const auto filter = make_filter();
    std::vector<StateVector> in;
    for (int i = 0; i < 50; ++i) {
        in.push_back(at("ae" + std::to_string(1000 + i), "", 39.5, 31.5));
        in.push_back(at("4b" + std::to_string(1000 + i), "TUAF" + std::to_string(i), 39.5, 31.5));
        in.push_back(at("4c" + std::to_string(1000 + i), "THY" + std::to_string(i), 39.5, 31.5));
    }
    FilterStats stats;
    const auto out = filter_states(std::move(in), area, filter, stats);
    EXPECT_EQ(stats.dropped_military, 100u);
    EXPECT_EQ(out.size(), 50u);
    EXPECT_TRUE(std::none_of(out.begin(), out.end(), [&](const StateVector& s) {
        return filter.classify(s).has_value();
    }));
}

TEST(Pipeline, StatsAccumulateAcrossCalls) {
    const auto area = square_area();
    const auto filter = make_filter();
    FilterStats stats;
    (void)filter_states({at("4b8001", "", 39.5, 31.5)}, area, filter, stats);
    (void)filter_states({at("4b8001", "", 45.0, 31.5)}, area, filter, stats);
    EXPECT_EQ(stats.total, 2u);
    EXPECT_EQ(stats.kept, 1u);
    EXPECT_EQ(stats.dropped_outside, 1u);
}

TEST(Pipeline, EmptyInput) {
    FilterStats stats;
    const auto out = filter_states({}, square_area(), make_filter(), stats);
    EXPECT_TRUE(out.empty());
    EXPECT_EQ(stats.total, 0u);
}
