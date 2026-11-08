#pragma once

#include <array>
#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>
#include <unordered_set>

#include <nlohmann/json_fwd.hpp>

#include "sema/state_vector.hpp"

namespace sema {

// What the public site (WebSocket feed and /api/*) shows about each aircraft.
// The database and the local log keep the real values; this only changes what
// leaves the server. OpenSky's terms ask for aircraft identifiers to be
// anonymised before data is published, so the default is Hidden.
enum class IdentifierMode {
    Hidden,   // ICAO24 replaced by a pseudonym, no callsign
    Airline,  // pseudonym, callsign only when it is an airline flight number (THY7, PGT2AB)
    Full,     // real ICAO24 and callsign (only with OpenSky's written permission)
};

const char* to_string(IdentifierMode m);
// "hidden", "airline" or "full"; throws std::invalid_argument otherwise.
IdentifierMode parse_identifier_mode(std::string_view s);

// True for ICAO airline callsigns: three-letter operator designator, a digit,
// then up to three letters/digits. Registrations used as callsigns (TCABC,
// N123AB, DEABC) are not, so private aircraft stay unnamed in Airline mode.
bool is_airline_callsign(std::string_view callsign);

// Pseudonyms start with this character; the web map uses it to tell them apart.
inline constexpr char kPseudonymPrefix = '~';

// Replaces aircraft identifiers in outgoing messages. A pseudonym is a keyed
// hash (SipHash-2-4) of the address and the local day, with a random key made
// at start-up and never written anywhere: it is stable for one day, so the map
// can follow an aircraft, but cannot be turned back into the address or linked
// across days or restarts.
class IdentityMasker {
public:
    explicit IdentityMasker(IdentifierMode mode);
    IdentityMasker(IdentifierMode mode, std::array<std::uint64_t, 2> key);

    [[nodiscard]] IdentifierMode mode() const { return mode_; }

    [[nodiscard]] std::string icao24(std::string_view real, double time) const;
    [[nodiscard]] std::string callsign(std::string_view callsign) const;

    // Masks the "aircraft" list (live snapshots, /api/replay) and the "events"
    // list (anomaly events) of one outgoing message in place. The day comes
    // from the message's own time field.
    void apply(nlohmann::json& message) const;

private:
    IdentifierMode mode_;
    std::array<std::uint64_t, 2> key_;
};

// Aircraft whose owners asked not to be shown. They are dropped together with
// military aircraft, before anything is stored or shown. One entry per line:
// a 6 digit hex ICAO24 address or a callsign; '#' starts a comment.
class OptOutList {
public:
    OptOutList() = default;
    static OptOutList parse(std::string_view text);
    // A missing file is an empty list.
    static OptOutList load(const std::filesystem::path& path);

    [[nodiscard]] bool contains(const StateVector& s) const;
    [[nodiscard]] std::size_t size() const { return icao24_.size() + callsigns_.size(); }

private:
    std::unordered_set<std::string> icao24_;     // lower case
    std::unordered_set<std::string> callsigns_;  // upper case, trimmed
};

}  // namespace sema
