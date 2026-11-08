#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "sema/geo.hpp"
#include "sema/state_vector.hpp"

namespace sema {

// Physical limits used to reject corrupt reports. Defaults are generous for
// civil traffic so that real aircraft are never rejected; see README.
struct QualityLimits {
    // Reported ground speed above this is treated as corrupt (~Mach 1 at cruise
    // altitude; no civil aircraft over Turkey flies this fast over the ground).
    double max_ground_speed_ms = 340.0;
    // Position jumps implying more than this speed are "teleports".
    double max_implied_speed_ms = 400.0;
    // Distance always allowed between two fixes, covering position noise and
    // OpenSky's one-second timestamp resolution.
    double position_slack_m = 500.0;
    double min_altitude_m = -400.0;     // below the Dead Sea; anything lower is corrupt
    double max_altitude_m = 15500.0;    // ~FL510, the business-jet ceiling
    double max_vertical_rate_ms = 125.0;  // ~24,600 ft/min
    double altitude_slack_m = 150.0;
};

enum QualityIssue : std::uint32_t {
    kIssueNone = 0,
    kIssueInvalidPosition = 1U << 0,  // NaN / out of range
    kIssueImpossibleSpeed = 1U << 1,  // reported velocity above max_ground_speed_ms
    kIssueTeleport = 1U << 2,         // position jump faster than max_implied_speed_ms
    kIssueBadAltitude = 1U << 3,      // altitude out of range or jumping too fast
    kIssueBadVerticalRate = 1U << 4,  // reported vertical rate out of range
};

std::vector<std::string> issue_names(std::uint32_t issues);

// Last accepted fix of a track, the reference for jump checks.
struct PreviousFix {
    double time;
    LatLon pos;
    std::optional<double> altitude;
    double altitude_time = 0.0;
};

// One report after quality checks. Fields that failed a check are cleared
// (e.g. a corrupt altitude is dropped but the position is still used).
struct Measurement {
    double time = 0.0;  // time of the position fix, Unix seconds
    LatLon pos{};
    std::optional<double> speed;      // m/s over ground
    std::optional<double> track_deg;  // clockwise from north
    std::optional<double> altitude;   // barometric, metres
    std::optional<double> vertical_rate;
    bool on_ground = false;
    int position_source = 0;
    std::uint32_t issues = kIssueNone;

    // False if the position itself must not be used (invalid or teleport).
    [[nodiscard]] bool position_usable() const {
        return (issues & (kIssueInvalidPosition | kIssueTeleport)) == 0;
    }
};

// Builds a measurement from a state vector with a position and runs all
// checks. `prev` is the track's last accepted fix, if any.
Measurement check_measurement(const StateVector& s, const PreviousFix* prev, const QualityLimits& limits);

// True if `b` is physically reachable from `a` under `limits` (used by the
// teleport check and when deciding whether to restart a track).
bool plausible_jump(const PreviousFix& a, double t, LatLon pos, const QualityLimits& limits);

}  // namespace sema
