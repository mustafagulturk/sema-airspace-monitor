#include <gtest/gtest.h>

#include <memory>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "sema/history.hpp"
#include "sema/prom_metrics.hpp"

using namespace sema;

namespace {

constexpr std::int64_t kT0 = 1'762'549'200;

StateVector sv(const std::string& icao, std::int64_t t, double lon, bool with_pos = true) {
    StateVector s;
    s.icao24 = icao;
    s.callsign = icao == "aaa001" ? "THY1" : "";
    s.origin_country = "Turkey";
    s.time_position = t;
    s.last_contact = t;
    if (with_pos) {
        s.latitude = 39.0;
        s.longitude = lon;
    }
    s.baro_altitude = 10000.0;
    s.velocity = 230.4;
    s.true_track = 90.0;
    return s;
}

FlightRecord flight(const std::string& icao, double first) {
    FlightRecord f;
    f.icao24 = icao;
    f.first_seen = first;
    f.last_seen = first + 1800;
    f.fixes = 20;
    f.airborne = true;
    f.departure = "LTFM";
    f.arrival = "LTAC";
    return f;
}

}  // namespace

TEST(ParseQuery, DecodesPairs) {
    const auto q = parse_query("from=1700000000&to=17%2E5&label=sentetik+veri&flag&=x&a=1&a=2");
    EXPECT_EQ(q.at("from"), "1700000000");
    EXPECT_EQ(q.at("to"), "17.5");
    EXPECT_EQ(q.at("label"), "sentetik veri");
    EXPECT_EQ(q.at("flag"), "");
    EXPECT_EQ(q.at("a"), "2");
    EXPECT_TRUE(parse_query("").empty());
    EXPECT_EQ(parse_query("x=%zz").at("x"), "%zz");
}

TEST(MemoryHistory, RecordsReplaysAndDeduplicates) {
    MemoryHistory h;
    EXPECT_FALSE(h.available());
    EXPECT_FALSE(h.collection_streak());
    h.record({kT0, {sv("aaa001", kT0, 30.0), sv("bbb002", kT0 - 2, 31.0), sv("ccc003", kT0, 0, false)}}, {});
    // OpenSky repeats aaa001's report; bbb002 moves on.
    h.record({kT0 + 10, {sv("aaa001", kT0, 30.0), sv("bbb002", kT0 + 9, 31.1)}}, {flight("aaa001", kT0 - 600)});
    h.record({kT0 + 70, {sv("aaa001", kT0 + 65, 30.2)}}, {});
    EXPECT_EQ(h.fix_count(), 4U);
    EXPECT_EQ(h.flight_count(), 1U);

    const auto range = h.available();
    ASSERT_TRUE(range);
    EXPECT_DOUBLE_EQ(range->from, static_cast<double>(kT0 - 2));
    EXPECT_DOUBLE_EQ(range->to, static_cast<double>(kT0 + 70));

    const auto all = h.replay(kT0 - 100, kT0 + 100);
    ASSERT_EQ(all.size(), 2U);
    EXPECT_EQ(all[0].icao24, "aaa001");
    EXPECT_EQ(all[0].callsign, "THY1");
    ASSERT_EQ(all[0].fixes.size(), 2U);
    EXPECT_DOUBLE_EQ(all[0].fixes[1].t, static_cast<double>(kT0 + 65));
    EXPECT_DOUBLE_EQ(all[0].fixes[1].lon, 30.2);
    EXPECT_EQ(all[1].fixes.size(), 2U);

    const auto part = h.replay(kT0 + 5, kT0 + 100);
    ASSERT_EQ(part.size(), 2U);
    EXPECT_EQ(part[0].icao24, "bbb002");  // first in time order within the range
    EXPECT_EQ(part[0].fixes.size(), 1U);

    EXPECT_EQ(h.flights(kT0 - 3600, kT0).size(), 1U);
    EXPECT_TRUE(h.flights(kT0, kT0 + 3600).empty());
}

