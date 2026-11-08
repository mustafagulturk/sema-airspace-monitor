#pragma once

#include <cstddef>
#include <cstdint>
#include <limits>
#include <vector>

#include <nlohmann/json.hpp>

namespace sema {

// Running summary of a non-negative quantity. Count, mean, min and max are
// exact; percentiles come from a bounded uniform sample (reservoir sampling
// with a fixed seed, so replays are reproducible).
class Distribution {
public:
    explicit Distribution(std::size_t max_samples = 100000) : max_samples_(max_samples) {}

    void add(double x);
    void merge(const Distribution& other);
    void reset();

    [[nodiscard]] std::uint64_t count() const { return count_; }
    [[nodiscard]] double mean() const { return count_ == 0 ? 0.0 : sum_ / static_cast<double>(count_); }
    [[nodiscard]] double min() const { return count_ == 0 ? 0.0 : min_; }
    [[nodiscard]] double max() const { return count_ == 0 ? 0.0 : max_; }
    // p in [0, 100]; linear interpolation between sample ranks. 0 if empty.
    [[nodiscard]] double percentile(double p) const;

    // {"n", "mean", "p50", "p90", "p95", "max"}, rounded to `decimals`.
    [[nodiscard]] nlohmann::json to_json(int decimals = 1) const;

private:
    std::size_t max_samples_;
    std::uint64_t count_ = 0;
    double sum_ = 0.0;
    double min_ = std::numeric_limits<double>::infinity();
    double max_ = -std::numeric_limits<double>::infinity();
    std::vector<double> samples_;
    std::uint64_t rng_ = 0x9E3779B97F4A7C15ULL;
};

}  // namespace sema
