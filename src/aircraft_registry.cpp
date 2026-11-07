#include "sema/aircraft_registry.hpp"

#include <algorithm>
#include <cctype>
#include <fstream>
#include <stdexcept>
#include <vector>

namespace sema {
namespace {

std::string to_lower_ascii(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return s;
}

std::string trim(std::string_view s) {
    const auto first = s.find_first_not_of(" \t\r\n");
    if (first == std::string_view::npos) return {};
    const auto last = s.find_last_not_of(" \t\r\n");
    return std::string(s.substr(first, last - first + 1));
}

// Splits one CSV line. A field that starts with ' or " is quoted with that
// character; a doubled quote character inside it is a literal quote.
std::vector<std::string> split_csv_line(std::string_view line) {
    std::vector<std::string> fields;
    std::size_t i = 0;
    const std::size_t n = line.size();
    while (true) {
        std::string field;
        if (i < n && (line[i] == '\'' || line[i] == '"')) {
            const char q = line[i++];
            while (i < n) {
                if (line[i] == q) {
                    if (i + 1 < n && line[i + 1] == q) {
                        field.push_back(q);
                        i += 2;
                    } else {
                        ++i;
                        break;
                    }
                } else {
                    field.push_back(line[i++]);
                }
            }
            // Skip anything between the closing quote and the separator.
            while (i < n && line[i] != ',') ++i;
        } else {
            while (i < n && line[i] != ',') field.push_back(line[i++]);
        }
        fields.push_back(trim(field));
        if (i >= n) break;
        ++i;  // skip ','
    }
    return fields;
}

}  // namespace

AircraftRegistry AircraftRegistry::from_csv(std::istream& in) {
    AircraftRegistry reg;
    std::string line;
    if (!std::getline(in, line)) return reg;
    if (line.size() >= 3 && static_cast<unsigned char>(line[0]) == 0xEF) line.erase(0, 3);  // UTF-8 BOM

    const auto header = split_csv_line(line);
    auto col = [&](std::string_view name) -> std::optional<std::size_t> {
        for (std::size_t i = 0; i < header.size(); ++i) {
            if (to_lower_ascii(header[i]) == name) return i;
        }
        return std::nullopt;
    };
    const auto c_icao = col("icao24");
    if (!c_icao) throw std::runtime_error("aircraft database CSV has no icao24 column");
    const auto c_reg = col("registration");
    const auto c_op = col("operator");
    const auto c_op_icao = col("operatoricao");
    const auto c_op_cs = col("operatorcallsign");
    const auto c_owner = col("owner");
    const auto c_model = col("model");

    while (std::getline(in, line)) {
        if (line.empty() || line == "\r") continue;
        const auto f = split_csv_line(line);
        auto get = [&](const std::optional<std::size_t>& c) -> std::string {
            return c && *c < f.size() ? f[*c] : std::string{};
        };
        AircraftInfo info;
        info.icao24 = to_lower_ascii(get(c_icao));
        if (info.icao24.empty()) continue;
        info.registration = get(c_reg);
        info.operator_name = get(c_op);
        info.operator_icao = get(c_op_icao);
        info.operator_callsign = get(c_op_cs);
        info.owner = get(c_owner);
        info.model = get(c_model);
        reg.add(std::move(info));
    }
    return reg;
}

AircraftRegistry AircraftRegistry::load_csv(const std::filesystem::path& path) {
    std::ifstream in(path);
    if (!in) throw std::runtime_error("cannot open aircraft database: " + path.string());
    return from_csv(in);
}

void AircraftRegistry::add(AircraftInfo info) {
    info.icao24 = to_lower_ascii(std::move(info.icao24));
    auto key = info.icao24;
    by_icao_.insert_or_assign(std::move(key), std::move(info));
}

const AircraftInfo* AircraftRegistry::find(std::string_view icao24) const {
    const auto it = by_icao_.find(to_lower_ascii(std::string(icao24)));
    return it == by_icao_.end() ? nullptr : &it->second;
}

}  // namespace sema
