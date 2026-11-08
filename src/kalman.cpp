#include "sema/kalman.hpp"

#include <algorithm>
#include <cmath>

namespace sema {
namespace {

// Discrete white-noise-acceleration process noise for one axis.
void add_axis_q(Mat<4, 4>& p, std::size_t pos, std::size_t vel, double dt, double q) {
    const double dt2 = dt * dt;
    p(pos, pos) += q * dt2 * dt / 3.0;
    p(pos, vel) += q * dt2 / 2.0;
    p(vel, pos) += q * dt2 / 2.0;
    p(vel, vel) += q * dt;
}

template <std::size_t M>
bool kalman_update(Mat<4, 1>& x, Mat<4, 4>& p, const Mat<M, 4>& h, const Mat<M, 1>& z, const Mat<M, M>& r) {
    const Mat<M, 1> y = z - h * x;
    const Mat<4, M> ht = h.t();
    const Mat<M, M> s = h * p * ht + r;
    const auto s_inv = inverse(s);
    if (!s_inv) return false;
    const Mat<4, M> k = p * ht * *s_inv;
    x += k * y;
    // Joseph form keeps P symmetric positive definite.
    const Mat<4, 4> i_kh = Mat<4, 4>::identity() - k * h;
    p = i_kh * p * i_kh.t() + k * r * k.t();
    return true;
}

}  // namespace

HorizontalFilter::HorizontalFilter(const Measurement& first, double initial_speed_sigma, double accel_psd)
    : q_(accel_psd) {
    x_(0, 0) = first.east;
    x_(1, 0) = first.north;
    const double pv = first.pos_sigma * first.pos_sigma;
    p_(0, 0) = pv;
    p_(1, 1) = pv;
    if (first.v_east && first.v_north) {
        x_(2, 0) = *first.v_east;
        x_(3, 0) = *first.v_north;
        const double vv = first.vel_sigma * first.vel_sigma;
        p_(2, 2) = vv;
        p_(3, 3) = vv;
    } else {
        const double vv = initial_speed_sigma * initial_speed_sigma;
        p_(2, 2) = vv;
        p_(3, 3) = vv;
    }
}

void HorizontalFilter::predict(double dt) {
    if (dt <= 0.0) return;
    Mat<4, 4> f = Mat<4, 4>::identity();
    f(0, 2) = dt;
    f(1, 3) = dt;
    x_ = f * x_;
    p_ = f * p_ * f.t();
    add_axis_q(p_, 0, 2, dt, q_);
    add_axis_q(p_, 1, 3, dt, q_);
}

bool HorizontalFilter::update(const Measurement& z) {
    const double pv = z.pos_sigma * z.pos_sigma;
    if (z.v_east && z.v_north) {
        Mat<4, 4> h = Mat<4, 4>::identity();
        Mat<4, 1> m;
        m(0, 0) = z.east;
        m(1, 0) = z.north;
        m(2, 0) = *z.v_east;
        m(3, 0) = *z.v_north;
        Mat<4, 4> r;
        const double vv = z.vel_sigma * z.vel_sigma;
        r(0, 0) = pv;
        r(1, 1) = pv;
        r(2, 2) = vv;
        r(3, 3) = vv;
        return kalman_update(x_, p_, h, m, r);
    }
    Mat<2, 4> h;
    h(0, 0) = 1.0;
    h(1, 1) = 1.0;
    Mat<2, 1> m;
    m(0, 0) = z.east;
    m(1, 0) = z.north;
    Mat<2, 2> r;
    r(0, 0) = pv;
    r(1, 1) = pv;
    return kalman_update(x_, p_, h, m, r);
}

void HorizontalFilter::shift_origin(double east, double north) {
    x_(0, 0) -= east;
    x_(1, 0) -= north;
}

VerticalFilter::VerticalFilter(double altitude, std::optional<double> vertical_rate, double alt_sigma, double accel_psd)
    : q_(accel_psd) {
    x_(0, 0) = altitude;
    x_(1, 0) = vertical_rate.value_or(0.0);
    p_(0, 0) = alt_sigma * alt_sigma;
    p_(1, 1) = vertical_rate ? 1.0 : 25.0;
}

void VerticalFilter::predict(double dt) {
    if (dt <= 0.0) return;
    Mat<2, 2> f = Mat<2, 2>::identity();
    f(0, 1) = dt;
    x_ = f * x_;
    p_ = f * p_ * f.t();
    const double dt2 = dt * dt;
    p_(0, 0) += q_ * dt2 * dt / 3.0;
    p_(0, 1) += q_ * dt2 / 2.0;
    p_(1, 0) += q_ * dt2 / 2.0;
    p_(1, 1) += q_ * dt;
}

void VerticalFilter::update(double altitude, std::optional<double> vertical_rate, double alt_sigma, double rate_sigma) {
    // Sequential scalar updates: altitude, then (optionally) vertical rate.
    auto scalar = [this](std::size_t idx, double z, double var) {
        const double s = p_(idx, idx) + var;
        if (s <= 0.0) return;
        const double k0 = p_(0, idx) / s;
        const double k1 = p_(1, idx) / s;
        const double y = z - x_(idx, 0);
        x_(0, 0) += k0 * y;
        x_(1, 0) += k1 * y;
        const double pi0 = p_(idx, 0);
        const double pi1 = p_(idx, 1);
        p_(0, 0) -= k0 * pi0;
        p_(0, 1) -= k0 * pi1;
        p_(1, 0) -= k1 * pi0;
        p_(1, 1) -= k1 * pi1;
    };
    scalar(0, altitude, alt_sigma * alt_sigma);
    if (vertical_rate) scalar(1, *vertical_rate, rate_sigma * rate_sigma);
}

namespace {

constexpr double kTwoPi = 6.28318530717958647692;

double wrap_pi(double a) {
    a = std::fmod(a + kTwoPi / 2.0, kTwoPi);
    if (a < 0.0) a += kTwoPi;
    return a - kTwoPi / 2.0;
}

}  // namespace

TurnFilter::TurnFilter(const Measurement& first, const Noise& noise) : noise_(noise) {
    x_(0, 0) = first.east;
    x_(1, 0) = first.north;
    x_(2, 0) = first.speed.value_or(0.0);
    x_(3, 0) = wrap_pi(first.track_rad.value_or(0.0));
    x_(4, 0) = 0.0;
    p_(0, 0) = first.pos_sigma * first.pos_sigma;
    p_(1, 1) = first.pos_sigma * first.pos_sigma;
    p_(2, 2) = first.speed_sigma * first.speed_sigma;
    p_(3, 3) = first.track_sigma * first.track_sigma;
    // Unknown turn rate: up to a standard-rate turn (3 deg/s ~ 0.052 rad/s).
    p_(4, 4) = 0.03 * 0.03;
}

TurnFilter::State TurnFilter::propagate(const State& x, double dt) {
    State out = x;
    const double v = x(2, 0);
    const double psi = x(3, 0);
    const double w = x(4, 0);
    if (std::abs(w) < 1e-6) {
        out(0, 0) += v * std::sin(psi) * dt;
        out(1, 0) += v * std::cos(psi) * dt;
    } else {
        out(0, 0) += v / w * (std::cos(psi) - std::cos(psi + w * dt));
        out(1, 0) += v / w * (std::sin(psi + w * dt) - std::sin(psi));
    }
    out(3, 0) = wrap_pi(psi + w * dt);
    return out;
}

void TurnFilter::predict(double dt) {
    if (dt <= 0.0) return;
    // Numerical Jacobian of the motion model (central differences).
    Mat<5, 5> f;
    constexpr std::array<double, 5> step = {1.0, 1.0, 0.01, 1e-5, 1e-6};
    for (std::size_t j = 0; j < 5; ++j) {
        State a = x_;
        State b = x_;
        a(j, 0) += step[j];
        b(j, 0) -= step[j];
        const State fa = propagate(a, dt);
        const State fb = propagate(b, dt);
        for (std::size_t i = 0; i < 5; ++i) {
            double d = fa(i, 0) - fb(i, 0);
            if (i == 3) d = wrap_pi(d);
            f(i, j) = d / (2.0 * step[j]);
        }
    }
    x_ = propagate(x_, dt);
    p_ = f * p_ * f.t();

    const double dt2 = dt * dt;
    const double dt3 = dt2 * dt;
    // Along-track acceleration noise on speed, spread isotropically on position.
    const double qa = noise_.accel_psd;
    p_(0, 0) += qa * dt3 / 3.0;
    p_(1, 1) += qa * dt3 / 3.0;
    p_(2, 2) += qa * dt;
    // Turn-rate random walk on (track, turn rate).
    const double qw = noise_.turn_accel_psd;
    p_(3, 3) += qw * dt3 / 3.0;
    p_(3, 4) += qw * dt2 / 2.0;
    p_(4, 3) += qw * dt2 / 2.0;
    p_(4, 4) += qw * dt;

    x_(4, 0) = std::clamp(x_(4, 0), -noise_.max_turn_rate, noise_.max_turn_rate);
}

template <std::size_t M>
bool TurnFilter::apply(const Mat<M, 5>& h, const Mat<M, 1>& y, const Mat<M, M>& r) {
    const Mat<5, M> ht = h.t();
    const auto s_inv = inverse(h * p_ * ht + r);
    if (!s_inv) return false;
    const Mat<5, M> k = p_ * ht * *s_inv;
    x_ += k * y;
    x_(3, 0) = wrap_pi(x_(3, 0));
    x_(4, 0) = std::clamp(x_(4, 0), -noise_.max_turn_rate, noise_.max_turn_rate);
    const Mat<5, 5> i_kh = Mat<5, 5>::identity() - k * h;
    p_ = i_kh * p_ * i_kh.t() + k * r * k.t();
    return true;
}

bool TurnFilter::update(const Measurement& z) {
    const double pv = z.pos_sigma * z.pos_sigma;
    if (z.speed && z.track_rad) {
        Mat<4, 5> h;
        for (std::size_t i = 0; i < 4; ++i) h(i, i) = 1.0;
        Mat<4, 1> y;
        y(0, 0) = z.east - x_(0, 0);
        y(1, 0) = z.north - x_(1, 0);
        y(2, 0) = *z.speed - x_(2, 0);
        y(3, 0) = wrap_pi(*z.track_rad - x_(3, 0));
        Mat<4, 4> r;
        r(0, 0) = pv;
        r(1, 1) = pv;
        r(2, 2) = z.speed_sigma * z.speed_sigma;
        r(3, 3) = z.track_sigma * z.track_sigma;
        return apply(h, y, r);
    }
    Mat<2, 5> h;
    h(0, 0) = 1.0;
    h(1, 1) = 1.0;
    Mat<2, 1> y;
    y(0, 0) = z.east - x_(0, 0);
    y(1, 0) = z.north - x_(1, 0);
    Mat<2, 2> r;
    r(0, 0) = pv;
    r(1, 1) = pv;
    return apply(h, y, r);
}

void TurnFilter::shift_origin(double east, double north) {
    x_(0, 0) -= east;
    x_(1, 0) -= north;
}

}  // namespace sema
