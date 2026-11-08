#include <gtest/gtest.h>

#include <string>
#include <vector>

#include "sema/traffic_stats.hpp"

using namespace sema;

namespace {

// 2025-11-07 00:00:00 UTC
constexpr double kDay = 1'762'473'600.0;

AirportIndex airports() {
    return AirportIndex({{"LTFM", "IST", "İstanbul Airport", "Istanbul", {41.27, 28.73}, 99.0},
                         {"LTAC", "ESB", "Esenboğa", "Ankara", {40.13, 32.99}, 953.0},
                         {"LTAI", "AYT", "Antalya", "Antalya", {36.90, 30.80}, 54.0}});
}

FlightRecord flight(const std::string& icao, double first, double last, std::string dep, std::string arr,
                    std::vector<std::uint32_t> cells = {}) {
    FlightRecord f;
    f.icao24 = icao;
    f.first_seen = first;
    f.last_seen = last;
    f.fixes = 10;
    f.airborne = true;
    f.departure = std::move(dep);
    f.arrival = std::move(arr);
    f.cells = std::move(cells);
    return f;
}

}  // namespace

TEST(TrafficStats, CountsDaysHoursAirportsRoutesAndCorridors) {
    const auto c1 = corridor_cell({40.6, 30.9});
    const auto c2 = corridor_cell({39.0, 31.0});
    std::vector<FlightRecord> fl = {
        // Local (UTC+3) 2025-11-07 09:00-10:30: IST -> ESB
        flight("aaa001", kDay + 6 * 3600, kDay + 7.5 * 3600, "LTFM", "LTAC", {c1}),
        // Same aircraft, back ESB -> IST at local 12:10-13:20
        flight("aaa001", kDay + 9 * 3600 + 600, kDay + 10 * 3600 + 1200, "LTAC", "LTFM", {c1}),
        // IST -> ESB again, local 09:30-10:40
        flight("bbb002", kDay + 6.5 * 3600, kDay + 7.6 * 3600, "LTFM", "LTAC", {c1, c2}),
        // Overflight at 22:30 UTC = 01:30 local on 2025-11-08
        flight("ccc003", kDay + 22.5 * 3600, kDay + 23 * 3600, "", "", {c2}),
    };
    // Not counted: ground-only, single fix, outside the range.
    auto ground = flight("ddd004", kDay + 3600, kDay + 4000, "LTAI", "");
    ground.airborne = false;
    fl.push_back(ground);
    auto single = flight("eee005", kDay + 3600, kDay + 3600, "LTAI", "");
    single.fixes = 1;
    fl.push_back(single);
    fl.push_back(flight("fff006", kDay + 3 * 86400, kDay + 3 * 86400 + 600, "LTAI", "LTFM"));

    StatsQuery q;
    q.from = kDay - 3 * 3600;          // local midnight 2025-11-07
    q.to = kDay + 2 * 86400 - 3 * 3600;  // local midnight 2025-11-09
    const auto j = compute_traffic_stats(fl, airports(), q);

    EXPECT_EQ(j["totals"]["flights"], 4);
    EXPECT_EQ(j["totals"]["aircraft"], 3);
    EXPECT_EQ(j["totals"]["departures"], 3);
    EXPECT_EQ(j["totals"]["arrivals"], 3);
    EXPECT_EQ(j["totals"]["days"], 2);

    ASSERT_EQ(j["daily"].size(), 2U);
    EXPECT_EQ(j["daily"][0]["date"], "2025-11-07");
    EXPECT_EQ(j["daily"][0]["flights"], 3);
    EXPECT_EQ(j["daily"][0]["aircraft"], 2);
    EXPECT_EQ(j["daily"][1]["date"], "2025-11-08");
    EXPECT_EQ(j["daily"][1]["flights"], 1);
    EXPECT_EQ(j["busiest_day"], "2025-11-07");

    // Local hours 09 and 10 have both IST->ESB flights in the air.
    ASSERT_EQ(j["hours"].size(), 24U);
    EXPECT_EQ(j["hours"][9]["total"], 2);
    EXPECT_EQ(j["hours"][10]["total"], 2);
    EXPECT_EQ(j["hours"][12]["total"], 1);
    EXPECT_EQ(j["hours"][13]["total"], 1);
    EXPECT_EQ(j["hours"][1]["total"], 1);
    EXPECT_EQ(j["hours"][3]["total"], 0);
    EXPECT_DOUBLE_EQ(j["hours"][9]["avg"].get<double>(), 1.0);
    EXPECT_EQ(j["peak_hour"], 9);

    ASSERT_EQ(j["airports"].size(), 2U);
    EXPECT_EQ(j["airports"][0]["icao"], "LTAC");  // tie on 3, sorted by code
    EXPECT_EQ(j["airports"][0]["arrivals"], 2);
    EXPECT_EQ(j["airports"][0]["departures"], 1);
    EXPECT_EQ(j["airports"][0]["iata"], "ESB");
    EXPECT_EQ(j["airports"][1]["icao"], "LTFM");

    ASSERT_EQ(j["routes"].size(), 2U);
    EXPECT_EQ(j["routes"][0]["from"], "LTFM");
    EXPECT_EQ(j["routes"][0]["to"], "LTAC");
    EXPECT_EQ(j["routes"][0]["flights"], 2);

    EXPECT_EQ(j["corridors"]["max"], 3);
    ASSERT_EQ(j["corridors"]["cells"].size(), 2U);
    EXPECT_DOUBLE_EQ(j["corridors"]["cells"][0][0].get<double>(), 40.5);
    EXPECT_DOUBLE_EQ(j["corridors"]["cells"][0][1].get<double>(), 30.75);
    EXPECT_EQ(j["corridors"]["cells"][0][2], 3);
    EXPECT_EQ(j["corridors"]["cells"][1][2], 2);
}

TEST(TrafficStats, EmptyRangeHasZeroDaysFilledAndNoPeak) {
    StatsQuery q;
    q.from = kDay - 3 * 3600;
    q.to = q.from + 3 * 86400;
    const auto j = compute_traffic_stats({}, airports(), q);
    EXPECT_EQ(j["totals"]["flights"], 0);
    EXPECT_EQ(j["daily"].size(), 3U);
    EXPECT_TRUE(j["peak_hour"].is_null());
    EXPECT_TRUE(j["busiest_day"].is_null());
    EXPECT_TRUE(j["routes"].empty());
    EXPECT_EQ(j["corridors"]["max"], 0);
}

TEST(TrafficStats, RoutesAreCappedAtTopN) {
    std::vector<FlightRecord> fl;
    const char* codes[] = {"LTFM", "LTAC", "LTAI"};
    for (int i = 0; i < 3; ++i) {
        for (int k = 0; k < 3; ++k) {
            if (i != k) fl.push_back(flight("a0000" + std::to_string(i), kDay + 3600 * (i * 3 + k), kDay + 3600 * (i * 3 + k) + 600, codes[i], codes[k]));
        }
    }
    StatsQuery q;
    q.from = kDay - 3 * 3600;
    q.to = q.from + 86400;
    q.top_n = 4;
    const auto j = compute_traffic_stats(fl, airports(), q);
    EXPECT_EQ(j["routes"].size(), 4U);
}
