#pragma once

#include <filesystem>
#include <istream>
#include <optional>
#include <string>
#include <unordered_map>

namespace sema {

// Subset of the OpenSky aircraft database relevant to filtering.
struct AircraftInfo {
    std::string icao24;
    std::string registration;
    std::string operator_name;
    std::string operator_icao;
    std::string operator_callsign;
    std::string owner;
    std::string model;
};

class AircraftRegistry {
public:
    // Reads the OpenSky aircraft database CSV (old lower-case header or the newer
    // camelCase header; single or double quoted). Unknown columns are ignored.
    static AircraftRegistry from_csv(std::istream& in);
    static AircraftRegistry load_csv(const std::filesystem::path& path);

    void add(AircraftInfo info);
    [[nodiscard]] const AircraftInfo* find(std::string_view icao24) const;
    [[nodiscard]] std::size_t size() const { return by_icao_.size(); }

private:
    std::unordered_map<std::string, AircraftInfo> by_icao_;
};

}  // namespace sema
