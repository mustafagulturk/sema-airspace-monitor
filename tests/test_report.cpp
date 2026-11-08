#include <gtest/gtest.h>

#include <filesystem>
#include <sstream>
#include <string>
#include <vector>

#include "sema/anomaly.hpp"
#include "sema/flights.hpp"
#include "sema/geo.hpp"
#include "sema/report.hpp"
#include "sema/snapshot_io.hpp"
#include "sema/tracker.hpp"
#include "sema/traffic_stats.hpp"

using namespace sema;

namespace {

AnomalyEvent event(EventKind kind, std::string icao, double start, std::string summary = "özet") {
    AnomalyEvent e;
    e.kind = kind;
    e.icao24 = std::move(icao);
    e.callsign = "THY1";
    e.start_time = start;
    e.end_time = start + 120.0;
    e.pos = {41.0, 29.0};
    e.altitude_m = 3000.0;
    e.summary = std::move(summary);
    return e;
}

bool contains(const std::string& haystack, const std::string& needle) {
    return haystack.find(needle) != std::string::npos;
}

}  // namespace

TEST(Report, FormatsNumbersTheTurkishWay) {
    EXPECT_EQ(format_tr(0), "0");
    EXPECT_EQ(format_tr(142.4), "142");
    EXPECT_EQ(format_tr(1000), "1.000");
    EXPECT_EQ(format_tr(1234567), "1.234.567");
    EXPECT_EQ(format_tr(13.84, 1), "13,8");
    EXPECT_EQ(format_tr(2.4, 2), "2,40");
    EXPECT_EQ(format_tr(-0.01, 1), "0,0");
    EXPECT_EQ(format_tr(-1500.5, 1), "-1.500,5");
}

TEST(Report, ParsesLabelsInTurkishAndEnglish) {
    EXPECT_EQ(parse_event_label("gerçek"), EventLabel::Real);
    EXPECT_EQ(parse_event_label("GERÇEK"), EventLabel::Real);
    EXPECT_EQ(parse_event_label("Gercek"), EventLabel::Real);
    EXPECT_EQ(parse_event_label(" real "), EventLabel::Real);
    EXPECT_EQ(parse_event_label("1"), EventLabel::Real);
    EXPECT_EQ(parse_event_label("yanlış"), EventLabel::FalseAlarm);
    EXPECT_EQ(parse_event_label("YANLIŞ ALARM"), EventLabel::FalseAlarm);
    EXPECT_EQ(parse_event_label("fp"), EventLabel::FalseAlarm);
    EXPECT_EQ(parse_event_label("hayır"), EventLabel::FalseAlarm);
    EXPECT_EQ(parse_event_label(""), EventLabel::Unlabeled);
    EXPECT_EQ(parse_event_label("?"), EventLabel::Unlabeled);
    EXPECT_THROW((void)parse_event_label("belki"), std::invalid_argument);
}

