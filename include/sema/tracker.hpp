#pragma once

#include <array>
#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

#include <nlohmann/json.hpp>

#include "sema/geo.hpp"
#include "sema/kalman.hpp"
#include "sema/metrics.hpp"
#include "sema/quality.hpp"
#include "sema/state_vector.hpp"

namespace sema {

// Which filter provides the tracker's position estimate. Both always run, and
// both are scored, so the choice can be made on real data.
enum class MotionModel { ConstantVelocity, CoordinatedTurn };

struct TrackerConfig {
    QualityLimits limits;
    MotionModel model = MotionModel::ConstantVelocity;
    // Constant-velocity Kalman tuning: white-noise acceleration spectral
    // density (m^2/s^3) per horizontal axis.
    double accel_psd = 4.0;
    // Coordinated-turn EKF tuning.
    TurnFilter::Noise turn_noise;
    double vertical_accel_psd = 0.5;
    // 1-sigma position noise per source (OpenSky position_source 0..3).
    std::array<double, 4> pos_sigma_m = {30.0, 150.0, 150.0, 50.0};  // ADS-B, ASTERIX, MLAT, FLARM
    // OpenSky timestamps are whole seconds, so a fix is up to a second away
    // from its timestamp; this adds speed/sqrt(12) metres of position noise.
    double time_resolution_s = 1.0;
    double speed_sigma_ms = 2.0;
    double track_sigma_deg = 1.0;
    double initial_speed_sigma_ms = 150.0;
    double altitude_sigma_m = 15.0;
    double vertical_rate_sigma_ms = 1.0;

    // Lifecycle.
    int confirm_updates = 2;          // tentative -> confirmed after this many fixes
    double terminate_after_s = 600.0;  // no fix for this long -> terminated
    double break_window_s = 1800.0;   // re-appearance within this after a signal-loss end counts as a break
    int restart_after_rejects = 2;    // consecutive mutually consistent rejected fixes -> restart the track
};

enum class TrackState { Tentative, Confirmed, Coasting };
enum class EndReason { SignalLost, LeftArea, Landed, Restarted };

const char* to_string(TrackState s);
const char* to_string(EndReason r);

// A track's state predicted to some time, for display and storage.
struct TrackView {
    std::uint64_t id = 0;
    std::string icao24;
    std::string callsign;
    TrackState state = TrackState::Tentative;
    double time = 0.0;             // time of this estimate
    double last_fix_time = 0.0;    // time of the newest measurement used
    LatLon pos{};
    double speed_ms = 0.0;
    double track_deg = 0.0;
    std::optional<double> turn_rate_dps;  // from the turn EKF, deg/s, positive = right turn
    std::optional<double> altitude_m;
    std::optional<double> vertical_rate_ms;
    double pos_sigma_m = 0.0;      // 1-sigma horizontal uncertainty (RMS of both axes)
    bool on_ground = false;
    int updates = 0;
};

struct EndedTrack {
    std::uint64_t id;
    std::string icao24;
    EndReason reason;
    double end_time;     // snapshot time when ended
    double last_fix_time;
    int updates;
};

struct SnapshotReport {
    double time = 0.0;
    std::size_t measurements = 0;   // state vectors with a position
    std::size_t updates = 0;        // tracks updated with a new fix
    std::size_t stale = 0;          // no new fix since the last snapshot
    std::size_t rejected = 0;       // position rejected by quality checks
    std::size_t created = 0;
    std::size_t reacquired = 0;
    std::vector<EndedTrack> ended;
    std::size_t tracked = 0;        // tentative + confirmed + coasting after this snapshot
    std::size_t confirmed = 0;
    std::size_t coasting = 0;
    double processing_us = 0.0;
};

// Prediction horizons used to bucket prediction errors.
inline constexpr std::array<double, 5> kHorizonEdgesS = {15.0, 45.0, 90.0, 180.0, 600.0};
inline constexpr std::size_t kHorizonBuckets = kHorizonEdgesS.size() + 1;
std::string horizon_label(std::size_t bucket);
std::size_t horizon_bucket(double dt);

struct TrackerMetrics {
    std::uint64_t snapshots = 0;
    std::uint64_t measurements = 0;
    std::uint64_t updates = 0;
    std::uint64_t stale = 0;

    std::uint64_t tracks_created = 0;
    std::uint64_t tracks_confirmed = 0;
    std::uint64_t coasting_started = 0;  // a confirmed track got no new fix in a snapshot
    std::uint64_t reacquisitions = 0;    // a coasting track got a fix again
    std::uint64_t track_breaks = 0;     // a track ended by signal loss and the aircraft came back as a new track
    std::array<std::uint64_t, 4> ended_by_reason{};