TEST(MemoryHistory, StreakRestartsAfterAGapAndOldDataIsPruned) {
    MemoryHistory::Limits lim;
    lim.keep_fixes_s = 3600;
    MemoryHistory h(lim);
    h.record({kT0, {sv("aaa001", kT0, 30.0)}}, {});
    h.record({kT0 + 600, {sv("aaa001", kT0 + 600, 30.5)}}, {});
    EXPECT_DOUBLE_EQ(h.collection_streak()->from, static_cast<double>(kT0));
    h.record({kT0 + 600 + 2000, {sv("aaa001", kT0 + 2600, 31.0)}}, {});  // 33 min gap
    EXPECT_DOUBLE_EQ(h.collection_streak()->from, static_cast<double>(kT0 + 2600));
    EXPECT_DOUBLE_EQ(h.collection_streak()->to, static_cast<double>(kT0 + 2600));
    h.record({kT0 + 4000, {sv("aaa001", kT0 + 4000, 31.5)}}, {});
    // The first fix is now older than an hour.
    EXPECT_EQ(h.fix_count(), 3U);
    EXPECT_DOUBLE_EQ(h.available()->from, static_cast<double>(kT0 + 600));
}

TEST(ReplayJson, CompactFixArrays) {
    ReplayAircraft a{"aaa001", "THY1", "Turkey", {}};
    ReplayFix f;
    f.t = 100;
    f.lat = 39.123456789;
    f.lon = 30.987654321;
    f.alt_m = 10000.4;
    f.speed_ms = 230.44;
    f.on_ground = false;
    a.fixes.push_back(f);
    const auto j = replay_to_json({a}, 50, 150);
    EXPECT_EQ(j["fixes"], 1);
    const auto& fx = j["aircraft"][0]["fixes"][0];
    EXPECT_DOUBLE_EQ(fx[1].get<double>(), 39.12346);
    EXPECT_DOUBLE_EQ(fx[3].get<double>(), 10000.0);
    EXPECT_NEAR(fx[4].get<double>(), 230.4, 1e-9);
    EXPECT_TRUE(fx[5].is_null());
    EXPECT_EQ(fx[6], 0);
}

class HistoryApiTest : public ::testing::Test {
protected:
    void SetUp() override {
        history_ = std::make_shared<MemoryHistory>();
        for (int i = 0; i < 10; ++i) {
            const std::int64_t t = kT0 + 60 * i;
            history_->record({t, {sv("aaa001", t, 30.0 + 0.1 * i)}}, {});
        }
        history_->record({kT0 + 600, {}}, {flight("aaa001", kT0), flight("bbb002", kT0 + 100)});
        auto airports = std::make_shared<const AirportIndex>(std::vector<Airport>{
            {"LTFM", "IST", "İstanbul Airport", "Istanbul", {41.27, 28.73}, 99.0},
            {"LTAC", "ESB", "Esenboğa", "Ankara", {40.13, 32.99}, 953.0}});
        HistoryApiConfig cfg;
        cfg.max_replay_s = 3600;
        api_ = std::make_unique<HistoryApi>(history_, airports, cfg, [this] { return now_; });
    }

    nlohmann::json call(const std::string& path, const std::string& query, int expect_status = 200) {
        const auto r = api_->handle({path, parse_query(query)});
        EXPECT_TRUE(r.has_value()) << path;
        if (!r) return {};
        EXPECT_EQ(r->status, expect_status) << path << '?' << query << " -> " << r->body;
        EXPECT_EQ(r->content_type, "application/json");
        return nlohmann::json::parse(r->body);
    }

    double now_ = static_cast<double>(kT0 + 1200);
    std::shared_ptr<MemoryHistory> history_;
    std::unique_ptr<HistoryApi> api_;
};

TEST_F(HistoryApiTest, Status) {
    now_ += 50;
    const auto j = call("/api/status", "");
    EXPECT_EQ(j["mode"], "live");
    EXPECT_EQ(j["uptime_s"], 50);
    EXPECT_EQ(j["history"]["from"], kT0);
    EXPECT_EQ(j["history"]["to"], kT0 + 600);
    EXPECT_EQ(j["streak"]["seconds"], 600);
    EXPECT_EQ(j["airports"], 2);
    EXPECT_EQ(j["max_replay_s"], 3600);
}

TEST_F(HistoryApiTest, Replay) {
    const auto j = call("/api/replay", "from=" + std::to_string(kT0 + 60) + "&to=" + std::to_string(kT0 + 300));
    ASSERT_EQ(j["aircraft"].size(), 1U);
    EXPECT_EQ(j["aircraft"][0]["fixes"].size(), 5U);
    EXPECT_EQ(j["fixes"], 5);
}