TEST(Report, EventsCsvRoundTripsLabels) {
    std::vector<AnomalyEvent> events = {event(EventKind::Holding, "4ba9c1", 1'762'500'000.0, "bekleme, \"tur\", 3"),
                                        event(EventKind::RapidDescent, "4bb1f2", 1'762'500'600.0)};
    std::ostringstream csv;
    write_events_csv(csv, events);
    const std::string text = csv.str();
    EXPECT_TRUE(contains(text, "key,baslangic_utc,tur,"));
    EXPECT_TRUE(contains(text, "\"bekleme, \"\"tur\"\", 3\""));  // quoted, quotes doubled

    // Nothing labelled yet.
    std::istringstream empty(text);
    const auto none = read_event_labels_csv(empty);
    ASSERT_EQ(none.size(), 2u);
    EXPECT_EQ(none.at(events[0].key()), EventLabel::Unlabeled);

    // The user fills the last column of the first row.
    std::string labelled = text;
    const auto first_row_end = labelled.find('\n', labelled.find('\n') + 1);
    labelled.insert(first_row_end, "gerçek");
    std::istringstream in(labelled);
    const auto labels = read_event_labels_csv(in);
    EXPECT_EQ(labels.at(events[0].key()), EventLabel::Real);
    EXPECT_EQ(labels.at(events[1].key()), EventLabel::Unlabeled);

    // Re-writing with labels keeps them.
    std::ostringstream again;
    write_events_csv(again, events, &labels);
    std::istringstream in2(again.str());
    EXPECT_EQ(read_event_labels_csv(in2), labels);
}

TEST(Report, ReadsExcelSemicolonCsvWithBom) {
    const std::string text =
        "\xEF\xBB\xBFkey;tur;etiket\r\n"
        "holding:4ba9c1:1762500000;holding;Yanlış\r\n"
        "rapid_descent:4bb1f2:1762500600;rapid_descent;GERÇEK\r\n";
    std::istringstream in(text);
    const auto labels = read_event_labels_csv(in);
    ASSERT_EQ(labels.size(), 2u);
    EXPECT_EQ(labels.at("holding:4ba9c1:1762500000"), EventLabel::FalseAlarm);
    EXPECT_EQ(labels.at("rapid_descent:4bb1f2:1762500600"), EventLabel::Real);
}

TEST(Report, RejectsBadLabelsAndMissingColumns) {
    std::istringstream bad("key,etiket\nholding:a:1,belki\n");
    EXPECT_THROW((void)read_event_labels_csv(bad), std::runtime_error);
    std::istringstream no_label("key,tur\nholding:a:1,holding\n");
    EXPECT_THROW((void)read_event_labels_csv(no_label), std::runtime_error);
}

TEST(Report, CountsFalseAlarmRatePerKind) {
    std::vector<AnomalyEvent> events = {event(EventKind::Holding, "a", 1.0), event(EventKind::Holding, "b", 2.0),
                                        event(EventKind::Holding, "c", 3.0), event(EventKind::Emergency, "d", 4.0)};
    EventLabels labels = {{events[0].key(), EventLabel::Real},
                          {events[1].key(), EventLabel::FalseAlarm},
                          {events[3].key(), EventLabel::Real}};
    const auto h = count_labels(events, labels, EventKind::Holding);
    EXPECT_EQ(h.real, 1u);
    EXPECT_EQ(h.false_alarm, 1u);
    EXPECT_EQ(h.unlabeled, 1u);
    ASSERT_TRUE(h.false_alarm_rate());
    EXPECT_DOUBLE_EQ(*h.false_alarm_rate(), 0.5);
    EXPECT_FALSE(count_labels(events, {}, EventKind::Holding).false_alarm_rate());
    EXPECT_EQ(count_labels(events, labels).labeled(), 3u);
}

TEST(Report, RecordingSummaryMeasuresIntervalAndAircraft) {
    RecordingSummary r;
    StatesSnapshot s;
    s.time = 1000;
    s.states.resize(10);
    r.add(s);
    s.time = 1065;
    s.states.resize(12);
    r.add(s);
    s.time = 1135;
    r.add(s);
    EXPECT_EQ(r.snapshots, 3u);
    EXPECT_DOUBLE_EQ(r.span_s(), 135.0);
    EXPECT_EQ(r.interval_s.count(), 2u);
    EXPECT_DOUBLE_EQ(r.interval_s.percentile(50), 67.5);
    EXPECT_DOUBLE_EQ(r.aircraft_per_snapshot.max(), 12.0);
}

// End to end on the synthetic anomaly recording: every section is there and
// the label columns appear only with labels.
TEST(Report, RendersEverySectionFromARecording) {
    const std::filesystem::path data = SEMA_DATA_DIR;
    auto area = std::make_shared<MultiPolygon>(MultiPolygon::load_geojson(data / "turkey_boundary.geojson"));
    Tracker tracker(TrackerConfig{}, [area](LatLon p) { return area->contains(p); });
    AnomalyConfig acfg;
    acfg.signal_loss_min_coverage = 3;
    AnomalyDetector detector(acfg, area);
    const auto airports = AirportIndex::load(data / "airports_tr.json");
    FlightLog log(&airports);
    std::vector<FlightRecord> flights;
    std::vector<AnomalyEvent> events;
    RecordingSummary rec;
    SnapshotReader reader(std::filesystem::path(SEMA_FIXTURE_DIR) / "anomaly_synthetic.jsonl");
    while (auto snap = reader.next()) {
        rec.add(*snap);
        const auto r = tracker.process(*snap);
        const auto ev = detector.process(*snap, tracker, r);
        events.insert(events.end(), ev.closed.begin(), ev.closed.end());
        for (auto& f : log.update(*snap, r, tracker)) flights.push_back(std::move(f));
    }
    for (const auto& e : detector.active()) events.push_back(e);
    for (auto& f : log.flush()) flights.push_back(std::move(f));
    ASSERT_EQ(events.size(), 5u);
    StatsQuery q;
    q.from = 0;
    q.to = 4e9;
    const auto stats = compute_traffic_stats(flights, airports, q);

    ReportInput in;
    in.recording_name = "anomaly_synthetic.jsonl";
    in.recording = &rec;
    in.tracker = &tracker.metrics();
    in.anomalies = &detector.metrics();
    in.events = &events;
    in.stats = &stats;
    const auto md = render_report_markdown(in);
    for (const char* s : {"## Kayıt", "## İz takibi", "## Anomali tespiti", "## Trafik istatistikleri",
                          "Gerçek güncelleme aralığı", "Tahmin hatası, sabit hız KF", "Bekleme / daire çizme",
                          "**Toplam** | 5 |", "Örnek olaylar", "--events-csv"}) {
        EXPECT_TRUE(contains(md, s)) << s << "\n" << md;
    }
    EXPECT_FALSE(contains(md, "| Yanlış alarm oranı |"));
    // Meant for the public README: no aircraft addresses or callsigns.
    for (const auto& e : events) {
        EXPECT_FALSE(contains(md, e.icao24)) << e.icao24;
        if (!e.callsign.empty()) {
            EXPECT_FALSE(contains(md, e.callsign)) << e.callsign;
        }
    }

    EventLabels labels;
    for (const auto& e : events) labels[e.key()] = EventLabel::Real;
    labels[events[0].key()] = EventLabel::FalseAlarm;
    in.labels = &labels;
    const auto md2 = render_report_markdown(in);
    EXPECT_TRUE(contains(md2, "| Yanlış alarm oranı |")) << md2;
    EXPECT_TRUE(contains(md2, "%20 (1/5)")) << md2;
}
