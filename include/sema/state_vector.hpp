#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace sema {

// One aircraft report as returned by OpenSky /states/all.
// Units follow OpenSky: metres, metres/second, degrees, Unix seconds.
struct StateVector {
    std::string icao24;  // lower-case hex, 6 chars
    std::string callsign;  // trimmed, may be empty
    std::string origin_country;
    std::optional<std::int64_t> time_position;
    std::int64_t last_contact = 0;
    std::optional<double> longitude;
    std::optional<double> latitude;
    std::optional<double> baro_altitude;
    bool on_ground = false;
    std::optional<double> velocity;
    std::optional<double> true_track;
    std::optional<double> vertical_rate;
    std::optional<double> geo_altitude;
    std::string squawk;
    bool spi = false;
    int position_source = 0;  // 0 ADS-B, 1 ASTERIX, 2 MLAT, 3 FLARM

    [[nodiscard]] bool has_position() const { return latitude && longitude; }
};

struct StatesSnapshot {
    std::int64_t time = 0;  // snapshot time (Unix seconds)
    std::vector<StateVector> states;
};

// Parses the JSON body of /states/all. Malformed individual rows are skipped
// and counted; a malformed top level throws std::runtime_error.
struct ParseResult {
    StatesSnapshot snapshot;
    std::size_t skipped_rows = 0;
};
ParseResult parse_states(std::string_view json);

}  // namespace sema
