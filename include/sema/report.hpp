#pragma once

// Measurement report: turns one replay of a real recording into the Markdown
// tables of the README's "Ölçümler" section, and lets the user label anomaly
// events by hand (CSV) so the false-alarm rate is computed, not guessed.
//
//   sema_replay kayit.jsonl --events-csv olaylar.csv      # list events, label column empty
//   (fill the "etiket" column: gerçek / yanlış)
//   sema_replay kayit.jsonl --report rapor.md --labels olaylar.csv

#include <cstdint>
#include <iosfwd>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <nlohmann/json.hpp>

#include "sema/anomaly.hpp"
#include "sema/metrics.hpp"
#include "sema/state_vector.hpp"
#include "sema/tracker.hpp"

namespace sema {

// What the recording itself says: span, real update interval and aircraft
// per snapshot (after the Turkey and military filters, which ran before
// --record wrote it).
struct RecordingSummary {
    std::uint64_t snapshots = 0;
    std::int64_t first_time = 0;
    std::int64_t last_time = 0;
    Distribution interval_s;
    Distribution aircraft_per_snapshot;

    void add(const StatesSnapshot& snapshot);
    [[nodiscard]] double span_s() const { return static_cast<double>(last_time - first_time); }
};

enum class EventLabel { Unlabeled, Real, FalseAlarm };

// Accepts Turkish and English, any case, with or without Turkish letters:
// gerçek/gercek/real/doğru/evet/1 -> Real, yanlış/yanlis/false/fp/hayır/0 -> FalseAlarm,
// empty or "?" -> Unlabeled. Anything else throws std::invalid_argument.
EventLabel parse_event_label(std::string_view text);

// Labels by AnomalyEvent::key() (kind, address, start time: stable across replays).
using EventLabels = std::map<std::string, EventLabel>;

// One row per event, RFC 4180 quoting. Columns:
// key,baslangic_utc,tur,icao24,cagri_adi,sure_dk,enlem,boylam,irtifa_m,ozet,etiket
// `labels` (optional) pre-fills the last column, so a re-run keeps earlier work.
void write_events_csv(std::ostream& out, const std::vector<AnomalyEvent>& events, const EventLabels* labels = nullptr);

// Reads the "key" and "etiket" columns of a CSV written by write_events_csv
// (other columns and their order do not matter). Throws std::runtime_error
// on a missing column or a bad label, naming the line.
EventLabels read_event_labels_csv(std::istream& in);

struct ReportInput {
    std::string recording_name;
    std::string generated_at_utc;  // "YYYY-MM-DD HH:MM"
    const RecordingSummary* recording = nullptr;
    const TrackerMetrics* tracker = nullptr;
    const AnomalyMetrics* anomalies = nullptr;
    const std::vector<AnomalyEvent>* events = nullptr;
    const EventLabels* labels = nullptr;  // optional
    const nlohmann::json* stats = nullptr;  // compute_traffic_stats output, optional
    std::size_t example_events = 10;
};

// Turkish Markdown, same table shapes as the README.
std::string render_report_markdown(const ReportInput& in);

// Per-kind label counts, exposed for tests.
struct LabelCounts {
    std::uint64_t real = 0;
    std::uint64_t false_alarm = 0;
    std::uint64_t unlabeled = 0;
    [[nodiscard]] std::uint64_t labeled() const { return real + false_alarm; }
    // false_alarm / labeled; empty if nothing is labeled.
    [[nodiscard]] std::optional<double> false_alarm_rate() const;
};
LabelCounts count_labels(const std::vector<AnomalyEvent>& events, const EventLabels& labels,
                         std::optional<EventKind> kind = std::nullopt);

// "1.234", "13,8": Turkish number format used throughout the README.
std::string format_tr(double x, int decimals = 0);

}  // namespace sema
