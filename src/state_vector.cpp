#include "sema/state_vector.hpp"

#include <algorithm>
#include <cctype>
#include <stdexcept>

#include <nlohmann/json.hpp>

namespace sema {
namespace {

using nlohmann::json;

std::optional<double> opt_double(const json& v) {
    if (v.is_number()) return v.get<double>();
    return std::nullopt;
}

std::optional<std::int64_t> opt_int(const json& v) {
    if (v.is_number_integer()) return v.get<std::int64_t>();
    if (v.is_number()) return static_cast<std::int64_t>(v.get<double>());
    return std::nullopt;
}

std::string opt_string(const json& v) { return v.is_string() ? v.get<std::string>() : std::string{}; }

std::string trim(std::string s) {
    auto not_space = [](unsigned char c) { return !std::isspace(c); };
    s.erase(s.begin(), std::find_if(s.begin(), s.end(), not_space));
    s.erase(std::find_if(s.rbegin(), s.rend(), not_space).base(), s.end());
    return s;
}

bool is_hex6(const std::string& s) {
    return s.size() == 6 && std::all_of(s.begin(), s.end(), [](unsigned char c) { return std::isxdigit(c) != 0; });
}

// Returns nullopt for rows that cannot be used at all.
std::optional<StateVector> parse_row(const json& row) {
    if (!row.is_array() || row.size() < 17) return std::nullopt;
    if (!row[0].is_string()) return std::nullopt;

    StateVector s;
    s.icao24 = trim(row[0].get<std::string>());
    std::transform(s.icao24.begin(), s.icao24.end(), s.icao24.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    if (!is_hex6(s.icao24)) return std::nullopt;

    s.callsign = trim(opt_string(row[1]));
    s.origin_country = opt_string(row[2]);
    s.time_position = opt_int(row[3]);
    auto last = opt_int(row[4]);
    if (!last) return std::nullopt;
    s.last_contact = *last;
    s.longitude = opt_double(row[5]);
    s.latitude = opt_double(row[6]);
    s.baro_altitude = opt_double(row[7]);
    s.on_ground = row[8].is_boolean() && row[8].get<bool>();
    s.velocity = opt_double(row[9]);
    s.true_track = opt_double(row[10]);
    s.vertical_rate = opt_double(row[11]);
    s.geo_altitude = opt_double(row[13]);
    s.squawk = trim(opt_string(row[14]));
    s.spi = row[15].is_boolean() && row[15].get<bool>();
    s.position_source = row[16].is_number_integer() ? row[16].get<int>() : 0;

    if (s.latitude && (*s.latitude < -90.0 || *s.latitude > 90.0)) return std::nullopt;
    if (s.longitude && (*s.longitude < -180.0 || *s.longitude > 180.0)) return std::nullopt;
    return s;
}

}  // namespace

ParseResult parse_states(std::string_view text) {
    json doc;
    try {
        doc = json::parse(text);
    } catch (const json::parse_error& e) {
        throw std::runtime_error(std::string("states: invalid JSON: ") + e.what());
    }
    if (!doc.is_object()) throw std::runtime_error("states: top level is not an object");

    ParseResult out;
    if (auto it = doc.find("time"); it != doc.end() && it->is_number()) {
        out.snapshot.time = it->get<std::int64_t>();
    } else {
        throw std::runtime_error("states: missing 'time'");
    }

    auto it = doc.find("states");
    if (it == doc.end() || it->is_null()) return out;  // no aircraft in the box
    if (!it->is_array()) throw std::runtime_error("states: 'states' is not an array");

    out.snapshot.states.reserve(it->size());
    for (const auto& row : *it) {
        if (auto s = parse_row(row)) {
            out.snapshot.states.push_back(std::move(*s));
        } else {
            ++out.skipped_rows;
        }
    }
    return out;
}

}  // namespace sema
