#include "sema/tracker.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <utility>

namespace sema {
namespace {

constexpr double kPi = 3.14159265358979323846;
constexpr double kDegToRad = kPi / 180.0;

// Velocity in east/north components from speed and track angle.
EnuOffset velocity_components(double speed, double track_deg) {
    return {speed * std::sin(track_deg * kDegToRad), speed * std::cos(track_deg * kDegToRad)};
}

}  // namespace

const char* to_string(TrackState s) {
    switch (s) {
        case TrackState::Tentative: return "tentative";
        case TrackState::Confirmed: return "confirmed";
        case TrackState::Coasting: return "coasting";
    }
    return "unknown";
}

const char* to_string(EndReason r) {
    switch (r) {
        case EndReason::SignalLost: return "signal_lost";
        case EndReason::LeftArea: return "left_area";
        case EndReason::Landed: return "landed";
        case EndReason::Restarted: return "restarted";
    }
    return "unknown";
}

std::size_t horizon_bucket(double dt) {
    std::size_t i = 0;
    while (i < kHorizonEdgesS.size() && dt >= kHorizonEdgesS[i]) ++i;
    return i;
}

std::string horizon_label(std::size_t bucket) {
    auto fmt = [](double s) { return std::to_string(static_cast<int>(s)); };
    if (bucket == 0) return "0-" + fmt(kHorizonEdgesS[0]) + "s";
    if (bucket >= kHorizonEdgesS.size()) return ">=" + fmt(kHorizonEdgesS.back()) + "s";
    return fmt(kHorizonEdgesS[bucket - 1]) + "-" + fmt(kHorizonEdgesS[bucket]) + "s";
}

std::uint64_t TrackerMetrics::signal_losses() const {
    return reacquisitions + ended_by_reason[static_cast<std::size_t>(EndReason::SignalLost)];
}

double TrackerMetrics::break_rate() const {
    return tracks_created == 0 ? 0.0 : static_cast<double>(track_breaks) / static_cast<double>(tracks_created);
}

nlohmann::json TrackerMetrics::to_json() const {
    nlohmann::json by_horizon = nlohmann::json::object();
    for (std::size_t i = 0; i < kHorizonBuckets; ++i) {
        if (ct_error_by_horizon_m[i].count() == 0) continue;
        by_horizon[horizon_label(i)] = {{"ct", ct_error_by_horizon_m[i].to_json()},
                                        {"cv", cv_error_by_horizon_m[i].to_json()},
                                        {"dead_reckoning", dead_reckoning_error_by_horizon_m[i].to_json()}};
    }
    nlohmann::json ended = nlohmann::json::object();
    for (int r = 0; r < 4; ++r) ended[to_string(static_cast<EndReason>(r))] = ended_by_reason[static_cast<std::size_t>(r)];

    return {
        {"snapshots", snapshots},
        {"measurements", measurements},
        {"updates", updates},
        {"stale", stale},
        {"tracks",
         {{"created", tracks_created},
          {"confirmed", tracks_confirmed},
          {"ended", ended},
          {"coasting_started", coasting_started},
          {"signal_losses", signal_losses()},
          {"reacquisitions", reacquisitions},
          {"breaks", track_breaks},
          {"break_rate", std::round(break_rate() * 10000.0) / 10000.0},
          {"reacquire_gap_s", reacquire_gap_s.to_json()}}},
        {"quality",
         {{"invalid_position", invalid_position},
          {"impossible_speed", impossible_speed},
          {"teleport", teleport},
          {"bad_altitude", bad_altitude},
          {"bad_vertical_rate", bad_vertical_rate}}},
        {"prediction_error_m",
         {{"ct", ct_error_m.to_json()},
          {"cv", cv_error_m.to_json()},
          {"dead_reckoning", dead_reckoning_error_m.to_json()},
          {"hold_last_position", hold_error_m.to_json()},
          {"by_horizon", by_horizon},
          {"altitude", altitude_error_m.to_json()}}},
        {"fix_interval_s", fix_interval_s.to_json()},
        {"tracked_per_snapshot", tracked_per_snapshot.to_json()},
        {"processing_us",
         {{"per_snapshot", processing_us_per_snapshot.to_json()},
          {"per_measurement", processing_us_per_measurement.to_json(3)}}},
    };
}

Tracker::Tracker(const TrackerConfig& config, std::function<bool(LatLon)> in_area)
    : cfg_(config), in_area_(std::move(in_area)) {}

HorizontalFilter::Measurement Tracker::to_filter_measurement(const Track& t, const Measurement& m) const {
    const EnuOffset d = to_local(t.origin, m.pos);
    HorizontalFilter::Measurement z;
    z.east = d.east;
    z.north = d.north;
    const std::size_t src = std::clamp(m.position_source, 0, 3);
    const double base = cfg_.pos_sigma_m[src];
    double speed = 0.0;
    if (m.speed) {
        speed = *m.speed;
    } else if (t.updates > 0) {
        speed = std::hypot(t.h.x()(2, 0), t.h.x()(3, 0));
    }
    const double quant = speed * cfg_.time_resolution_s / std::sqrt(12.0);
    z.pos_sigma = std::sqrt(base * base + quant * quant);
    if (m.speed && m.track_deg) {
        const EnuOffset v = velocity_components(*m.speed, *m.track_deg);
        z.v_east = v.east;
        z.v_north = v.north;
        const double cross = *m.speed * cfg_.track_sigma_deg * kDegToRad;
        z.vel_sigma = std::sqrt(cfg_.speed_sigma_ms * cfg_.speed_sigma_ms + cross * cross);
    }
    return z;
}

TurnFilter::Measurement Tracker::to_turn_measurement(const HorizontalFilter::Measurement& z,
                                                     const Measurement& m) const {
    TurnFilter::Measurement out{z.east, z.north, z.pos_sigma, std::nullopt, std::nullopt, 0.0, 0.0};
    if (m.speed && m.track_deg) {
        out.speed = m.speed;
        out.track_rad = *m.track_deg * kDegToRad;
        out.speed_sigma = cfg_.speed_sigma_ms;
        out.track_sigma = cfg_.track_sigma_deg * kDegToRad;
    }
    return out;
}

Tracker::Track Tracker::start_track(const StateVector& s, const Measurement& m) {
    Track t;
    t.id = next_id_++;
    t.icao24 = s.icao24;
    t.callsign = s.callsign;
    t.origin = m.pos;
    const auto z = to_filter_measurement(t, m);
    t.h = HorizontalFilter(z, cfg_.initial_speed_sigma_ms, cfg_.accel_psd);
    if (m.speed && m.track_deg) t.ct = TurnFilter(to_turn_measurement(z, m), cfg_.turn_noise);
    if (m.altitude) t.v = VerticalFilter(*m.altitude, m.vertical_rate, cfg_.altitude_sigma_m, cfg_.vertical_accel_psd);
    t.filter_time = m.time;
    t.last_fix = {m.time, m.pos, m.altitude, m.time};
    t.last_speed = m.speed;
    t.last_track_deg = m.track_deg;
    t.on_ground = m.on_ground;
    t.updates = 1;
    t.state = cfg_.confirm_updates <= 1 ? TrackState::Confirmed : TrackState::Tentative;
    if (t.state == TrackState::Confirmed) ++metrics_.tracks_confirmed;
    t.last_snapshot = snapshot_index_;
    ++metrics_.tracks_created;

    if (auto it = recent_ends_.find(s.icao24); it != recent_ends_.end()) {
        if (it->second.reason == EndReason::SignalLost && m.time - it->second.time <= cfg_.break_window_s) {
            ++metrics_.track_breaks;
        }
        recent_ends_.erase(it);
    }
    return t;
}

void Tracker::record_prediction_error(const Track& t, const Measurement& m) {
    // Only confirmed tracks with an established velocity, so all four
    // predictors are compared on the same fixes.
    if (t.updates < cfg_.confirm_updates || !t.ct) return;
    const double dt = m.time - t.filter_time;
    if (dt <= 0.0) return;

    HorizontalFilter h = t.h;
    h.predict(dt);
    const double cv_err = haversine_m(from_local(t.origin, {h.x()(0, 0), h.x()(1, 0)}), m.pos);
    TurnFilter ct = *t.ct;
    ct.predict(dt);
    const double ct_err = haversine_m(from_local(t.origin, {ct.x()(0, 0), ct.x()(1, 0)}), m.pos);

    const double dt_fix = m.time - t.last_fix.time;
    LatLon dr = t.last_fix.pos;
    if (t.last_speed && t.last_track_deg) {
        const EnuOffset v = velocity_components(*t.last_speed, *t.last_track_deg);
        dr = from_local(t.last_fix.pos, {v.east * dt_fix, v.north * dt_fix});
    }
    const double dr_err = haversine_m(dr, m.pos);

    metrics_.ct_error_m.add(ct_err);
    metrics_.cv_error_m.add(cv_err);
    metrics_.dead_reckoning_error_m.add(dr_err);
    metrics_.hold_error_m.add(haversine_m(t.last_fix.pos, m.pos));
    const std::size_t b = horizon_bucket(dt);
    metrics_.ct_error_by_horizon_m[b].add(ct_err);
    metrics_.cv_error_by_horizon_m[b].add(cv_err);
    metrics_.dead_reckoning_error_by_horizon_m[b].add(dr_err);
    metrics_.fix_interval_s.add(dt_fix);

    if (t.v && m.altitude) {
        VerticalFilter v = *t.v;
        v.predict(dt);
        metrics_.altitude_error_m.add(std::abs(v.altitude() - *m.altitude));
    }
}

void Tracker::update_track(Track& t, const StateVector& s, const Measurement& m) {
    const double dt = m.time - t.filter_time;
    const auto z = to_filter_measurement(t, m);
    t.h.predict(dt);
    t.h.update(z);
    if (t.ct) {
        t.ct->predict(dt);
        t.ct->update(to_turn_measurement(z, m));
    } else if (m.speed && m.track_deg) {
        t.ct = TurnFilter(to_turn_measurement(z, m), cfg_.turn_noise);
    }
    // Re-centre the local frame on the new estimate so it stays accurate.
    const double e = t.h.x()(0, 0);
    const double n = t.h.x()(1, 0);
    t.origin = from_local(t.origin, {e, n});
    t.h.shift_origin(e, n);
    if (t.ct) t.ct->shift_origin(e, n);

    if (m.on_ground) {
        t.v.reset();
    } else if (m.altitude) {
        if (t.v) {
            t.v->predict(dt);
            t.v->update(*m.altitude, m.vertical_rate, cfg_.altitude_sigma_m, cfg_.vertical_rate_sigma_ms);
        } else {
            t.v = VerticalFilter(*m.altitude, m.vertical_rate, cfg_.altitude_sigma_m, cfg_.vertical_accel_psd);
        }
    } else if (t.v) {
        t.v->predict(dt);
    }

    t.filter_time = m.time;
    t.last_fix.time = m.time;
    t.last_fix.pos = m.pos;
    if (m.altitude) {
        t.last_fix.altitude = m.altitude;
        t.last_fix.altitude_time = m.time;
    }
    t.last_speed = m.speed;
    t.last_track_deg = m.track_deg;
    t.on_ground = m.on_ground;
    if (!s.callsign.empty()) t.callsign = s.callsign;
    ++t.updates;
    t.state = t.updates >= cfg_.confirm_updates ? TrackState::Confirmed : TrackState::Tentative;
    if (t.updates == cfg_.confirm_updates) ++metrics_.tracks_confirmed;
    t.last_snapshot = snapshot_index_;
    ++metrics_.updates;
}

void Tracker::end_track(const Track& t, EndReason reason, double now, SnapshotReport& report) {
    ++metrics_.ended_by_reason[static_cast<std::size_t>(reason)];
    report.ended.push_back({t.id, t.icao24, reason, now, t.last_fix.time, t.updates});
    if (reason != EndReason::Restarted) recent_ends_[t.icao24] = {reason, now};
}

SnapshotReport Tracker::process(const StatesSnapshot& snapshot) {
    const auto started = std::chrono::steady_clock::now();
    ++snapshot_index_;
    ++metrics_.snapshots;
    const auto now = static_cast<double>(snapshot.time);
    SnapshotReport report;
    report.time = now;

    auto count_issues = [this](std::uint32_t issues) {
        if ((issues & kIssueInvalidPosition) != 0) ++metrics_.invalid_position;
        if ((issues & kIssueImpossibleSpeed) != 0) ++metrics_.impossible_speed;
        if ((issues & kIssueTeleport) != 0) ++metrics_.teleport;
        if ((issues & kIssueBadAltitude) != 0) ++metrics_.bad_altitude;
        if ((issues & kIssueBadVerticalRate) != 0) ++metrics_.bad_vertical_rate;
    };

    for (const auto& s : snapshot.states) {
        if (!s.has_position()) continue;
        ++report.measurements;
        ++metrics_.measurements;

        auto it = tracks_.find(s.icao24);
        if (it == tracks_.end()) {
            const Measurement m = check_measurement(s, nullptr, cfg_.limits);
            count_issues(m.issues);
            if (!m.position_usable()) {
                ++report.rejected;
                continue;
            }
            tracks_.emplace(s.icao24, start_track(s, m));
            ++report.created;
            continue;
        }

        Track& t = it->second;
        const auto fix_time = static_cast<double>(s.time_position.value_or(s.last_contact));
        if (fix_time <= t.last_fix.time) {
            // OpenSky repeats the last known position until a new one arrives.
            ++report.stale;
            ++metrics_.stale;
            continue;
        }

        const Measurement m = check_measurement(s, &t.last_fix, cfg_.limits);
        count_issues(m.issues);
        if (!m.position_usable()) {
            ++report.rejected;
            if ((m.issues & kIssueTeleport) == 0) continue;
            const bool consistent_with_pending =
                t.pending && plausible_jump(*t.pending, m.time, m.pos, cfg_.limits);
            t.consecutive_rejects = consistent_with_pending ? t.consecutive_rejects + 1 : 1;
            t.pending = PreviousFix{m.time, m.pos, m.altitude, m.time};
            if (t.consecutive_rejects >= cfg_.restart_after_rejects) {
                // The track, not the data, was wrong (or the address was reused):
                // start again from the new, self-consistent fixes.
                end_track(t, EndReason::Restarted, now, report);
                tracks_.erase(it);
                const Measurement fresh = check_measurement(s, nullptr, cfg_.limits);
                tracks_.emplace(s.icao24, start_track(s, fresh));
                ++report.created;
            }
            continue;
        }
        t.consecutive_rejects = 0;
        t.pending.reset();

        if (t.state == TrackState::Coasting) {
            ++report.reacquired;
            ++metrics_.reacquisitions;
            metrics_.reacquire_gap_s.add(m.time - t.last_fix.time);
        }
        record_prediction_error(t, m);
        update_track(t, s, m);
        ++report.updates;
    }

    // Tracks without a new fix in this snapshot coast or end.
    for (auto it = tracks_.begin(); it != tracks_.end();) {
        Track& t = it->second;
        if (t.last_snapshot == snapshot_index_) {
            ++it;
            continue;
        }
        if (now - t.last_fix.time > cfg_.terminate_after_s) {
            EndReason reason = EndReason::SignalLost;
            const auto last = view(t, t.last_fix.time);
            const bool low_descending =
                last.altitude_m && *last.altitude_m < 1200.0 && last.vertical_rate_ms.value_or(0.0) < -1.0;
            if (t.on_ground || low_descending) {
                reason = EndReason::Landed;
            } else if (in_area_) {
                // Where would it be one typical polling interval after the last fix?
                const auto ahead = view(t, t.last_fix.time + 90.0);
                if (!in_area_(ahead.pos)) reason = EndReason::LeftArea;
            }
            end_track(t, reason, now, report);
            it = tracks_.erase(it);
            continue;
        }
        if (t.state != TrackState::Coasting) {
            if (t.state == TrackState::Confirmed) ++metrics_.coasting_started;
            t.state = TrackState::Coasting;
        }
        ++it;
    }

    for (auto it = recent_ends_.begin(); it != recent_ends_.end();) {
        it = (now - it->second.time > cfg_.break_window_s) ? recent_ends_.erase(it) : std::next(it);
    }

    report.tracked = tracks_.size();
    for (const auto& [_, t] : tracks_) {
        if (t.state == TrackState::Confirmed) ++report.confirmed;
        if (t.state == TrackState::Coasting) ++report.coasting;
    }
    metrics_.tracked_per_snapshot.add(static_cast<double>(report.tracked));

    const auto elapsed = std::chrono::duration<double, std::micro>(std::chrono::steady_clock::now() - started);
    report.processing_us = elapsed.count();
    metrics_.processing_us_per_snapshot.add(report.processing_us);
    if (report.measurements > 0) {
        metrics_.processing_us_per_measurement.add(report.processing_us / static_cast<double>(report.measurements));
    }
    return report;
}

TrackView Tracker::view(const Track& t, double time) const {
    TrackView out;
    out.id = t.id;
    out.icao24 = t.icao24;
    out.callsign = t.callsign;
    out.state = t.state;
    out.time = time;
    out.last_fix_time = t.last_fix.time;
    out.on_ground = t.on_ground;
    out.updates = t.updates;

    const double dt = time - t.filter_time;
    // A landed aircraft is not extrapolated across the airport.
    const double pdt = t.on_ground ? 0.0 : dt;
    double trk = 0.0;
    if (t.ct) out.turn_rate_dps = t.ct->x()(4, 0) / kDegToRad;
    if (t.ct && cfg_.model == MotionModel::CoordinatedTurn) {
        TurnFilter ct = *t.ct;
        ct.predict(pdt);
        const auto& x = ct.x();
        out.pos = from_local(t.origin, {x(0, 0), x(1, 0)});
        out.speed_ms = x(2, 0);
        trk = x(3, 0) / kDegToRad;
        out.pos_sigma_m = std::sqrt((ct.P()(0, 0) + ct.P()(1, 1)) / 2.0);
    } else {
        HorizontalFilter h = t.h;
        h.predict(pdt);
        const auto& x = h.x();
        out.pos = from_local(t.origin, {x(0, 0), x(1, 0)});
        out.speed_ms = std::hypot(x(2, 0), x(3, 0));
        trk = std::atan2(x(2, 0), x(3, 0)) / kDegToRad;
        out.pos_sigma_m = std::sqrt((h.P()(0, 0) + h.P()(1, 1)) / 2.0);
    }
    if (trk < 0.0) trk += 360.0;
    out.track_deg = trk;
    if (t.v) {
        VerticalFilter v = *t.v;
        v.predict(dt);
        out.altitude_m = v.altitude();
        out.vertical_rate_ms = v.vertical_rate();
    }
    return out;
}

std::vector<TrackView> Tracker::tracks_at(double t) const {
    std::vector<TrackView> out;
    out.reserve(tracks_.size());
    for (const auto& [_, track] : tracks_) out.push_back(view(track, t));
    std::sort(out.begin(), out.end(), [](const TrackView& a, const TrackView& b) { return a.id < b.id; });
    return out;
}

std::optional<TrackView> Tracker::track_at(const std::string& icao24, double t) const {
    auto it = tracks_.find(icao24);
    if (it == tracks_.end()) return std::nullopt;
    return view(it->second, t);
}

}  // namespace sema
