#include "sema/quality.hpp"

#include <algorithm>
#include <cmath>

namespace sema {

std::vector<std::string> issue_names(std::uint32_t issues) {
    std::vector<std::string> out;
    if ((issues & kIssueInvalidPosition) != 0) out.emplace_back("invalid_position");
    if ((issues & kIssueImpossibleSpeed) != 0) out.emplace_back("impossible_speed");
    if ((issues & kIssueTeleport) != 0) out.emplace_back("teleport");
    if ((issues & kIssueBadAltitude) != 0) out.emplace_back("bad_altitude");
    if ((issues & kIssueBadVerticalRate) != 0) out.emplace_back("bad_vertical_rate");
    return out;
}

bool plausible_jump(const PreviousFix& a, double t, LatLon pos, const QualityLimits& limits) {
    const double dt = std::max(0.0, t - a.time);
    const double dist = haversine_m(a.pos, pos);
    return dist <= limits.position_slack_m + limits.max_implied_speed_ms * dt;
}

Measurement check_measurement(const StateVector& s, const PreviousFix* prev, const QualityLimits& limits) {
    Measurement m;
    m.time = static_cast<double>(s.time_position.value_or(s.last_contact));
    m.on_ground = s.on_ground;
    m.position_source = s.position_source;

    const double lat = s.latitude.value_or(std::nan(""));
    const double lon = s.longitude.value_or(std::nan(""));
    if (!std::isfinite(lat) || !std::isfinite(lon) || lat < -90.0 || lat > 90.0 || lon < -180.0 || lon > 180.0) {
        m.issues |= kIssueInvalidPosition;
        return m;
    }
    m.pos = {lat, lon};

    if (s.velocity && s.true_track && std::isfinite(*s.velocity) && std::isfinite(*s.true_track)) {
        if (*s.velocity < 0.0 || *s.velocity > limits.max_ground_speed_ms) {
            m.issues |= kIssueImpossibleSpeed;
        } else {
            m.speed = s.velocity;
            m.track_deg = s.true_track;
        }
    }

    if (prev != nullptr && !plausible_jump(*prev, m.time, m.pos, limits)) {
        m.issues |= kIssueTeleport;
    }

    if (!s.on_ground && s.baro_altitude) {
        const double h = *s.baro_altitude;
        bool ok = std::isfinite(h) && h >= limits.min_altitude_m && h <= limits.max_altitude_m;
        if (ok && prev != nullptr && prev->altitude) {
            const double dt = std::max(0.0, m.time - prev->altitude_time);
            ok = std::abs(h - *prev->altitude) <= limits.altitude_slack_m + limits.max_vertical_rate_ms * dt;
        }
        if (ok) {
            m.altitude = h;
        } else {
            m.issues |= kIssueBadAltitude;
        }
    }

    if (s.vertical_rate) {
        const double vr = *s.vertical_rate;
        if (std::isfinite(vr) && std::abs(vr) <= limits.max_vertical_rate_ms) {
            m.vertical_rate = vr;
        } else {
            m.issues |= kIssueBadVerticalRate;
        }
    }
    return m;
}

}  // namespace sema
