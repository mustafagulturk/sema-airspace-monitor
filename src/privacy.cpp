#include "sema/privacy.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <fstream>
#include <random>
#include <sstream>
#include <stdexcept>

#include <nlohmann/json.hpp>

namespace sema {
namespace {

// Turkey has been on UTC+3 all year since 2016; pseudonyms change at local midnight.
constexpr double kLocalOffsetS = 3 * 3600.0;

std::uint64_t rotl(std::uint64_t x, int b) { return (x << b) | (x >> (64 - b)); }

// SipHash-2-4 (Aumasson & Bernstein), a keyed PRF for short inputs.
std::uint64_t siphash24(const std::array<std::uint64_t, 2>& key, std::string_view data) {
    std::uint64_t v0 = 0x736f6d6570736575ULL ^ key[0];
    std::uint64_t v1 = 0x646f72616e646f6dULL ^ key[1];
    std::uint64_t v2 = 0x6c7967656e657261ULL ^ key[0];
    std::uint64_t v3 = 0x7465646279746573ULL ^ key[1];
    auto round = [&] {
        v0 += v1; v1 = rotl(v1, 13); v1 ^= v0; v0 = rotl(v0, 32);
        v2 += v3; v3 = rotl(v3, 16); v3 ^= v2;
        v0 += v3; v3 = rotl(v3, 21); v3 ^= v0;
        v2 += v1; v1 = rotl(v1, 17); v1 ^= v2; v2 = rotl(v2, 32);
    };
    const std::size_t n = data.size();
    std::size_t i = 0;
    for (; i + 8 <= n; i += 8) {
        std::uint64_t m = 0;
        for (int b = 0; b < 8; ++b) m |= static_cast<std::uint64_t>(static_cast<unsigned char>(data[i + b])) << (8 * b);
        v3 ^= m;
        round();
        round();
        v0 ^= m;
    }
    std::uint64_t last = static_cast<std::uint64_t>(n & 0xff) << 56;
    for (int b = 0; i + b < n; ++b) last |= static_cast<std::uint64_t>(static_cast<unsigned char>(data[i + b])) << (8 * b);
    v3 ^= last;
    round();
    round();
    v0 ^= last;
    v2 ^= 0xff;
    round();
    round();
    round();
    round();
    return v0 ^ v1 ^ v2 ^ v3;
}

std::array<std::uint64_t, 2> random_key() {
    std::random_device rd;
    auto word = [&rd] { return (static_cast<std::uint64_t>(rd()) << 32) ^ static_cast<std::uint64_t>(rd()); };
    return {word(), word()};
}

std::string trim_upper(std::string_view s) {
    while (!s.empty() && std::isspace(static_cast<unsigned char>(s.front())) != 0) s.remove_prefix(1);
    while (!s.empty() && std::isspace(static_cast<unsigned char>(s.back())) != 0) s.remove_suffix(1);
    std::string out(s);
    std::transform(out.begin(), out.end(), out.begin(),
                   [](unsigned char c) { return static_cast<char>(std::toupper(c)); });
    return out;
}

std::string lower(std::string_view s) {
    std::string out(s);
    std::transform(out.begin(), out.end(), out.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return out;
}

double message_time(const nlohmann::json& m) {
    for (const char* k : {"data_time", "time", "from"}) {
        if (auto it = m.find(k); it != m.end() && it->is_number()) return it->get<double>();
    }
    return 0.0;
}

void mask_entry(const IdentityMasker& ids, nlohmann::json& a, double t) {
    if (auto it = a.find("icao24"); it != a.end() && it->is_string()) {
        *it = ids.icao24(it->get<std::string>(), t);
    }
    if (auto it = a.find("callsign"); it != a.end() && it->is_string()) {
        *it = ids.callsign(it->get<std::string>());
    }
}

}  // namespace

const char* to_string(IdentifierMode m) {
    switch (m) {
        case IdentifierMode::Hidden: return "hidden";
        case IdentifierMode::Airline: return "airline";
        case IdentifierMode::Full: return "full";
    }
    return "hidden";
}

IdentifierMode parse_identifier_mode(std::string_view s) {
    if (s == "hidden") return IdentifierMode::Hidden;
    if (s == "airline") return IdentifierMode::Airline;
    if (s == "full") return IdentifierMode::Full;
    throw std::invalid_argument("identifier mode must be hidden, airline or full: " + std::string(s));
}

bool is_airline_callsign(std::string_view callsign) {
    const auto cs = trim_upper(callsign);
    if (cs.size() < 4 || cs.size() > 7) return false;
    for (std::size_t i = 0; i < 3; ++i) {
        if (cs[i] < 'A' || cs[i] > 'Z') return false;
    }
    if (cs[3] < '0' || cs[3] > '9') return false;
    return std::all_of(cs.begin() + 4, cs.end(), [](char c) {
        return (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9');
    });
}

IdentityMasker::IdentityMasker(IdentifierMode mode) : IdentityMasker(mode, random_key()) {}

IdentityMasker::IdentityMasker(IdentifierMode mode, std::array<std::uint64_t, 2> key) : mode_(mode), key_(key) {}

std::string IdentityMasker::icao24(std::string_view real, double time) const {
    if (mode_ == IdentifierMode::Full) return std::string(real);
    const auto day = static_cast<std::int64_t>(std::floor((time + kLocalOffsetS) / 86400.0));
    const auto h = siphash24(key_, lower(real) + "/" + std::to_string(day));
    char buf[16];
    (void)std::snprintf(buf, sizeof buf, "%c%08llx", kPseudonymPrefix, static_cast<unsigned long long>(h >> 32));
    return buf;
}

std::string IdentityMasker::callsign(std::string_view callsign) const {
    switch (mode_) {
        case IdentifierMode::Full: return std::string(callsign);
        case IdentifierMode::Airline: return is_airline_callsign(callsign) ? std::string(callsign) : std::string();
        case IdentifierMode::Hidden: return {};
    }
    return {};
}

void IdentityMasker::apply(nlohmann::json& message) const {
    if (mode_ == IdentifierMode::Full || !message.is_object()) return;
    const double t = message_time(message);
    if (auto it = message.find("aircraft"); it != message.end() && it->is_array()) {
        for (auto& a : *it) mask_entry(*this, a, t);
    }
    if (auto it = message.find("events"); it != message.end() && it->is_array()) {
        for (auto& e : *it) {
            mask_entry(*this, e, t);
            // The key is kind:icao24:start; rebuild it around the pseudonym.
            if (e.contains("kind") && e.contains("start") && e.contains("icao24")) {
                e["key"] = e["kind"].get<std::string>() + ":" + e["icao24"].get<std::string>() + ":" +
                           std::to_string(static_cast<std::int64_t>(e["start"].get<double>()));
            }
        }
    }
}

OptOutList OptOutList::parse(std::string_view text) {
    OptOutList out;
    std::istringstream in{std::string(text)};
    std::string line;
    while (std::getline(in, line)) {
        if (const auto hash = line.find('#'); hash != std::string::npos) line.resize(hash);
        auto entry = trim_upper(line);
        if (entry.empty()) continue;
        // "CALLSIGN:ABC123" forces a callsign that would otherwise read as an address.
        if (entry.starts_with("CALLSIGN:")) {
            entry = trim_upper(std::string_view(entry).substr(9));
            if (!entry.empty()) out.callsigns_.insert(entry);
            continue;
        }
        if (entry.size() == 6 && std::all_of(entry.begin(), entry.end(), [](unsigned char c) {
                return std::isxdigit(c) != 0;
            })) {
            out.icao24_.insert(lower(entry));
        } else {
            out.callsigns_.insert(entry);
        }
    }
    return out;
}

OptOutList OptOutList::load(const std::filesystem::path& path) {
    std::ifstream in(path);
    if (!in) return {};
    std::ostringstream ss;
    ss << in.rdbuf();
    return parse(ss.str());
}

bool OptOutList::contains(const StateVector& s) const {
    if (icao24_.count(lower(s.icao24)) != 0) return true;
    return !s.callsign.empty() && callsigns_.count(trim_upper(s.callsign)) != 0;
}

}  // namespace sema