    // Quality checks.
    std::uint64_t invalid_position = 0;
    std::uint64_t impossible_speed = 0;
    std::uint64_t teleport = 0;
    std::uint64_t bad_altitude = 0;
    std::uint64_t bad_vertical_rate = 0;

    // Horizontal prediction error (metres) at the time of each new fix, before
    // the fix is used, for confirmed tracks. "cv" is the constant-velocity
    // Kalman filter, "ct" the coordinated-turn EKF.
    // "Dead reckoning" extrapolates the last raw fix along its reported
    // velocity and "hold" assumes the aircraft did not move: baselines that
    // show what the filters add.
    Distribution ct_error_m;
    Distribution cv_error_m;
    Distribution dead_reckoning_error_m;
    Distribution hold_error_m;
    std::array<Distribution, kHorizonBuckets> ct_error_by_horizon_m;
    std::array<Distribution, kHorizonBuckets> cv_error_by_horizon_m;
    std::array<Distribution, kHorizonBuckets> dead_reckoning_error_by_horizon_m;
    Distribution altitude_error_m;
    Distribution fix_interval_s;      // time between consecutive fixes of a track
    Distribution reacquire_gap_s;     // length of gaps that were bridged

    Distribution tracked_per_snapshot;
    Distribution processing_us_per_snapshot;
    Distribution processing_us_per_measurement;

    // Gaps a confirmed track went through: bridged (reacquired) or not (ended
    // as signal lost). Coasting before a landing or leaving the area is not a
    // signal loss.
    [[nodiscard]] std::uint64_t signal_losses() const;
    // Track breaks per created track; 0 if no tracks.
    [[nodiscard]] double break_rate() const;
    [[nodiscard]] nlohmann::json to_json() const;
};

// Multi-aircraft tracker. Tracks are keyed by ICAO 24-bit address, so there is
// no data-association problem; the work is in filtering, lifecycle and
// rejecting bad reports. Not thread safe.
class Tracker {
public:
    // `in_area` (optional) tells whether a position is inside the monitored
    // area; it is used to tell "left the area" apart from "signal lost".
    explicit Tracker(const TrackerConfig& config = {}, std::function<bool(LatLon)> in_area = {});

    // Feeds one OpenSky snapshot (already filtered to civil aircraft in the
    // area). Snapshots must be given in time order.
    SnapshotReport process(const StatesSnapshot& snapshot);

    // All live tracks predicted to time `t` (Unix seconds).
    [[nodiscard]] std::vector<TrackView> tracks_at(double t) const;
    [[nodiscard]] std::optional<TrackView> track_at(const std::string& icao24, double t) const;

    [[nodiscard]] const TrackerMetrics& metrics() const { return metrics_; }
    [[nodiscard]] std::size_t size() const { return tracks_.size(); }

private:
    struct Track {
        std::uint64_t id = 0;
        std::string icao24;
        std::string callsign;
        TrackState state = TrackState::Tentative;
        LatLon origin{};  // frame origin of `h` and `ct`, re-centred on every fix
        HorizontalFilter h;
        std::optional<TurnFilter> ct;  // started on the first fix with speed and track
        std::optional<VerticalFilter> v;
        double filter_time = 0.0;  // time the filters are valid at
        PreviousFix last_fix{};
        std::optional<double> last_speed;
        std::optional<double> last_track_deg;
        bool on_ground = false;
        int updates = 0;
        int consecutive_rejects = 0;
        std::optional<PreviousFix> pending;  // last rejected fix, for restart decisions
        std::uint64_t last_snapshot = 0;     // snapshot index of the last fix
    };

    struct PastEnd {
        EndReason reason;
        double time;
    };

    [[nodiscard]] HorizontalFilter::Measurement to_filter_measurement(const Track& t, const Measurement& m) const;
    [[nodiscard]] TurnFilter::Measurement to_turn_measurement(const HorizontalFilter::Measurement& z,
                                                              const Measurement& m) const;
    Track start_track(const StateVector& s, const Measurement& m);
    void update_track(Track& t, const StateVector& s, const Measurement& m);
    void record_prediction_error(const Track& t, const Measurement& m);
    void end_track(const Track& t, EndReason reason, double now, SnapshotReport& report);
    [[nodiscard]] TrackView view(const Track& t, double time) const;

    TrackerConfig cfg_;
    std::function<bool(LatLon)> in_area_;
    std::unordered_map<std::string, Track> tracks_;
    std::unordered_map<std::string, PastEnd> recent_ends_;
    std::uint64_t next_id_ = 1;
    std::uint64_t snapshot_index_ = 0;
    TrackerMetrics metrics_;
};

}  // namespace sema
