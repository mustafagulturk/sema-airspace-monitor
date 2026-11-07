#pragma once

#include <cstdint>
#include <filesystem>
#include <optional>
#include <regex>
#include <string>
#include <vector>

#include "sema/aircraft_registry.hpp"
#include "sema/state_vector.hpp"

namespace sema {

// Why an aircraft was classified as military/government.
enum class ExclusionReason {
    IcaoBlock,          // transponder address in a known military block
    CallsignPattern,    // callsign matches a military/state pattern
    OperatorKeyword,    // registry operator/owner names an armed force or state body
    OperatorDesignator, // registry operator ICAO designator is a military one
};

const char* to_string(ExclusionReason r);

struct IcaoBlock {
    std::uint32_t first;
    std::uint32_t last;
    std::string note;
};

struct MilitaryRules {
    std::vector<IcaoBlock> icao_blocks;
    std::vector<std::string> callsign_patterns;   // ECMAScript regex, matched on the full callsign
    std::vector<std::string> operator_keywords;   // case-insensitive substring
    std::vector<std::string> operator_designators;// exact ICAO operator designators

    static MilitaryRules from_json(const std::string& json);
    static MilitaryRules load(const std::filesystem::path& path);
};

// Decides whether an aircraft must be discarded before storage or display.
// The filter is deliberately conservative: any single matching rule drops it.
class MilitaryFilter {
public:
    MilitaryFilter(MilitaryRules rules, const AircraftRegistry* registry);

    [[nodiscard]] std::optional<ExclusionReason> classify(const StateVector& s) const;

private:
    MilitaryRules rules_;
    std::vector<std::regex> callsign_res_;
    std::vector<std::string> keywords_lower_;
    const AircraftRegistry* registry_;
};

// Parses a 6 digit hex ICAO 24-bit address; nullopt if malformed.
std::optional<std::uint32_t> parse_icao24(std::string_view hex);

}  // namespace sema
