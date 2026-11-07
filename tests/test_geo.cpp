#include <stdexcept>

#include <gtest/gtest.h>

#include "sema/geo.hpp"

using sema::BoundingBox;
using sema::LatLon;
using sema::MultiPolygon;

TEST(CreditCost, Tiers) {
    EXPECT_EQ(sema::opensky_credit_cost({0, 0, 5, 5}), 1);     // 25
    EXPECT_EQ(sema::opensky_credit_cost({0, 0, 5, 5.1}), 2);   // 25.5
    EXPECT_EQ(sema::opensky_credit_cost({0, 0, 10, 10}), 2);   // 100
    EXPECT_EQ(sema::opensky_credit_cost({0, 0, 10, 10.5}), 3); // 105
    EXPECT_EQ(sema::opensky_credit_cost({0, 0, 20, 20}), 3);   // 400
    EXPECT_EQ(sema::opensky_credit_cost({0, 0, 20, 21}), 4);   // 420
    EXPECT_EQ(sema::opensky_credit_cost({36, 26, 42, 45}), 3); // scope document box
}

TEST(BoundingBox, Contains) {
    const BoundingBox b{36, 26, 42, 45};
    EXPECT_TRUE(b.contains({39, 35}));
    EXPECT_TRUE(b.contains({36, 26}));
    EXPECT_FALSE(b.contains({35.9, 35}));
    EXPECT_FALSE(b.contains({39, 45.1}));
}

namespace {
// 10x10 square with a 2x2 hole in the middle, plus a separate island.
const char* kSquareWithHole = R"({
  "type": "FeatureCollection",
  "features": [
    {"type": "Feature", "properties": {}, "geometry": {"type": "Polygon", "coordinates": [
      [[0,0],[10,0],[10,10],[0,10],[0,0]],
      [[4,4],[6,4],[6,6],[4,6],[4,4]]
    ]}},
    {"type": "Feature", "properties": {}, "geometry": {"type": "MultiPolygon", "coordinates": [
      [[[20,20],[21,20],[21,21],[20,21],[20,20]]]
    ]}}
  ]
})";
}  // namespace

TEST(MultiPolygon, HolesAndMultipleParts) {
    const auto mp = MultiPolygon::from_geojson(kSquareWithHole);
    EXPECT_EQ(mp.polygons().size(), 2u);
    EXPECT_TRUE(mp.contains({1, 1}));
    EXPECT_FALSE(mp.contains({5, 5}));  // in the hole
    EXPECT_TRUE(mp.contains({20.5, 20.5}));
    EXPECT_FALSE(mp.contains({15, 15}));
    EXPECT_FALSE(mp.contains({-1, 5}));
    EXPECT_DOUBLE_EQ(mp.bounds().lat_max, 21);
    EXPECT_DOUBLE_EQ(mp.bounds().lon_min, 0);
}

TEST(MultiPolygon, GeoJsonRoundTrip) {
    const auto mp = MultiPolygon::from_geojson(kSquareWithHole);
    const auto again = MultiPolygon::from_geojson(mp.to_geojson_geometry());
    ASSERT_EQ(again.polygons().size(), 2u);
    EXPECT_EQ(again.polygons()[0].holes.size(), 1u);
    EXPECT_FALSE(again.contains({5, 5}));
    EXPECT_TRUE(again.contains({20.5, 20.5}));
}

TEST(MultiPolygon, RejectsInvalidInput) {
    EXPECT_THROW(MultiPolygon::from_geojson("{"), std::runtime_error);
    EXPECT_THROW(MultiPolygon::from_geojson(R"({"type":"Point","coordinates":[1,2]})"), std::runtime_error);
    EXPECT_THROW(MultiPolygon::from_geojson(R"({"type":"Polygon","coordinates":[[[0,0],[1,1]]]})"),
                 std::runtime_error);
    EXPECT_THROW(MultiPolygon::load_geojson("/nonexistent/file.geojson"), std::runtime_error);
}

class TurkeyBoundary : public ::testing::Test {
protected:
    static void SetUpTestSuite() {
        area_ = new MultiPolygon(MultiPolygon::load_geojson(std::string(SEMA_DATA_DIR) + "/turkey_boundary.geojson"));
    }
    static void TearDownTestSuite() {
        delete area_;
        area_ = nullptr;
    }
    static MultiPolygon* area_;
};
MultiPolygon* TurkeyBoundary::area_ = nullptr;

TEST_F(TurkeyBoundary, PointsInsideTurkey) {
    EXPECT_TRUE(area_->contains({41.01, 28.98})) << "Istanbul";
    EXPECT_TRUE(area_->contains({39.93, 32.85})) << "Ankara";
    EXPECT_TRUE(area_->contains({38.50, 43.38})) << "Van";
    EXPECT_TRUE(area_->contains({36.20, 36.16})) << "Antakya (Hatay)";
    EXPECT_TRUE(area_->contains({41.08, 29.06})) << "Bosphorus";
    EXPECT_TRUE(area_->contains({40.18, 25.88})) << "Gokceada";
    EXPECT_TRUE(area_->contains({36.88, 30.70})) << "Antalya";
}

TEST_F(TurkeyBoundary, PointsOutsideTurkey) {
    EXPECT_FALSE(area_->contains({37.98, 23.73})) << "Athens";
    EXPECT_FALSE(area_->contains({42.70, 23.32})) << "Sofia";
    EXPECT_FALSE(area_->contains({35.17, 33.36})) << "Nicosia";
    EXPECT_FALSE(area_->contains({39.105, 26.555})) << "Mytilene, Lesbos";
    EXPECT_FALSE(area_->contains({41.72, 44.79})) << "Tbilisi";
    EXPECT_FALSE(area_->contains({43.0, 34.0})) << "mid Black Sea";
}

TEST_F(TurkeyBoundary, QueryBoxCoversPolygonAndCostsThreeCredits) {
    const BoundingBox q = sema::query_box_for(*area_);
    const BoundingBox& b = area_->bounds();
    EXPECT_LE(q.lat_min, b.lat_min);
    EXPECT_LE(q.lon_min, b.lon_min);
    EXPECT_GE(q.lat_max, b.lat_max);
    EXPECT_GE(q.lon_max, b.lon_max);
    EXPECT_LT(b.lat_min - q.lat_min, 0.05 + 1e-9);
    EXPECT_LT(q.lon_max - b.lon_max, 0.05 + 1e-9);
    EXPECT_EQ(sema::opensky_credit_cost(q), 3);
    for (const auto& poly : area_->polygons())
        for (const auto& p : poly.outer) EXPECT_TRUE(q.contains(p));
}
