#include "sema/snapshot_io.hpp"

#include <stdexcept>

#include <nlohmann/json.hpp>

namespace sema {
namespace {

using nlohmann::json;

template <typename T>
json opt(const std::optional<T>& v) {
    return v ? json(*v) : json(nullptr);
}

}  // namespace

std::string to_opensky_json(const StatesSnapshot& snapshot) {
    json rows = json::array();
    for (const auto& s : snapshot.states) {
        rows.push_back(json::array({s.icao24, s.callsign, s.origin_country, opt(s.time_position), s.last_contact,
                                    opt(s.longitude), opt(s.latitude), opt(s.baro_altitude), s.on_ground,
                                    opt(s.velocity), opt(s.true_track), opt(s.vertical_rate), nullptr,
                                    opt(s.geo_altitude), s.squawk.empty() ? json(nullptr) : json(s.squawk), s.spi,
                                    s.position_source}));
    }
    return json{{"time", snapshot.time}, {"states", std::move(rows)}}.dump();
}

SnapshotWriter::SnapshotWriter(const std::filesystem::path& path) : out_(path, std::ios::app) {
    if (!out_) throw std::runtime_error("cannot open " + path.string() + " for writing");
}

void SnapshotWriter::write(const StatesSnapshot& snapshot) {
    out_ << to_opensky_json(snapshot) << '\n' << std::flush;
    if (!out_) throw std::runtime_error("failed to write snapshot recording");
}

SnapshotReader::SnapshotReader(const std::filesystem::path& path) : in_(path) {
    if (!in_) throw std::runtime_error("cannot open " + path.string());
}

std::optional<StatesSnapshot> SnapshotReader::next() {
    std::string line;
    while (std::getline(in_, line)) {
        ++line_no_;
        if (line.find_first_not_of(" \t\r") == std::string::npos) continue;
        try {
            auto r = parse_states(line);
            skipped_rows_ += r.skipped_rows;
            return std::move(r.snapshot);
        } catch (const std::exception& e) {
            throw std::runtime_error("line " + std::to_string(line_no_) + ": " + e.what());
        }
    }
    return std::nullopt;
}

}  // namespace sema
