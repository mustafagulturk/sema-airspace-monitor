#include <gtest/gtest.h>

#include "sema/metrics.hpp"

using namespace sema;

TEST(Distribution, ExactStatsAndPercentiles) {
    Distribution d;
    EXPECT_EQ(d.count(), 0U);
    EXPECT_DOUBLE_EQ(d.percentile(50), 0.0);
    for (int i = 1; i <= 101; ++i) d.add(i);
    EXPECT_EQ(d.count(), 101U);
    EXPECT_DOUBLE_EQ(d.mean(), 51.0);
    EXPECT_DOUBLE_EQ(d.min(), 1.0);
    EXPECT_DOUBLE_EQ(d.max(), 101.0);
    EXPECT_DOUBLE_EQ(d.percentile(50), 51.0);
    EXPECT_DOUBLE_EQ(d.percentile(90), 91.0);
    EXPECT_DOUBLE_EQ(d.percentile(0), 1.0);
    EXPECT_DOUBLE_EQ(d.percentile(100), 101.0);
    const auto j = d.to_json();
    EXPECT_EQ(j["n"], 101);
    EXPECT_DOUBLE_EQ(j["p50"].get<double>(), 51.0);
}

TEST(Distribution, ReservoirKeepsPercentilesApproximatelyRight) {
    Distribution d(1000);
    for (int i = 0; i < 100000; ++i) d.add(i % 1000);
    EXPECT_EQ(d.count(), 100000U);
    EXPECT_DOUBLE_EQ(d.max(), 999.0);
    EXPECT_NEAR(d.percentile(50), 500.0, 60.0);
    EXPECT_NEAR(d.percentile(90), 900.0, 40.0);
}

TEST(Distribution, MergeAndReset) {
    Distribution a;
    Distribution b;
    a.add(1);
    a.add(3);
    b.add(10);
    a.merge(b);
    EXPECT_EQ(a.count(), 3U);
    EXPECT_DOUBLE_EQ(a.max(), 10.0);
    EXPECT_DOUBLE_EQ(a.mean(), 14.0 / 3.0);
    a.reset();
    EXPECT_EQ(a.count(), 0U);
    EXPECT_DOUBLE_EQ(a.max(), 0.0);
}
