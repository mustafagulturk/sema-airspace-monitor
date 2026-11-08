#include "sema/metrics.hpp"

#include <algorithm>
#include <cmath>

namespace sema {
namespace {

double round_to(double x, int decimals) {
    const double f = std::pow(10.0, decimals);
    return std::round(x * f) / f;
}

}  // namespace

void Distribution::add(double x) {
    ++count_;
    sum_ += x;
    min_ = std::min(min_, x);
    max_ = std::max(max_, x);
    if (samples_.size() < max_samples_) {
        samples_.push_back(x);
        return;
    }
    // xorshift64; replace a random slot with probability max_samples/count.
    rng_ ^= rng_ << 13;
    rng_ ^= rng_ >> 7;
    rng_ ^= rng_ << 17;
    const std::uint64_t j = rng_ % count_;
    if (j < max_samples_) samples_[j] = x;
}

void Distribution::merge(const Distribution& other) {
    // Approximate when either side has been sampled; exact otherwise.
    const auto c = count_ + other.count_;
    const double s = sum_ + other.sum_;
    const double lo = std::min(min_, other.min_);
    const double hi = std::max(max_, other.max_);
    for (double x : other.samples_) add(x);
    count_ = c;
    sum_ = s;
    min_ = lo;
    max_ = hi;
}

void Distribution::reset() {
    count_ = 0;
    sum_ = 0.0;
    min_ = std::numeric_limits<double>::infinity();
    max_ = -std::numeric_limits<double>::infinity();
    samples_.clear();
}

double Distribution::percentile(double p) const {
    if (samples_.empty()) return 0.0;
    std::vector<double> v = samples_;
    std::sort(v.begin(), v.end());
    const double rank = std::clamp(p, 0.0, 100.0) / 100.0 * static_cast<double>(v.size() - 1);
    const auto lo = static_cast<std::size_t>(std::floor(rank));
    const auto hi = static_cast<std::size_t>(std::ceil(rank));
    return v[lo] + (v[hi] - v[lo]) * (rank - static_cast<double>(lo));
}

nlohmann::json Distribution::to_json(int decimals) const {
    return {{"n", count_},
            {"mean", round_to(mean(), decimals)},
            {"p50", round_to(percentile(50), decimals)},
            {"p90", round_to(percentile(90), decimals)},
            {"p95", round_to(percentile(95), decimals)},
            {"max", round_to(max(), decimals)}};
}

}  // namespace sema
