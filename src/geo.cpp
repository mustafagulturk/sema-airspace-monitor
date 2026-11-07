#include "sema/geo.hpp"

#include <algorithm>
#include <cmath>
#include <fstream>
#include <limits>
#include <sstream>
#include <stdexcept>

#include <nlohmann/json.hpp>

namespace sema {
namespace {

using nlohmann::json;

BoundingBox ring_bounds(const MultiPolygon::Ring& ring) {
    BoundingBox b{std::numeric_limits<double>::infinity(), std::numeric_limits<double>::infinity(),
                  -std::numeric_limits<double>::infinity(), -std::numeric_limits<double>::infinity()};
    for (const auto& p : ring) {
        b.lat_min = std::min(b.lat_min, p.lat);
        b.lat_max = std::max(b.lat_max, p.lat);
        b.lon_min = std::min(b.lon_min, p.lon);
        b.lon_max = std::max(b.lon_max, p.lon);
    }
    return b;
}

// Even-odd ray casting along +lon. Works for open or closed rings.
bool ring_contains(const MultiPolygon::Ring& ring, LatLon p) {
    bool inside = false;
    const std::size_t n = ring.size();
    for (std::size_t i = 0, j = n - 1; i < n; j = i++) {
        const LatLon& a = ring[i];
        const LatLon& b = ring[j];
        if ((a.lat > p.lat) != (b.lat > p.lat)) {
            const double lon_at = a.lon + (p.lat - a.lat) * (b.lon - a.lon) / (b.lat - a.lat);
            if (p.lon < lon_at) inside = !inside;
        }
    }
    return inside;
}

MultiPolygon::Ring parse_ring(const json& coords) {
    if (!coords.is_array()) throw std::runtime_error("geojson: ring is not an array");
    MultiPolygon::Ring ring;
    ring.reserve(coords.size());
    for (const auto& pt : coords) {
        if (!pt.is_array() || pt.size() < 2 || !pt[0].is_number() || !pt[1].is_number()) {
            throw std::runtime_error("geojson: bad position");
        }
        ring.push_back(LatLon{pt[1].get<double>(), pt[0].get<double>()});
    }
    if (ring.size() < 4) throw std::runtime_error("geojson: ring has fewer than 4 positions");
    return ring;
}

MultiPolygon::Polygon parse_polygon(const json& coords) {
    if (!coords.is_array() || coords.empty()) throw std::runtime_error("geojson: empty polygon");
    MultiPolygon::Polygon poly;
    poly.outer = parse_ring(coords[0]);
    for (std::size_t i = 1; i < coords.size(); ++i) poly.holes.push_back(parse_ring(coords[i]));
    return poly;
}

void collect(const json& obj, std::vector<MultiPolygon::Polygon>& out) {
    if (!obj.is_object()) throw std::runtime_error("geojson: expected object");
    const std::string type = obj.value("type", "");
    if (type == "FeatureCollection") {
        for (const auto& f : obj.at("features")) collect(f, out);
    } else if (type == "Feature") {
        if (obj.contains("geometry") && !obj["geometry"].is_null()) collect(obj["geometry"], out);
    } else if (type == "Polygon") {
        out.push_back(parse_polygon(obj.at("coordinates")));
    } else if (type == "MultiPolygon") {
        for (const auto& p : obj.at("coordinates")) out.push_back(parse_polygon(p));
    } else if (type == "GeometryCollection") {
        for (const auto& g : obj.at("geometries")) collect(g, out);
    }
    // Other geometry types (points, lines) carry no area and are ignored.
}

json ring_to_json(const MultiPolygon::Ring& ring) {
    json arr = json::array();
    for (const auto& p : ring) arr.push_back({p.lon, p.lat});
    return arr;
}

}  // namespace

int opensky_credit_cost(const BoundingBox& box) {
    const double area = box.area_sq_deg();
    if (area <= 25.0) return 1;
    if (area <= 100.0) return 2;
    if (area <= 400.0) return 3;
    return 4;
}

MultiPolygon::MultiPolygon(std::vector<Polygon> polygons) : polygons_(std::move(polygons)) {
    if (polygons_.empty()) throw std::invalid_argument("MultiPolygon: no polygons");
    bool first = true;
    for (const auto& p : polygons_) {
        const BoundingBox b = ring_bounds(p.outer);
        polygon_bounds_.push_back(b);
        if (first) {
            bounds_ = b;
            first = false;
        } else {
            bounds_.lat_min = std::min(bounds_.lat_min, b.lat_min);
            bounds_.lat_max = std::max(bounds_.lat_max, b.lat_max);
            bounds_.lon_min = std::min(bounds_.lon_min, b.lon_min);
            bounds_.lon_max = std::max(bounds_.lon_max, b.lon_max);
        }
    }
}

MultiPolygon MultiPolygon::from_geojson(const std::string& geojson) {
    json doc;
    try {
        doc = json::parse(geojson);
    } catch (const json::parse_error& e) {
        throw std::runtime_error(std::string("geojson: invalid JSON: ") + e.what());
    }
    std::vector<Polygon> polys;
    collect(doc, polys);
    if (polys.empty()) throw std::runtime_error("geojson: no polygon geometry found");
    return MultiPolygon(std::move(polys));
}

MultiPolygon MultiPolygon::load_geojson(const std::filesystem::path& path) {
    std::ifstream in(path);
    if (!in) throw std::runtime_error("geojson: cannot open " + path.string());
    std::ostringstream ss;
    ss << in.rdbuf();
    return from_geojson(ss.str());
}

bool MultiPolygon::contains(LatLon p) const {
    if (!bounds_.contains(p)) return false;
    for (std::size_t i = 0; i < polygons_.size(); ++i) {
        if (!polygon_bounds_[i].contains(p)) continue;
        const Polygon& poly = polygons_[i];
        if (!ring_contains(poly.outer, p)) continue;
        bool in_hole = false;
        for (const auto& h : poly.holes) {
            if (ring_contains(h, p)) {
                in_hole = true;
                break;
            }
        }
        if (!in_hole) return true;
    }
    return false;
}

std::string MultiPolygon::to_geojson_geometry() const {
    json coords = json::array();
    for (const auto& p : polygons_) {
        json poly = json::array();
        poly.push_back(ring_to_json(p.outer));
        for (const auto& h : p.holes) poly.push_back(ring_to_json(h));
        coords.push_back(std::move(poly));
    }
    return json{{"type", "MultiPolygon"}, {"coordinates", std::move(coords)}}.dump();
}

BoundingBox query_box_for(const MultiPolygon& area) {
    constexpr double step = 0.05;
    const BoundingBox& b = area.bounds();
    // Small epsilon so values already on the grid are not pushed one step out.
    constexpr double eps = 1e-9;
    return BoundingBox{std::floor(b.lat_min / step + eps) * step, std::floor(b.lon_min / step + eps) * step,
                       std::ceil(b.lat_max / step - eps) * step, std::ceil(b.lon_max / step - eps) * step};
}

}  // namespace sema
