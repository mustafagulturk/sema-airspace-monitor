#include "sema/prom_metrics.hpp"

#include <unistd.h>

#include <cmath>
#include <cstdio>
#include <fstream>
#include <sstream>
#include <stdexcept>

namespace sema {
namespace {

std::string escape_label(const std::string& v) {
    std::string out;
    out.reserve(v.size());
    for (const char c : v) {
        if (c == '\\' || c == '"') {
            out.push_back('\\');
            out.push_back(c);
        } else if (c == '\n') {
            out += "\\n";
        } else {
            out.push_back(c);
        }
    }
    return out;
}

std::string format_value(double v) {
    if (std::isnan(v)) return "NaN";
    if (std::isinf(v)) return v > 0 ? "+Inf" : "-Inf";
    char buf[64];
    (void)std::snprintf(buf, sizeof buf, "%.17g", v);
    return buf;
}

}  // namespace

std::string PromRegistry::label_string(const Labels& labels) {
    if (labels.empty()) return {};
    std::string s = "{";
    for (std::size_t i = 0; i < labels.size(); ++i) {
        if (i > 0) s.push_back(',');
        s += labels[i].first + "=\"" + escape_label(labels[i].second) + "\"";
    }
    s.push_back('}');
    return s;
}

PromRegistry::Family& PromRegistry::family(const std::string& name, const char* type, const std::string& help) {
    auto& f = families_[name];
    if (f.type.empty()) {
        f.type = type;
        f.help = help;
    } else if (f.type != type) {
        throw std::logic_error("metric " + name + " registered as " + f.type);
    }
    return f;
}

void PromRegistry::set_gauge(const std::string& name, const std::string& help, double value, const Labels& labels) {
    const std::scoped_lock lock(mu_);
    family(name, "gauge", help).series[label_string(labels)] = value;
}

void PromRegistry::add_counter(const std::string& name, const std::string& help, double delta, const Labels& labels) {
    const std::scoped_lock lock(mu_);
    family(name, "counter", help).series[label_string(labels)] += delta;
}

void PromRegistry::clear(const std::string& name) {
    const std::scoped_lock lock(mu_);
    if (auto it = families_.find(name); it != families_.end()) it->second.series.clear();
}

double PromRegistry::value(const std::string& name, const Labels& labels) const {
    const std::scoped_lock lock(mu_);
    const auto it = families_.find(name);
    if (it == families_.end()) return 0.0;
    const auto s = it->second.series.find(label_string(labels));
    return s == it->second.series.end() ? 0.0 : s->second;
}

std::string PromRegistry::render() const {
    const std::scoped_lock lock(mu_);
    std::ostringstream out;
    for (const auto& [name, f] : families_) {
        if (f.series.empty()) continue;
        out << "# HELP " << name << ' ' << f.help << '\n' << "# TYPE " << name << ' ' << f.type << '\n';
        for (const auto& [labels, v] : f.series) out << name << labels << ' ' << format_value(v) << '\n';
    }
    return out.str();
}

std::uint64_t process_rss_bytes() {
    std::ifstream in("/proc/self/statm");
    std::uint64_t pages_total = 0;
    std::uint64_t pages_resident = 0;
    if (!(in >> pages_total >> pages_resident)) return 0;
    const long page = sysconf(_SC_PAGESIZE);
    return page > 0 ? pages_resident * static_cast<std::uint64_t>(page) : 0;
}

}  // namespace sema
