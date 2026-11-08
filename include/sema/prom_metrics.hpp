#pragma once

#include <cstdint>
#include <map>
#include <mutex>
#include <string>
#include <type_traits>
#include <utility>
#include <vector>

namespace sema {

// Tiny Prometheus text-format registry (exposition format 0.0.4): gauges and
// counters with optional labels. Thread safe; the collector loop writes, the
// HTTP worker renders /metrics.
class PromRegistry {
public:
    using Labels = std::vector<std::pair<std::string, std::string>>;

    void set_gauge(const std::string& name, const std::string& help, double value, const Labels& labels = {});
    void add_counter(const std::string& name, const std::string& help, double delta, const Labels& labels = {});
    // Integer convenience overloads (counts, sizes, timestamps).
    template <class T>
        requires(std::is_arithmetic_v<T> && !std::is_same_v<T, double>)
    void set_gauge(const std::string& name, const std::string& help, T value, const Labels& labels = {}) {
        set_gauge(name, help, static_cast<double>(value), labels);
    }
    template <class T>
        requires(std::is_arithmetic_v<T> && !std::is_same_v<T, double>)
    void add_counter(const std::string& name, const std::string& help, T delta, const Labels& labels = {}) {
        add_counter(name, help, static_cast<double>(delta), labels);
    }
    // Removes every series of a gauge, e.g. before re-setting a labelled family.
    void clear(const std::string& name);

    [[nodiscard]] std::string render() const;
    [[nodiscard]] double value(const std::string& name, const Labels& labels = {}) const;

private:
    struct Family {
        std::string type;
        std::string help;
        std::map<std::string, double> series;  // rendered label set -> value
    };
    static std::string label_string(const Labels& labels);
    Family& family(const std::string& name, const char* type, const std::string& help);

    mutable std::mutex mu_;
    std::map<std::string, Family> families_;
};

// Resident set size of this process in bytes (Linux /proc), 0 if unknown.
std::uint64_t process_rss_bytes();

}  // namespace sema
