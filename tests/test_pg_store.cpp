// Integration tests for PgStore. They need a PostgreSQL database with PostGIS
// available and are skipped unless SEMA_TEST_DATABASE_URL is set, e.g.
//   SEMA_TEST_DATABASE_URL=postgresql://postgres:postgres@localhost:5432/sema_test
#include <gtest/gtest.h>

#include <chrono>
#include <cstdlib>
#include "../src/pqxx_compat.hpp"

#include "sema/pg_history.hpp"
#include "sema/pg_store.hpp"

namespace {

using namespace std::chrono_literals;
using sema::PgStore;
using sema::StateVector;

const char* db_url() { return std::getenv("SEMA_TEST_DATABASE_URL"); }

StateVector make_state(std::string icao, double lat, double lon) {
    StateVector s;
    s.icao24 = std::move(icao);
    s.callsign = "THY1AB";
    s.origin_country = "Turkey";
    s.time_position = 1'700'000'000;
    s.last_contact = 1'700'000'001;
    s.latitude = lat;
    s.longitude = lon;
    s.baro_altitude = 10972.8;
    s.geo_altitude = 11010.0;
    s.velocity = 231.5;
    s.true_track = 87.0;
    s.vertical_rate = -0.33;
    s.on_ground = false;
    s.squawk = "2341";
    s.position_source = 0;
    return s;
}

class PgStoreTest : public ::testing::Test {
protected:
    void SetUp() override {
        if (db_url() == nullptr) GTEST_SKIP() << "SEMA_TEST_DATABASE_URL not set";
        store_ = std::make_unique<PgStore>(db_url());
        store_->migrate();
        conn_ = std::make_unique<pqxx::connection>(db_url());
        pqxx::work tx(*conn_);
        tx.exec("TRUNCATE positions, positions_hourly, fetch_log, boundaries, anomaly_events, flights");
        tx.commit();
    }

    template <typename T>
    T scalar(const std::string& sql) {
        pqxx::work tx(*conn_);
        auto v = tx.query_value<T>(sql);
        tx.commit();
        return v;
    }