TEST_F(HistoryApiTest, ReplayValidatesTheRange) {
    EXPECT_EQ(call("/api/replay", "from=1", 400)["error"], "'from' and 'to' are required");
    call("/api/replay", "from=10&to=5", 400);
    call("/api/replay", "from=abc&to=5", 400);
    call("/api/replay", "from=1e400&to=5", 400);
    call("/api/replay", "from=0&to=7200", 400);  // over max_replay_s
}

TEST_F(HistoryApiTest, StatsDefaultRangeIncludesOpenFlightsAndIsCached) {
    FlightRecord open = flight("ccc003", kT0 + 900);
    open.end_reason = "open";
    history_->set_open_flights({open});
    auto j = call("/api/stats", "");
    EXPECT_EQ(j["totals"]["flights"], 3);
    EXPECT_EQ(j["completed_flights"], 2);
    EXPECT_EQ(j["open_flights"], 1);
    EXPECT_EQ(j["routes"][0]["flights"], 3);
    EXPECT_EQ(j["totals"]["days"], 8);  // 7 days back from now, partial days at both ends

    // Within the cache time the same answer comes back even if data changed.
    history_->set_open_flights({});
    j = call("/api/stats", "");
    EXPECT_EQ(j["totals"]["flights"], 3);
    now_ += 120;
    j = call("/api/stats", "");
    EXPECT_EQ(j["totals"]["flights"], 2);
}

TEST_F(HistoryApiTest, StatsExplicitRangeAndErrors) {
    const auto j = call("/api/stats", "from=" + std::to_string(kT0 + 50) + "&to=" + std::to_string(kT0 + 3600));
    EXPECT_EQ(j["totals"]["flights"], 1);
    call("/api/stats", "from=0&to=99999999", 400);
}

TEST_F(HistoryApiTest, UnknownAndForeignPaths) {
    EXPECT_EQ(call("/api/nope", "", 404)["error"], "unknown endpoint");
    EXPECT_FALSE(api_->handle({"/metrics", {}}).has_value());
    EXPECT_FALSE(api_->handle({"/index.html", {}}).has_value());
}

TEST(PromRegistry, RendersGaugesAndCountersWithLabels) {
    PromRegistry p;
    p.set_gauge("sema_tracks", "Tracks by state", 12, {{"state", "confirmed"}});
    p.set_gauge("sema_tracks", "Tracks by state", 3, {{"state", "coasting"}});
    p.add_counter("sema_requests_total", "Requests", 1.0, {{"status", "ok"}});
    p.add_counter("sema_requests_total", "Requests", 2, {{"status", "ok"}});
    p.add_counter("sema_requests_total", "Requests", 1, {{"status", "say \"hi\"\n"}});
    p.set_gauge("sema_up", "Up", 1.0);
    const auto text = p.render();
    EXPECT_NE(text.find("# HELP sema_tracks Tracks by state\n# TYPE sema_tracks gauge\n"), std::string::npos);
    EXPECT_NE(text.find("sema_tracks{state=\"coasting\"} 3\n"), std::string::npos);
    EXPECT_NE(text.find("sema_tracks{state=\"confirmed\"} 12\n"), std::string::npos);
    EXPECT_NE(text.find("# TYPE sema_requests_total counter\n"), std::string::npos);
    EXPECT_NE(text.find("sema_requests_total{status=\"ok\"} 3\n"), std::string::npos);
    EXPECT_NE(text.find("sema_requests_total{status=\"say \\\"hi\\\"\\n\"} 1\n"), std::string::npos);
    EXPECT_NE(text.find("sema_up 1\n"), std::string::npos);
    EXPECT_DOUBLE_EQ(p.value("sema_requests_total", {{"status", "ok"}}), 3.0);
    EXPECT_THROW(p.add_counter("sema_up", "Up", 1.0), std::logic_error);
    p.clear("sema_tracks");
    EXPECT_EQ(p.render().find("sema_tracks"), std::string::npos);
}

TEST(PromRegistry, ProcessRssIsKnownOnLinux) { EXPECT_GT(process_rss_bytes(), 1024U * 1024U); }
