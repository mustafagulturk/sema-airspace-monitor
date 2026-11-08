#include "sema/report.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <ctime>
#include <istream>
#include <ostream>
#include <sstream>
#include <stdexcept>
#include <utility>

namespace sema {

namespace {

using nlohmann::json;

// Folds Turkish letters to ASCII and lower-cases, dropping spaces,
// so "Yanlış alarm" and "yanlis" compare equal.
std::string fold(std::string_view s) {
    static constexpr std::array<std::pair<std::string_view, char>, 12> kMap{{
        {"\xC3\xA7", 'c'}, {"\xC3\x87", 'c'}, {"\xC5\x9F", 's'}, {"\xC5\x9E", 's'},
        {"\xC4\xB1", 'i'}, {"\xC4\xB0", 'i'}, {"\xC4\x9F", 'g'}, {"\xC4\x9E", 'g'},
        {"\xC3\xB6", 'o'}, {"\xC3\x96", 'o'}, {"\xC3\xBC", 'u'}, {"\xC3\x9C", 'u'},
    }};
    std::string out;
    for (std::size_t i = 0; i < s.size();) {
        bool mapped = false;
        for (const auto& [from, to] : kMap) {
            if (s.substr(i, from.size()) == from) {
                out.push_back(to);
                i += from.size();
                mapped = true;
                break;
            }
        }
        if (mapped) continue;
        const char c = s[i++];
        if (c == ' ' || c == '\t' || c == '\r' || c == '\n') continue;
        out.push_back(c >= 'A' && c <= 'Z' ? static_cast<char>(c - 'A' + 'a') : c);
    }
    return out;
}

std::string csv_field(std::string_view s) {
    if (s.find_first_of(",;\"\n\r") == std::string_view::npos) return std::string(s);
    std::string out = "\"";
    for (const char c : s) {
        if (c == '"') out.push_back('"');
        out.push_back(c);
    }
    out.push_back('"');
    return out;
}

// Splits a whole CSV document into rows (RFC 4180 quotes; newlines inside
// quotes are kept).
std::vector<std::vector<std::string>> parse_csv(const std::string& text, char sep) {
    std::vector<std::vector<std::string>> rows;
    std::vector<std::string> row;
    std::string field;
    bool quoted = false;
    bool any = false;
    for (std::size_t i = 0; i < text.size(); ++i) {
        const char c = text[i];
        if (quoted) {
            if (c == '"') {
                if (i + 1 < text.size() && text[i + 1] == '"') {
                    field.push_back('"');
                    ++i;
                } else {
                    quoted = false;
                }
            } else {
                field.push_back(c);
            }
            continue;
        }
        if (c == '"') {
            quoted = true;
            any = true;
        } else if (c == sep) {
            row.push_back(std::move(field));
            field.clear();
            any = true;
        } else if (c == '\n' || c == '\r') {
            if (c == '\r' && i + 1 < text.size() && text[i + 1] == '\n') ++i;
            if (any || !field.empty()) {
                row.push_back(std::move(field));
                rows.push_back(std::move(row));
            }
            row.clear();
            field.clear();
            any = false;
        } else {
            field.push_back(c);
            any = true;
        }
    }
    if (any || !field.empty()) {
        row.push_back(std::move(field));
        rows.push_back(std::move(row));
    }
    return rows;
}

std::string utc_string(double t, const char* format) {
    const auto tt = static_cast<std::time_t>(t);
    std::tm tm{};
    gmtime_r(&tt, &tm);
    char buf[40];
    (void)std::strftime(buf, sizeof buf, format, &tm);
    return buf;
}

std::string fixed(double x, int decimals) {
    char buf[64];
    (void)std::snprintf(buf, sizeof buf, "%.*f", decimals, x);
    return buf;
}

std::string metres(double m) {
    if (m >= 10000.0) return format_tr(m / 1000.0, 1) + " km";
    return format_tr(m, 0) + " m";
}

std::string percent(double fraction, int decimals = 1) { return "%" + format_tr(fraction * 100.0, decimals); }

std::string duration(double s) {
    if (s < 3600.0) return format_tr(s / 60.0, 0) + " dk";
    if (s < 2.0 * 86400.0) return format_tr(s / 3600.0, 1) + " saat";
    return format_tr(s / 86400.0, 1) + " gün";
}

std::string dist_row(const Distribution& d) {
    if (d.count() == 0) return "veri yok";
    return "medyan " + metres(d.percentile(50)) + ", p90 " + metres(d.percentile(90)) + ", p95 " +
           metres(d.percentile(95)) + " (n=" + format_tr(static_cast<double>(d.count())) + ")";
}

const char* kind_tr(EventKind k) {
    switch (k) {
        case EventKind::Emergency: return "Acil durum kodu";
        case EventKind::RapidDescent: return "Ani irtifa kaybı";
        case EventKind::Holding: return "Bekleme / daire çizme";
        case EventKind::RouteDeviation: return "Rota sapması (geri dönüş)";
        case EventKind::SignalLoss: return "Uzun sinyal kaybı";
    }
    return "?";
}

const char* label_tr(EventLabel l) {
    switch (l) {
        case EventLabel::Real: return "gerçek";
        case EventLabel::FalseAlarm: return "yanlış alarm";
        case EventLabel::Unlabeled: return "etiketsiz";
    }
    return "?";
}

EventLabel label_of(const AnomalyEvent& e, const EventLabels* labels) {
    if (labels == nullptr) return EventLabel::Unlabeled;
    const auto it = labels->find(e.key());
    return it == labels->end() ? EventLabel::Unlabeled : it->second;
}

std::string md_cell(std::string s) {
    for (auto& c : s) {
        if (c == '|' || c == '\n') c = ' ';
    }
    return s;
}

void recording_section(std::ostringstream& md, const ReportInput& in) {
    const auto& r = *in.recording;
    md << "## Kayıt\n\n| Ölçüt | Değer |\n|---|---|\n";
    md << "| Zaman aralığı (UTC) | " << utc_string(static_cast<double>(r.first_time), "%Y-%m-%d %H:%M") << " – "
       << utc_string(static_cast<double>(r.last_time), "%Y-%m-%d %H:%M") << " (" << duration(r.span_s()) << ") |\n";
    md << "| Anlık görüntü (istek) | " << format_tr(static_cast<double>(r.snapshots)) << " |\n";
    if (r.interval_s.count() > 0) {
        md << "| Gerçek güncelleme aralığı | medyan " << format_tr(r.interval_s.percentile(50), 1) << " s, p90 "
           << format_tr(r.interval_s.percentile(90), 1) << " s, en uzun " << duration(r.interval_s.max()) << " |\n";
    }
    if (r.aircraft_per_snapshot.count() > 0) {
        md << "| İstek başına uçak (filtre sonrası) | ortalama " << format_tr(r.aircraft_per_snapshot.mean(), 1)
           << ", medyan " << format_tr(r.aircraft_per_snapshot.percentile(50)) << ", en çok "
           << format_tr(r.aircraft_per_snapshot.max()) << " |\n";
    }
    md << "\nGünlük kredi kullanımı ve atılan askeri/devlet uçağı sayısı kayıtta yoktur (filtre kayıttan önce "
          "çalışır); veritabanında `SELECT * FROM daily_metrics;` ile alınır.\n\n";
}

void tracking_section(std::ostringstream& md, const TrackerMetrics& m) {
    md << "## İz takibi\n\n| Ölçüt | Değer |\n|---|---|\n";
    md << "| Tahmin hatası, sabit hız KF | " << dist_row(m.cv_error_m) << " |\n";
    md << "| Tahmin hatası, koordineli dönüş EKF | " << dist_row(m.ct_error_m) << " |\n";
    md << "| Tahmin hatası, ölü hesap (taban çizgisi) | " << dist_row(m.dead_reckoning_error_m) << " |\n";
    md << "| Tahmin hatası, son konumda bekle (taban çizgisi) | " << dist_row(m.hold_error_m) << " |\n";
    if (m.altitude_error_m.count() > 0) {
        md << "| İrtifa tahmin hatası | medyan " << metres(m.altitude_error_m.percentile(50)) << ", p90 "
           << metres(m.altitude_error_m.percentile(90)) << " |\n";
    }
    if (m.tracked_per_snapshot.count() > 0) {
        md << "| İzlenen uçak (anlık görüntü başına) | medyan " << format_tr(m.tracked_per_snapshot.percentile(50))
           << ", en çok " << format_tr(m.tracked_per_snapshot.max()) << " |\n";
    }
    md << "| İz kopma oranı | " << percent(m.break_rate()) << " (" << format_tr(static_cast<double>(m.tracks_created))
       << " izde " << format_tr(static_cast<double>(m.track_breaks)) << " kopma; "
       << format_tr(static_cast<double>(m.signal_losses())) << " sinyal kaybının "
       << format_tr(static_cast<double>(m.reacquisitions)) << "'i köprülendi) |\n";
    const auto rejects = m.invalid_position + m.impossible_speed + m.teleport + m.bad_altitude + m.bad_vertical_rate;
    md << "| Kalite kontrolünde atılan rapor | " << format_tr(static_cast<double>(rejects)) << " (imkânsız hız "
       << m.impossible_speed << ", ışınlanma " << m.teleport << ", bozuk irtifa " << m.bad_altitude
       << ", bozuk dikey hız " << m.bad_vertical_rate << ", geçersiz konum " << m.invalid_position << ") |\n";
    if (m.processing_us_per_snapshot.count() > 0) {
        md << "| İşlem süresi | anlık görüntü başına medyan " << format_tr(m.processing_us_per_snapshot.percentile(50))
           << " µs, rapor başına medyan " << format_tr(m.processing_us_per_measurement.percentile(50), 1) << " µs |\n";
    }

    bool any_horizon = false;
    for (std::size_t i = 0; i < kHorizonBuckets; ++i) any_horizon |= m.cv_error_by_horizon_m[i].count() > 0;
    if (any_horizon) {
        md << "\nUfka göre medyan tahmin hatası:\n\n| Ufuk | n | Sabit hız KF | Dönüş EKF | Ölü hesap |\n|---|---|---|---|---|\n";
        for (std::size_t i = 0; i < kHorizonBuckets; ++i) {
            if (m.cv_error_by_horizon_m[i].count() == 0) continue;
            md << "| " << horizon_label(i) << " | " << format_tr(static_cast<double>(m.cv_error_by_horizon_m[i].count()))
               << " | " << metres(m.cv_error_by_horizon_m[i].percentile(50)) << " | "
               << metres(m.ct_error_by_horizon_m[i].percentile(50)) << " | "
               << metres(m.dead_reckoning_error_by_horizon_m[i].percentile(50)) << " |\n";
        }
    }
    md << '\n';
}

void anomaly_section(std::ostringstream& md, const ReportInput& in) {
    const auto& m = *in.anomalies;
    static const std::vector<AnomalyEvent> kNone;
    const auto& events = in.events != nullptr ? *in.events : kNone;
    const bool labelled = in.labels != nullptr;
    md << "## Anomali tespiti\n\n";
    md << format_tr(static_cast<double>(m.events())) << " olay, " << format_tr(m.airborne_aircraft_hours, 1)
       << " havadaki uçak-saatte.\n\n";
    md << "| Tür | Olay | / 1.000 uçak-saat |";
    if (labelled) md << " Gerçek | Yanlış alarm | Etiketsiz | Yanlış alarm oranı |";
    md << "\n|---|---|---|";
    if (labelled) md << "---|---|---|---|";
    md << '\n';
    auto rate = [&](std::uint64_t n) {
        return m.airborne_aircraft_hours > 0.0
                   ? format_tr(static_cast<double>(n) * 1000.0 / m.airborne_aircraft_hours, 2)
                   : std::string("–");
    };
    auto label_cells = [&](const LabelCounts& c) {
        const auto fa = c.false_alarm_rate();
        md << ' ' << c.real << " | " << c.false_alarm << " | " << c.unlabeled << " | "
           << (fa ? percent(*fa, 0) + " (" + std::to_string(c.false_alarm) + "/" + std::to_string(c.labeled()) + ")"
                  : std::string("–"))
           << " |";
    };
    for (std::size_t i = 0; i < kEventKinds; ++i) {
        const auto k = static_cast<EventKind>(i);
        md << "| " << kind_tr(k) << " | " << m.events_by_kind[i] << " | " << rate(m.events_by_kind[i]) << " |";
        if (labelled) label_cells(count_labels(events, *in.labels, k));
        md << '\n';
    }
    md << "| **Toplam** | " << m.events() << " | " << rate(m.events()) << " |";
    if (labelled) label_cells(count_labels(events, *in.labels));
    md << "\n\n";
    if (!labelled && !events.empty()) {
        md << "Yanlış alarm oranı için olayları `--events-csv olaylar.csv` ile dökün, `etiket` sütununu "
              "doldurun (gerçek / yanlış) ve raporu `--labels olaylar.csv` ile yeniden üretin.\n\n";
    }
    if (m.processing_us_per_snapshot.count() > 0) {
        md << "Dedektörün anlık görüntü başına işlem süresi: medyan "
           << format_tr(m.processing_us_per_snapshot.percentile(50)) << " µs.\n\n";
    }
    if (events.empty() || in.example_events == 0) return;

    // Examples: real ones first, then unlabeled, then false alarms; oldest first within each.
    std::vector<const AnomalyEvent*> order;
    order.reserve(events.size());
    for (const auto& e : events) order.push_back(&e);
    auto rank = [&](const AnomalyEvent* e) {
        switch (label_of(*e, in.labels)) {
            case EventLabel::Real: return 0;
            case EventLabel::Unlabeled: return 1;
            case EventLabel::FalseAlarm: return 2;
        }
        return 3;
    };
    std::stable_sort(order.begin(), order.end(), [&](const auto* a, const auto* b) {
        if (rank(a) != rank(b)) return rank(a) < rank(b);
        return a->start_time < b->start_time;
    });
    if (order.size() > in.example_events) order.resize(in.example_events);
    // The report is meant to be pasted into the public README, so no aircraft
    // address or callsign appears in it (events of private aircraft could
    // identify a person); the local CSV keeps them for labelling.
    md << "Örnek olaylar (uçak kimliği bilerek yazılmadı):\n\n| Başlangıç (UTC) | Tür | Süre | Özet |";
    if (labelled) md << " Etiket |";
    md << "\n|---|---|---|---|";
    if (labelled) md << "---|";
    md << '\n';
    for (const auto* e : order) {
        md << "| " << utc_string(e->start_time, "%Y-%m-%d %H:%M") << " | " << kind_tr(e->kind) << " | "
           << format_tr((e->end_time - e->start_time) / 60.0, 1) << " dk | " << md_cell(e->summary) << " |";
        if (labelled) md << ' ' << label_tr(label_of(*e, in.labels)) << " |";
        md << '\n';
    }
    md << '\n';
}

std::string airport_name(const json& a) {
    std::string name = a.value("city", std::string());
    if (name.empty()) name = a.value("name", std::string());
    const std::string code = a.value("iata", std::string()).empty() ? a.value("icao", std::string())
                                                                    : a.value("iata", std::string());
    return name.empty() ? code : md_cell(name) + " (" + code + ")";
}

void stats_section(std::ostringstream& md, const json& s) {
    md << "## Trafik istatistikleri\n\n| Ölçüt | Değer |\n|---|---|\n";
    const auto& t = s.at("totals");
    const auto flights = t.value("flights", 0.0);
    const auto days = std::max(1.0, t.value("days", 1.0));
    md << "| Uçuş / farklı uçak | " << format_tr(flights) << " / " << format_tr(t.value("aircraft", 0.0)) << " |\n";
    md << "| Kalkışı görülen / inişi görülen | " << format_tr(t.value("departures", 0.0)) << " / "
       << format_tr(t.value("arrivals", 0.0)) << " |\n";
    md << "| Günlük ortalama uçuş | " << format_tr(flights / days, 0) << " (" << format_tr(days) << " gün) |\n";
    if (s.contains("busiest_day") && !s["busiest_day"].is_null()) {
        std::string day = s["busiest_day"].get<std::string>();
        double n = 0;
        for (const auto& d : s.value("daily", json::array())) {
            if (d.value("date", std::string()) == day) n = d.value("flights", 0.0);
        }
        md << "| En yoğun gün | " << day << " (" << format_tr(n) << " uçuş) |\n";
    }
    if (s.contains("peak_hour") && !s["peak_hour"].is_null()) {
        const int h = s["peak_hour"].get<int>();
        // Top three hours by total.
        std::vector<std::pair<double, int>> hours;
        for (const auto& x : s.value("hours", json::array())) hours.emplace_back(x.value("total", 0.0), x.value("hour", 0));
        std::stable_sort(hours.begin(), hours.end(), [](const auto& a, const auto& b) { return a.first > b.first; });
        std::string top;
        for (std::size_t i = 0; i < std::min<std::size_t>(3, hours.size()); ++i) {
            if (hours[i].first <= 0) break;
            char buf[16];
            (void)std::snprintf(buf, sizeof buf, "%02d:00", hours[i].second);
            top += (top.empty() ? "" : ", ") + std::string(buf);
        }
        char peak[16];
        (void)std::snprintf(peak, sizeof peak, "%02d:00–%02d:00", h, (h + 1) % 24);
        md << "| En yoğun saat (TSİ) | " << peak << " (ilk üç: " << top << ") |\n";
    }
    const auto airports = s.value("airports", json::array());
    if (!airports.empty()) {
        std::string list;
        for (std::size_t i = 0; i < std::min<std::size_t>(5, airports.size()); ++i) {
            const auto& a = airports[i];
            list += (list.empty() ? "" : "; ") + airport_name(a) + " " + format_tr(a.value("total", 0.0)) + " (" +
                    format_tr(a.value("departures", 0.0)) + " kalkış, " + format_tr(a.value("arrivals", 0.0)) + " iniş)";
        }
        md << "| En çok hareket (ilk 5 havalimanı) | " << list << " |\n";
    }
    const auto routes = s.value("routes", json::array());
    if (!routes.empty()) {
        std::string list;
        for (std::size_t i = 0; i < std::min<std::size_t>(5, routes.size()); ++i) {
            const auto& r = routes[i];
            list += (list.empty() ? "" : "; ") + md_cell(r.value("from", std::string("?"))) + "→" +
                    md_cell(r.value("to", std::string("?"))) + " " + format_tr(r.value("flights", 0.0));
        }
        md << "| En yoğun hatlar (ilk 5) | " << list << " |\n";
    }
    md << '\n';
}

}  // namespace

void RecordingSummary::add(const StatesSnapshot& s) {
    if (snapshots == 0) {
        first_time = s.time;
    } else if (s.time > last_time) {
        interval_s.add(static_cast<double>(s.time - last_time));
    }
    last_time = std::max(last_time, s.time);
    ++snapshots;
    aircraft_per_snapshot.add(static_cast<double>(s.states.size()));
}

EventLabel parse_event_label(std::string_view text) {
    const auto t = fold(text);
    if (t.empty() || t == "?" || t == "-") return EventLabel::Unlabeled;
    for (const char* r : {"gercek", "real", "dogru", "evet", "1", "tp", "true", "g", "d", "e"}) {
        if (t == r) return EventLabel::Real;
    }
    for (const char* f : {"yanlis", "yanlisalarm", "false", "fp", "hayir", "0", "y", "h", "falsealarm"}) {
        if (t == f) return EventLabel::FalseAlarm;
    }
    throw std::invalid_argument("unknown label '" + std::string(text) + "' (use gerçek / yanlış)");
}

void write_events_csv(std::ostream& out, const std::vector<AnomalyEvent>& events, const EventLabels* labels) {
    out << "key,baslangic_utc,tur,icao24,cagri_adi,sure_dk,enlem,boylam,irtifa_m,ozet,etiket\n";
    for (const auto& e : events) {
        const auto l = label_of(e, labels);
        out << csv_field(e.key()) << ',' << utc_string(e.start_time, "%Y-%m-%d %H:%M:%S") << ',' << to_string(e.kind)
            << ',' << e.icao24 << ',' << csv_field(e.callsign) << ',' << fixed((e.end_time - e.start_time) / 60.0, 1)
            << ',' << fixed(e.pos.lat, 4) << ',' << fixed(e.pos.lon, 4) << ','
            << (e.altitude_m ? fixed(*e.altitude_m, 0) : std::string()) << ',' << csv_field(e.summary) << ','
            << (l == EventLabel::Real ? "gerçek" : l == EventLabel::FalseAlarm ? "yanlış" : "") << '\n';
    }
}

EventLabels read_event_labels_csv(std::istream& in) {
    std::string text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    if (text.rfind("\xEF\xBB\xBF", 0) == 0) text.erase(0, 3);  // Excel's BOM
    // Excel with a Turkish locale saves with ';'; detect from the header line.
    const auto header_end = text.find_first_of("\r\n");
    const std::string_view header = std::string_view(text).substr(0, header_end);
    char sep = ',';
    if (header.find(',') == std::string_view::npos) {
        if (header.find(';') != std::string_view::npos) sep = ';';
        else if (header.find('\t') != std::string_view::npos) sep = '\t';
    }
    const auto rows = parse_csv(text, sep);
    if (rows.empty()) throw std::runtime_error("labels CSV is empty");
    std::optional<std::size_t> key_col;
    std::optional<std::size_t> label_col;
    for (std::size_t i = 0; i < rows[0].size(); ++i) {
        const auto h = fold(rows[0][i]);
        if (h == "key") key_col = i;
        if (h == "etiket" || h == "label") label_col = i;
    }
    if (!key_col || !label_col) throw std::runtime_error("labels CSV needs 'key' and 'etiket' columns");
    EventLabels labels;
    for (std::size_t r = 1; r < rows.size(); ++r) {
        const auto& row = rows[r];
        if (row.size() <= *key_col || row[*key_col].empty()) continue;
        const std::string label = row.size() > *label_col ? row[*label_col] : std::string();
        try {
            labels[row[*key_col]] = parse_event_label(label);
        } catch (const std::invalid_argument& e) {
            throw std::runtime_error("labels CSV row " + std::to_string(r + 1) + ": " + e.what());
        }
    }
    return labels;
}

std::optional<double> LabelCounts::false_alarm_rate() const {
    if (labeled() == 0) return std::nullopt;
    return static_cast<double>(false_alarm) / static_cast<double>(labeled());
}

LabelCounts count_labels(const std::vector<AnomalyEvent>& events, const EventLabels& labels,
                         std::optional<EventKind> kind) {
    LabelCounts c;
    for (const auto& e : events) {
        if (kind && e.kind != *kind) continue;
        switch (label_of(e, &labels)) {
            case EventLabel::Real: ++c.real; break;
            case EventLabel::FalseAlarm: ++c.false_alarm; break;
            case EventLabel::Unlabeled: ++c.unlabeled; break;
        }
    }
    return c;
}

std::string format_tr(double x, int decimals) {
    if (!std::isfinite(x)) return "–";
    std::string s = fixed(x, decimals);
    const bool neg = !s.empty() && s[0] == '-';
    if (neg) s.erase(0, 1);
    const auto dot = s.find('.');
    std::string int_part = s.substr(0, dot);
    std::string frac = dot == std::string::npos ? std::string() : s.substr(dot + 1);
    std::string grouped;
    // Turkish style groups thousands from 1.000 up (as in "1.000 uçak-saat").
    for (std::size_t i = 0; i < int_part.size(); ++i) {
        if (i > 0 && (int_part.size() - i) % 3 == 0) grouped.push_back('.');
        grouped.push_back(int_part[i]);
    }
    std::string out = grouped;
    if (!frac.empty()) out += "," + frac;
    const bool zero = std::all_of(out.begin(), out.end(), [](char c) { return c == '0' || c == ',' || c == '.'; });
    return (neg && !zero ? "-" : "") + out;
}

std::string render_report_markdown(const ReportInput& in) {
    std::ostringstream md;
    md << "# Semâ ölçüm raporu\n\n";
    md << "Kayıt: `" << in.recording_name << "`";
    if (!in.generated_at_utc.empty()) md << " · üretildi: " << in.generated_at_utc << " UTC";
    md << "\n\nBu dosya `sema_replay --report` ile üretildi. Tablolar README'nin \"Ölçümler\" bölümüyle aynı "
          "biçimdedir; satırlar oraya kopyalanabilir.\n\n";
    if (in.recording != nullptr) recording_section(md, in);
    if (in.tracker != nullptr) tracking_section(md, *in.tracker);
    if (in.anomalies != nullptr) anomaly_section(md, in);
    if (in.stats != nullptr) stats_section(md, *in.stats);
    return md.str();
}

}  // namespace sema