    std::unique_ptr<PgStore> store_;
    std::unique_ptr<pqxx::connection> conn_;
};

TEST_F(PgStoreTest, MigrateIsIdempotent) {
    EXPECT_NO_THROW(store_->migrate());
    EXPECT_NO_THROW(store_->migrate());
    EXPECT_EQ(scalar<int>("SELECT max(version) FROM schema_version"), 3);
}

TEST_F(PgStoreTest, InsertAndReadBack) {
    const auto now = std::chrono::system_clock::now();
    auto full = make_state("4bb1a2", 40.98, 28.82);
    StateVector sparse;
    sparse.icao24 = "4b8e01";
    sparse.last_contact = 1'700'000'010;
    sparse.latitude = 39.93;
    sparse.longitude = 32.86;
    StateVector no_position;
    no_position.icao24 = "4b8e02";
    no_position.last_contact = 1'700'000'010;

    EXPECT_EQ(store_->insert_positions(now, {full, sparse, no_position}), 2u);

    pqxx::work tx(*conn_);
    auto r = tx.exec1(
        "SELECT callsign, origin_country, extract(epoch FROM time_position)::bigint, "
        "ST_Y(geom::geometry), ST_X(geom::geometry), baro_altitude_m, velocity_ms, squawk, on_ground, "
        "position_source FROM positions WHERE icao24 = '4bb1a2'");
    EXPECT_EQ(r[0].as<std::string>(), "THY1AB");
    EXPECT_EQ(r[1].as<std::string>(), "Turkey");
    EXPECT_EQ(r[2].as<std::int64_t>(), 1'700'000'000);
    EXPECT_NEAR(r[3].as<double>(), 40.98, 1e-9);
    EXPECT_NEAR(r[4].as<double>(), 28.82, 1e-9);
    EXPECT_NEAR(r[5].as<double>(), 10972.8, 0.01);
    EXPECT_NEAR(r[6].as<double>(), 231.5, 0.01);
    EXPECT_EQ(r[7].as<std::string>(), "2341");
    EXPECT_FALSE(r[8].as<bool>());
    EXPECT_EQ(r[9].as<int>(), 0);

    auto n = tx.exec1(
        "SELECT callsign IS NULL, squawk IS NULL, time_position IS NULL, baro_altitude_m IS NULL, "
        "velocity_ms IS NULL FROM positions WHERE icao24 = '4b8e01'");
    for (int i = 0; i < 5; ++i) EXPECT_TRUE(n[i].as<bool>()) << "column " << i;
}

TEST_F(PgStoreTest, EventsUpsertByKey) {
    sema::AnomalyEvent e;
    e.id = 7;
    e.kind = sema::EventKind::Emergency;
    e.icao24 = "4bb1a2";
    e.callsign = "THY1AB";
    e.origin_country = "Turkey";
    e.start_time = 1'700'000'000;
    e.detect_time = 1'700'000'065;
    e.end_time = 1'700'000'065;
    e.pos = {40.0, 30.0};
    e.altitude_m = 10000.0;
    e.summary = "Acil durum kodu 7700 (genel acil durum)";
    e.details = {{"squawk", "7700"}};
    e.track = {{1'699'999'940, {39.99, 29.9}, 10000.0}, {1'700'000'000, {40.0, 30.0}, 10000.0}};
    store_->upsert_events({e});

    e.active = false;
    e.end_time = 1'700'000'600;
    e.track.push_back({1'700'000'600, {40.1, 30.5}, 9000.0});
    store_->upsert_events({e});

    EXPECT_EQ(scalar<int>("SELECT count(*) FROM anomaly_events"), 1);
    pqxx::work tx(*conn_);
    auto r = tx.exec1(
        "SELECT key, kind, active, extract(epoch FROM end_time)::bigint, details->>'squawk', "
        "ST_NPoints(track), ST_Y(geom::geometry) FROM anomaly_events");
    EXPECT_EQ(r[0].as<std::string>(), "emergency:4bb1a2:1700000000");
    EXPECT_EQ(r[1].as<std::string>(), "emergency");
    EXPECT_FALSE(r[2].as<bool>());
    EXPECT_EQ(r[3].as<std::int64_t>(), 1'700'000'600);
    EXPECT_EQ(r[4].as<std::string>(), "7700");
    EXPECT_EQ(r[5].as<int>(), 3);
    EXPECT_NEAR(r[6].as<double>(), 40.0, 1e-9);
    tx.commit();
    EXPECT_EQ(scalar<int>("SELECT count(*) FROM daily_events"), 1);
}

TEST_F(PgStoreTest, DuplicateInsertIgnored) {
    const auto now = std::chrono::system_clock::now();
    const std::vector<StateVector> batch{make_state("4bb1a2", 40.0, 30.0)};
    EXPECT_EQ(store_->insert_positions(now, batch), 1u);
    EXPECT_EQ(store_->insert_positions(now, batch), 0u);
    EXPECT_EQ(scalar<int>("SELECT count(*) FROM positions"), 1);
}

TEST_F(PgStoreTest, BoundaryUpsertAndContains) {
    // Crude box around Anatolia; Athens lies outside it.
    const std::string poly1 = R"({"type":"Polygon","coordinates":[[[26,36],[45,36],[45,42],[26,42],[26,36]]]})";
    const std::string poly2 = R"({"type":"MultiPolygon","coordinates":[[[[26.5,36],[45,36],[45,42],[26.5,42],[26.5,36]]]]})";
    store_->upsert_boundary("turkey", poly1);
    store_->upsert_boundary("turkey", poly2);  // replaces, does not duplicate
    EXPECT_EQ(scalar<int>("SELECT count(*) FROM boundaries"), 1);
    EXPECT_TRUE(scalar<bool>(
        "SELECT ST_Contains(geom, ST_SetSRID(ST_MakePoint(32.86, 39.93), 4326)) FROM boundaries WHERE name='turkey'"));
    EXPECT_FALSE(scalar<bool>(
        "SELECT ST_Contains(geom, ST_SetSRID(ST_MakePoint(23.73, 37.98), 4326)) FROM boundaries WHERE name='turkey'"));
    EXPECT_EQ(scalar<std::string>("SELECT GeometryType(geom) FROM boundaries"), "MULTIPOLYGON");
}

TEST_F(PgStoreTest, FetchLogFeedsDailyMetrics) {
    const auto t0 = std::chrono::system_clock::now() - 2min;
    sema::FetchLogEntry e;
    e.started_at = t0;
    e.latency = 420ms;
    e.http_status = 200;
    e.attempts = 1;
    e.credits_cost = 3;
    e.credits_remaining = 3997;
    e.data_time = 1'700'000'000;
    e.states_total = 300;
    e.states_kept = 250;
    e.dropped_outside = 48;
    e.dropped_military = 2;
    store_->log_fetch(e);
    e.started_at = t0 + 65s;
    e.credits_remaining = 3994;
    e.data_time = 1'700'000'065;
    store_->log_fetch(e);

    sema::FetchLogEntry failed;
    failed.started_at = t0 + 70s;
    failed.attempts = 4;
    failed.credits_cost = 3;
    failed.error = "timeout";
    store_->log_fetch(failed);

    EXPECT_EQ(scalar<int>("SELECT count(*) FROM fetch_log"), 3);
    EXPECT_EQ(scalar<std::string>("SELECT error FROM fetch_log WHERE http_status IS NULL"), "timeout");

    pqxx::work tx(*conn_);
    auto r = tx.exec1(
        "SELECT requests, credits_used, min_credits_remaining, avg_aircraft_kept, avg_request_interval_s, "
        "avg_data_interval_s FROM daily_metrics ORDER BY day DESC LIMIT 1");
    // Only successful requests are counted.
    EXPECT_EQ(r[0].as<int>(), 2);
    EXPECT_EQ(r[1].as<int>(), 6);
    EXPECT_EQ(r[2].as<int>(), 3994);
    EXPECT_NEAR(r[3].as<double>(), 250.0, 0.01);
    EXPECT_NEAR(r[4].as<double>(), 65.0, 0.5);
    EXPECT_NEAR(r[5].as<double>(), 65.0, 0.01);
}

TEST_F(PgStoreTest, RollupAndPrune) {
    const auto now = std::chrono::system_clock::now();
    // Three samples of one aircraft, 5 hours ago, within the same hour bucket.
    auto old_hour = std::chrono::floor<std::chrono::hours>(now - 5h);
    for (int i = 0; i < 3; ++i) {
        auto s = make_state("4bb1a2", 40.0 + 0.1 * i, 30.0 + 0.1 * i);
        s.baro_altitude = 10000.0 + 100.0 * i;
        store_->insert_positions(old_hour + std::chrono::minutes(10 + i), {s});
    }
    // Single old sample of another aircraft: no line can be built.
    store_->insert_positions(old_hour + 20min, {make_state("4b8e01", 38.0, 27.0)});
    // Recent sample must survive.
    store_->insert_positions(now, {make_state("4bb1a2", 41.0, 29.0)});

    EXPECT_EQ(store_->rollup_and_prune(1h), 4);
    EXPECT_EQ(scalar<int>("SELECT count(*) FROM positions"), 1);
    EXPECT_EQ(scalar<int>("SELECT count(*) FROM positions_hourly"), 2);

    pqxx::work tx(*conn_);
    auto r = tx.exec1(
        "SELECT samples, callsign, min_altitude_m, max_altitude_m, GeometryType(track), ST_NPoints(track) "
        "FROM positions_hourly WHERE icao24 = '4bb1a2'");
    EXPECT_EQ(r[0].as<int>(), 3);
    EXPECT_EQ(r[1].as<std::string>(), "THY1AB");
    EXPECT_NEAR(r[2].as<double>(), 10000.0, 0.01);
    EXPECT_NEAR(r[3].as<double>(), 10200.0, 0.01);
    EXPECT_EQ(r[4].as<std::string>(), "LINESTRING");
    EXPECT_EQ(r[5].as<int>(), 3);
    EXPECT_TRUE(tx.query_value<bool>("SELECT track IS NULL FROM positions_hourly WHERE icao24 = '4b8e01'"));
    tx.commit();

    // Running again is a no-op.
    EXPECT_EQ(store_->rollup_and_prune(1h), 0);
}

}  // namespace

TEST_F(PgStoreTest, FlightsRoundTripThroughPgHistory) {
    sema::FlightRecord f;
    f.track_id = 42;
    f.icao24 = "4b8001";
    f.callsign = "THY7";
    f.origin_country = "Turkey";
    f.first_seen = 1'700'000'000;
    f.last_seen = 1'700'003'600;
    f.first_pos = {41.27, 28.73};
    f.last_pos = {40.13, 32.99};
    f.max_altitude_m = 11000.0;
    f.fixes = 55;
    f.airborne = true;
    f.departure = "LTFM";
    f.arrival = "LTAC";
    f.end_reason = "landed";
    f.cells = {1'000'001, 1'000'002};
    sema::FlightRecord g = f;
    g.icao24 = "4b8002";
    g.first_seen = 1'700'090'000;
    g.departure.clear();
    g.cells.clear();
    EXPECT_EQ(store_->insert_flights({f, g}), 2U);
    EXPECT_EQ(store_->insert_flights({f}), 0U);  // duplicate (icao24, first_seen)
    EXPECT_EQ(scalar<int>("SELECT count(*) FROM daily_traffic"), 2);
    EXPECT_EQ(scalar<int>("SELECT sum(departures) FROM daily_traffic"), 1);

    sema::PgHistory h(db_url());
    const auto rows = h.flights(1'699'999'000, 1'700'100'000);
    ASSERT_EQ(rows.size(), 2U);
    EXPECT_EQ(rows[0].icao24, "4b8001");
    EXPECT_EQ(rows[0].callsign, "THY7");
    EXPECT_EQ(rows[0].departure, "LTFM");
    EXPECT_EQ(rows[0].arrival, "LTAC");
    EXPECT_EQ(rows[0].end_reason, "landed");
    EXPECT_EQ(rows[0].cells, f.cells);
    EXPECT_NEAR(rows[0].first_pos.lat, 41.27, 1e-9);
    EXPECT_NEAR(rows[0].last_pos.lon, 32.99, 1e-9);
    EXPECT_DOUBLE_EQ(rows[0].first_seen, f.first_seen);
    EXPECT_FALSE(rows[0].min_altitude_m.has_value());
    EXPECT_NEAR(rows[0].max_altitude_m.value_or(0), 11000.0, 1e-3);
    EXPECT_EQ(rows[1].departure, "");
    EXPECT_TRUE(rows[1].cells.empty());
    EXPECT_EQ(h.flights(1'700'000'001, 1'700'090'000).size(), 0U);
}

TEST_F(PgStoreTest, PgHistoryReplaysPositionsWithoutRepeats) {
    sema::PgHistory h(db_url());
    EXPECT_FALSE(h.available());
    const auto fetched = std::chrono::system_clock::time_point(std::chrono::seconds(1'700'000'000));
    auto a = make_state("4b8001", 39.0, 32.0);
    store_->insert_positions(fetched, {a, make_state("4b8002", 40.0, 30.0)});
    // OpenSky repeats a's report in the next snapshot, then a moves on.
    store_->insert_positions(fetched + 10s, {a});
    a.time_position = 1'700'000'065;
    a.latitude = 39.1;
    a.baro_altitude.reset();
    store_->insert_positions(fetched + 65s, {a});

    const auto range = h.available();
    ASSERT_TRUE(range);
    EXPECT_DOUBLE_EQ(range->from, 1'700'000'000.0);
    EXPECT_DOUBLE_EQ(range->to, 1'700'000'065.0);

    auto r = h.replay(1'699'999'000, 1'700'001'000);
    ASSERT_EQ(r.size(), 2U);
    EXPECT_EQ(r[0].icao24, "4b8001");
    EXPECT_EQ(r[0].callsign, "THY1AB");
    ASSERT_EQ(r[0].fixes.size(), 2U);
    EXPECT_DOUBLE_EQ(r[0].fixes[0].t, 1'700'000'000.0);
    EXPECT_NEAR(r[0].fixes[1].lat, 39.1, 1e-9);
    // Falls back to geometric altitude when there is no barometric one.
    EXPECT_NEAR(r[0].fixes[1].alt_m.value_or(0), 11010.0, 0.01);
    EXPECT_NEAR(r[0].fixes[0].speed_ms.value_or(0), 231.5, 0.01);

    r = h.replay(1'700'000'030, 1'700'001'000);
    ASSERT_EQ(r.size(), 1U);
    EXPECT_EQ(r[0].fixes.size(), 1U);
}

TEST_F(PgStoreTest, CollectionStreakFromFetchLog) {
    sema::PgHistory h(db_url());
    EXPECT_FALSE(h.collection_streak());
    const auto now = std::chrono::system_clock::now();
    sema::FetchLogEntry e;
    e.http_status = 200;
    e.attempts = 1;
    e.credits_cost = 3;
    // An old run, a 40 minute gap, then a run every minute up to now.
    for (int i = 0; i < 5; ++i) {
        e.started_at = now - 3h + std::chrono::minutes(i);
        store_->log_fetch(e);
    }
    for (int i = 0; i <= 100; ++i) {
        e.started_at = now - 2h + std::chrono::minutes(i + 20) - 1min;
        store_->log_fetch(e);
    }
    const auto s = h.collection_streak();
    ASSERT_TRUE(s);
    EXPECT_NEAR(s->to - s->from, 100 * 60.0, 1.0);

    // Nothing for 20 minutes: no current streak.
    pqxx::work tx(*conn_);
    tx.exec("UPDATE fetch_log SET started_at = started_at - interval '20 minutes'");
    tx.commit();
    EXPECT_FALSE(h.collection_streak());
}
