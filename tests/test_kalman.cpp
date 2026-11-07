#include <gtest/gtest.h>

#include <cmath>
#include <random>

#include "sema/geo.hpp"
#include "sema/kalman.hpp"

using namespace sema;

namespace {
constexpr double kPi = 3.14159265358979323846;
}

TEST(Geo, HaversineMatchesKnownDistance) {
    // Istanbul Airport to Ankara Esenboga, ~362 km great-circle.
    const double d = haversine_m({41.275, 28.752}, {40.128, 32.995});
    EXPECT_NEAR(d, 377'000.0, 5'000.0);
    EXPECT_DOUBLE_EQ(haversine_m({39.0, 35.0}, {39.0, 35.0}), 0.0);
    // One degree of latitude is ~111.2 km.
    EXPECT_NEAR(haversine_m({39.0, 35.0}, {40.0, 35.0}), 111'195.0, 50.0);
}

TEST(Geo, LocalFrameRoundTripsAndAgreesWithHaversine) {
    const LatLon ref{39.5, 33.0};
    const LatLon p{39.7, 33.3};
    const EnuOffset d = to_local(ref, p);
    EXPECT_GT(d.east, 0.0);
    EXPECT_GT(d.north, 0.0);
    const LatLon back = from_local(ref, d);
    EXPECT_NEAR(back.lat, p.lat, 1e-9);
    EXPECT_NEAR(back.lon, p.lon, 1e-9);
    // ~34 km away: the ellipsoidal plane and the spherical great circle agree
    // to within the sphere's ~0.2% scale error.
    const double gc = haversine_m(ref, p);
    EXPECT_NEAR(std::hypot(d.east, d.north), gc, 0.003 * gc);
}

TEST(Matrix, InverseOfSpdMatrix) {
    Mat<3, 3> a;
    a.v = {4, 1, 0, 1, 3, 1, 0, 1, 2};
    const auto inv = inverse(a);
    ASSERT_TRUE(inv.has_value());
    const auto id = a * *inv;
    for (std::size_t r = 0; r < 3; ++r)
        for (std::size_t c = 0; c < 3; ++c) EXPECT_NEAR(id(r, c), r == c ? 1.0 : 0.0, 1e-12);
    Mat<2, 2> singular;
    singular.v = {1, 2, 2, 4};
    EXPECT_FALSE(inverse(singular).has_value());
}

TEST(HorizontalFilter, NoiseFreeStraightLineIsPredictedExactly) {
    HorizontalFilter::Measurement z{0.0, 0.0, 30.0, 150.0, 200.0, 2.0};
    HorizontalFilter f(z, 150.0, 4.0);
    for (int k = 1; k <= 5; ++k) {
        f.predict(60.0);
        z.east = 150.0 * 60.0 * k;
        z.north = 200.0 * 60.0 * k;
        f.update(z);
    }
    f.predict(70.0);
    EXPECT_NEAR(f.x()(0, 0), 150.0 * 370.0, 1.0);
    EXPECT_NEAR(f.x()(1, 0), 200.0 * 370.0, 1.0);
    EXPECT_NEAR(f.x()(2, 0), 150.0, 1e-6);
}

TEST(HorizontalFilter, LearnsVelocityFromPositionsOnly) {
    std::mt19937 rng(7);
    std::normal_distribution<double> noise(0.0, 30.0);
    HorizontalFilter::Measurement z{noise(rng), noise(rng), 30.0, std::nullopt, std::nullopt, 0.0};
    HorizontalFilter f(z, 150.0, 1.0);
    for (int k = 1; k <= 20; ++k) {
        f.predict(10.0);
        z.east = -120.0 * 10.0 * k + noise(rng);
        z.north = 80.0 * 10.0 * k + noise(rng);
        f.update(z);
    }
    EXPECT_NEAR(f.x()(2, 0), -120.0, 3.0);
    EXPECT_NEAR(f.x()(3, 0), 80.0, 3.0);
    // Uncertainty shrinks well below the measurement noise.
    EXPECT_LT(std::sqrt(f.P()(0, 0)), 30.0);
}

TEST(HorizontalFilter, ShiftOriginKeepsPhysicalPosition) {
    HorizontalFilter f({100.0, -50.0, 30.0, 10.0, 0.0, 2.0}, 150.0, 4.0);
    f.shift_origin(100.0, -50.0);
    EXPECT_NEAR(f.x()(0, 0), 0.0, 1e-9);
    EXPECT_NEAR(f.x()(1, 0), 0.0, 1e-9);
    EXPECT_NEAR(f.x()(2, 0), 10.0, 1e-9);
}

TEST(TurnFilter, PropagateFollowsACircle) {
    TurnFilter::State x;
    x(2, 0) = 200.0;           // m/s
    x(3, 0) = 0.0;             // heading north
    x(4, 0) = kPi / 2 / 60.0;  // quarter turn to the right per minute
    const auto y = TurnFilter::propagate(x, 60.0);
    const double r = 200.0 / x(4, 0);
    EXPECT_NEAR(y(0, 0), r, 1e-6);  // ends due east of the start by one radius
    EXPECT_NEAR(y(1, 0), r, 1e-6);
    EXPECT_NEAR(y(3, 0), kPi / 2, 1e-12);
    // Straight flight is the omega -> 0 limit.
    x(4, 0) = 0.0;
    const auto s = TurnFilter::propagate(x, 60.0);
    EXPECT_NEAR(s(1, 0), 12'000.0, 1e-9);
}

TEST(TurnFilter, EstimatesSteadyTurnAndBeatsConstantVelocityInIt) {
    std::mt19937 rng(3);
    std::normal_distribution<double> n(0.0, 1.0);
    const double w = 1.0 * kPi / 180.0;  // 1 deg/s
    const double v = 220.0;
    TurnFilter::State truth;
    truth(2, 0) = v;
    truth(3, 0) = 0.3;
    truth(4, 0) = w;
    auto meas = [&] {
        TurnFilter::Measurement m{truth(0, 0) + 10 * n(rng), truth(1, 0) + 10 * n(rng), 30.0,
                                  v + 0.5 * n(rng), truth(3, 0) + 0.005 * n(rng), 2.0, 0.0175};
        return m;
    };
    auto to_cv = [](const TurnFilter::Measurement& m) {
        return HorizontalFilter::Measurement{m.east, m.north, m.pos_sigma, *m.speed * std::sin(*m.track_rad),
                                             *m.speed * std::cos(*m.track_rad), 4.0};
    };
    auto first = meas();
    TurnFilter ct(first, {});
    HorizontalFilter cv(to_cv(first), 150.0, 4.0);
    double ct_err = 0.0;
    double cv_err = 0.0;
    for (int k = 0; k < 15; ++k) {
        truth = TurnFilter::propagate(truth, 30.0);
        ct.predict(30.0);
        cv.predict(30.0);
        if (k >= 5) {
            ct_err += std::hypot(ct.x()(0, 0) - truth(0, 0), ct.x()(1, 0) - truth(1, 0));
            cv_err += std::hypot(cv.x()(0, 0) - truth(0, 0), cv.x()(1, 0) - truth(1, 0));
        }
        const auto z = meas();
        ct.update(z);
        cv.update(to_cv(z));
    }
    EXPECT_NEAR(ct.x()(4, 0), w, 0.1 * w);
    EXPECT_LT(ct_err, cv_err / 2.0);
}

TEST(VerticalFilter, TracksClimb) {
    VerticalFilter f(1000.0, 10.0, 15.0, 0.5);
    for (int k = 1; k <= 10; ++k) {
        f.predict(60.0);
        f.update(1000.0 + 10.0 * 60.0 * k, 10.0, 15.0, 1.0);
    }
    EXPECT_NEAR(f.altitude(), 7000.0, 5.0);
    EXPECT_NEAR(f.vertical_rate(), 10.0, 0.2);
}
