#include "sema/military_filter.hpp"

#include <algorithm>
#include <cctype>
#include <fstream>
#include <sstream>
#include <stdexcept>

#include <nlohmann/json.hpp>

namespace sema {
namespace {

std::string upper_ascii(std::string_view s) {
    std::string out(s);
    std::transform(out.begin(), out.end(), out.begin(),
                   [](unsigned char c) { return static_cast<char>(std::toupper(c)); });
    return out;
}

// Lower-cases ASCII and the Turkish letters std::tolower cannot handle in UTF-8
// (İ->i, I stays i, Ş->ş, Ğ->ğ, Ü->ü, Ö->ö, Ç->ç), so "HAVA KUVVETLERİ" and
// "Hava Kuvvetleri" compare equal.
std::string fold_case(std::string_view s) {
    static const std::pair<std::string_view, std::string_view> kMap[] = {
        {"\xC4\xB0", "i"},          // İ
        {"\xC4\xB1", "i"},          // ı (folded to i so that "HAVA" and "hava" both match)
        {"\xC5\x9E", "\xC5\x9F"},   // Ş -> ş
        {"\xC4\x9E", "\xC4\x9F"},   // Ğ -> ğ
        {"\xC3\x9C", "\xC3\xBC"},   // Ü -> ü
        {"\xC3\x96", "\xC3\xB6"},   // Ö -> ö
        {"\xC3\x87", "\xC3\xA7"},   // Ç -> ç
    };
    std::string out;
    out.reserve(s.size());
    for (std::size_t i = 0; i < s.size();) {
        bool replaced = false;
        for (const auto& [from, to] : kMap) {
            if (s.compare(i, from.size(), from) == 0) {
                out.append(to);
                i += from.size();
                replaced = true;
                break;
            }
        }
        if (!replaced) {
            out.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(s[i]))));
            ++i;
        }
    }
    return out;
}

bool contains_any(const std::string& haystack, const std::vector<std::string>& needles) {
    if (haystack.empty()) return false;
    return std::any_of(needles.begin(), needles.end(),
                       [&](const std::string& n) { return haystack.find(n) != std::string::npos; });
}

}  // namespace

const char* to_string(ExclusionReason r) {
    switch (r) {
        case ExclusionReason::IcaoBlock: return "icao_block";
        case ExclusionReason::CallsignPattern: return "callsign_pattern";
        case ExclusionReason::OperatorKeyword: return "operator_keyword";
        case ExclusionReason::OperatorDesignator: return "operator_designator";
    }
    return "unknown";
}

std::optional<std::uint32_t> parse_icao24(std::string_view hex) {
    if (hex.size() != 6) return std::nullopt;
    std::uint32_t v = 0;
    for (char c : hex) {
        v <<= 4;
        if (c >= '0' && c <= '9') v |= static_cast<std::uint32_t>(c - '0');
        else if (c >= 'a' && c <= 'f') v |= static_cast<std::uint32_t>(c - 'a' + 10);
        else if (c >= 'A' && c <= 'F') v |= static_cast<std::uint32_t>(c - 'A' + 10);
        else return std::nullopt;
    }
    return v;
}

MilitaryRules MilitaryRules::from_json(const std::string& json) {
    const auto j = nlohmann::json::parse(json);
    MilitaryRules r;
    for (const auto& b : j.value("icao_blocks", nlohmann::json::array())) {
        const auto from = parse_icao24(b.at("from").get<std::string>());
        const auto to = parse_icao24(b.at("to").get<std::string>());
        if (!from || !to || *from > *to) {
            throw std::runtime_error("invalid icao block: " + b.dump());
        }
        r.icao_blocks.push_back({*from, *to, b.value("note", "")});
    }
    auto strings = [&](const char* key) {
        std::vector<std::string> out;
        for (const auto& v : j.value(key, nlohmann::json::array())) out.push_back(v.get<std::string>());
        return out;
    };
    r.callsign_patterns = strings("callsign_patterns");
    r.operator_keywords = strings("operator_keywords");
    r.operator_designators = strings("operator_designators");
    return r;
}

MilitaryRules MilitaryRules::load(const std::filesystem::path& path) {
    std::ifstream in(path);
    if (!in) throw std::runtime_error("cannot open military rules: " + path.string());
    std::ostringstream ss;
    ss << in.rdbuf();
    return from_json(ss.str());
}

MilitaryFilter::MilitaryFilter(MilitaryRules rules, const AircraftRegistry* registry)
    : rules_(std::move(rules)), registry_(registry) {
    for (const auto& p : rules_.callsign_patterns) {
        callsign_res_.emplace_back(p, std::regex::ECMAScript | std::regex::optimize);
    }
    for (const auto& k : rules_.operator_keywords) keywords_lower_.push_back(fold_case(k));
    for (auto& d : rules_.operator_designators) d = upper_ascii(d);
}

std::optional<ExclusionReason> MilitaryFilter::classify(const StateVector& s) const {
    if (const auto addr = parse_icao24(s.icao24)) {
        for (const auto& b : rules_.icao_blocks) {
            if (*addr >= b.first && *addr <= b.last) return ExclusionReason::IcaoBlock;
        }
    }

    if (!s.callsign.empty()) {
        const auto cs = upper_ascii(s.callsign);
        for (const auto& re : callsign_res_) {
            if (std::regex_match(cs, re)) return ExclusionReason::CallsignPattern;
        }
    }

    if (registry_ != nullptr) {
        if (const AircraftInfo* info = registry_->find(s.icao24)) {
            const auto designator = upper_ascii(info->operator_icao);
            if (!designator.empty() &&
                std::find(rules_.operator_designators.begin(), rules_.operator_designators.end(),
                          designator) != rules_.operator_designators.end()) {
                return ExclusionReason::OperatorDesignator;
            }
            if (contains_any(fold_case(info->operator_name), keywords_lower_) ||
                contains_any(fold_case(info->owner), keywords_lower_) ||
                contains_any(fold_case(info->operator_callsign), keywords_lower_)) {
                return ExclusionReason::OperatorKeyword;
            }
        }
    }
    return std::nullopt;
}

}  // namespace sema
