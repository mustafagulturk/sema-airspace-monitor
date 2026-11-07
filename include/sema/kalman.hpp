#pragma once

#include <array>
#include <cmath>
#include <cstddef>
#include <optional>
#include <utility>

namespace sema {

// Minimal fixed-size row-major matrix for the small filters below.
template <std::size_t R, std::size_t C>
struct Mat {
    std::array<double, R * C> v{};

    double& operator()(std::size_t r, std::size_t c) { return v[r * C + c]; }
    double operator()(std::size_t r, std::size_t c) const { return v[r * C + c]; }

    static Mat identity() {
        static_assert(R == C);
        Mat m;
        for (std::size_t i = 0; i < R; ++i) m(i, i) = 1.0;
        return m;
    }
    [[nodiscard]] Mat<C, R> t() const {
        Mat<C, R> m;
        for (std::size_t r = 0; r < R; ++r)
            for (std::size_t c = 0; c < C; ++c) m(c, r) = (*this)(r, c);
        return m;
    }
    Mat& operator+=(const Mat& o) {
        for (std::size_t i = 0; i < R * C; ++i) v[i] += o.v[i];
        return *this;
    }
    Mat& operator-=(const Mat& o) {
        for (std::size_t i = 0; i < R * C; ++i) v[i] -= o.v[i];
        return *this;
    }
    friend Mat operator+(Mat a, const Mat& b) { return a += b; }
    friend Mat operator-(Mat a, const Mat& b) { return a -= b; }
};

template <std::size_t R, std::size_t K, std::size_t C>
Mat<R, C> operator*(const Mat<R, K>& a, const Mat<K, C>& b) {
    Mat<R, C> m;
    for (std::size_t r = 0; r < R; ++r)
        for (std::size_t k = 0; k < K; ++k) {
            const double x = a(r, k);
            if (x == 0.0) continue;
            for (std::size_t c = 0; c < C; ++c) m(r, c) += x * b(k, c);
        }
    return m;
}

// Inverse of a symmetric positive definite matrix (Gauss-Jordan with partial
// pivoting). Returns nullopt if the matrix is numerically singular.
template <std::size_t N>
std::optional<Mat<N, N>> inverse(Mat<N, N> a) {
    Mat<N, N> inv = Mat<N, N>::identity();
    for (std::size_t col = 0; col < N; ++col) {
        std::size_t piv = col;
        for (std::size_t r = col + 1; r < N; ++r)
            if (std::abs(a(r, col)) > std::abs(a(piv, col))) piv = r;
        if (std::abs(a(piv, col)) < 1e-12) return std::nullopt;
        if (piv != col)
            for (std::size_t c = 0; c < N; ++c) {
                std::swap(a(col, c), a(piv, c));
                std::swap(inv(col, c), inv(piv, c));
            }
        const double d = a(col, col);
        for (std::size_t c = 0; c < N; ++c) {
            a(col, c) /= d;
            inv(col, c) /= d;
        }
        for (std::size_t r = 0; r < N; ++r) {
            if (r == col) continue;
            const double f = a(r, col);
            if (f == 0.0) continue;
            for (std::size_t c = 0; c < N; ++c) {
                a(r, c) -= f * a(col, c);
                inv(r, c) -= f * inv(col, c);
            }
        }
    }
    return inv;
}

// Horizontal constant-velocity Kalman filter in a local east/north plane.
// State: [east m, north m, v_east m/s, v_north m/s]. Process noise is white
// acceleration with spectral density `accel_psd` (m^2/s^3) on each axis.
class HorizontalFilter {
public:
    using State = Mat<4, 1>;
    using Cov = Mat<4, 4>;

    struct Measurement {
        double east;
        double north;
        double pos_sigma;  // metres, 1-sigma per axis
        // Optional velocity measurement (from OpenSky velocity + true_track).
        std::optional<double> v_east;
        std::optional<double> v_north;
        double vel_sigma = 0.0;  // m/s, 1-sigma per axis
    };

    HorizontalFilter() = default;
    HorizontalFilter(const Measurement& first, double initial_speed_sigma, double accel_psd);

    void predict(double dt);
    // Returns false (and leaves the state untouched) if the innovation
    // covariance is singular.
    bool update(const Measurement& z);
    // Moves the frame origin by (east, north) metres; the state is shifted so
    // the physical position does not change.
    void shift_origin(double east, double north);

    [[nodiscard]] const State& x() const { return x_; }
    [[nodiscard]] const Cov& P() const { return p_; }
    [[nodiscard]] double accel_psd() const { return q_; }

private:
    State x_{};
    Cov p_{};
    double q_ = 1.0;
};

// Coordinated-turn extended Kalman filter in a local east/north plane.
// State: [east m, north m, speed m/s, track rad (clockwise from north),
// turn rate rad/s]. Aircraft mostly fly straight or turn at a steady rate, so
// over OpenSky's ~60-75 s gaps this follows turns that a constant-velocity
// model cuts across. Speed and track are measured directly (OpenSky velocity
// and true_track), so the measurement model is linear; only the prediction is
// non-linear (Jacobian by central differences).
class TurnFilter {
public:
    using State = Mat<5, 1>;
    using Cov = Mat<5, 5>;

    struct Measurement {
        double east;
        double north;
        double pos_sigma;  // metres, per axis
        // Speed and track come together; a fix without them only updates position.
        std::optional<double> speed;  // m/s
        std::optional<double> track_rad;
        double speed_sigma = 0.0;
        double track_sigma = 0.0;  // rad
    };

    struct Noise {
        double accel_psd = 4.0;        // along-track acceleration, m^2/s^3
        double turn_accel_psd = 1e-5;  // change of turn rate, rad^2/s^3
        double max_turn_rate = 0.1;  // rad/s, clamp (~5.7 deg/s, well above standard rate)
    };

    TurnFilter() = default;
    // `first` must carry speed and track.
    TurnFilter(const Measurement& first, const Noise& noise);

    void predict(double dt);
    bool update(const Measurement& z);
    void shift_origin(double east, double north);

    [[nodiscard]] const State& x() const { return x_; }
    [[nodiscard]] const Cov& P() const { return p_; }

    // Noise-free motion model, exposed for tests.
    static State propagate(const State& x, double dt);

private:
    template <std::size_t M>
    bool apply(const Mat<M, 5>& h, const Mat<M, 1>& y, const Mat<M, M>& r);

    State x_{};
    Cov p_{};
    Noise noise_{};
};

// Vertical constant-velocity filter: [altitude m, vertical rate m/s].
class VerticalFilter {
public:
    VerticalFilter() = default;
    VerticalFilter(double altitude, std::optional<double> vertical_rate, double alt_sigma, double accel_psd);

    void predict(double dt);
    void update(double altitude, std::optional<double> vertical_rate, double alt_sigma, double rate_sigma);

    [[nodiscard]] double altitude() const { return x_(0, 0); }
    [[nodiscard]] double vertical_rate() const { return x_(1, 0); }

private:
    Mat<2, 1> x_{};
    Mat<2, 2> p_{};
    double q_ = 1.0;
};

}  // namespace sema
