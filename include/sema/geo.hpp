#pragma once

#include <filesystem>
#include <string>
#include <vector>

namespace sema {

struct LatLon {
    double lat;
    double lon;
};

struct BoundingBox {
    double lat_min;
    double lon_min;
    double lat_max;
    double lon_max;

    [[nodiscard]] double area_sq_deg() const { return (lat_max - lat_min) * (lon_max - lon_min); }
    [[nodiscard]] bool contains(LatLon p) const {
        return p.lat >= lat_min && p.lat <= lat_max && p.lon >= lon_min && p.lon <= lon_max;
    }
};

// OpenSky credit cost of one /states/all request for this box
// (<=25 sq deg: 1, <=100: 2, <=400: 3, larger: 4).
int opensky_credit_cost(const BoundingBox& box);

// Planar polygon with holes in lon/lat degrees. Good enough for a country-sized
// area far from the poles and the antimeridian.
class MultiPolygon {
public:
    using Ring = std::vector<LatLon>;
    struct Polygon {
        Ring outer;
        std::vector<Ring> holes;
    };

    explicit MultiPolygon(std::vector<Polygon> polygons);

    // Parses a GeoJSON Polygon/MultiPolygon geometry, Feature or FeatureCollection
    // (all polygonal geometries are merged).
    static MultiPolygon from_geojson(const std::string& geojson);
    static MultiPolygon load_geojson(const std::filesystem::path& path);

    [[nodiscard]] bool contains(LatLon p) const;
    [[nodiscard]] const BoundingBox& bounds() const { return bounds_; }
    [[nodiscard]] const std::vector<Polygon>& polygons() const { return polygons_; }
    // GeoJSON MultiPolygon geometry, for loading into PostGIS.
    [[nodiscard]] std::string to_geojson_geometry() const;

private:
    std::vector<Polygon> polygons_;
    std::vector<BoundingBox> polygon_bounds_;
    BoundingBox bounds_{};
};

// Smallest box (rounded outwards to 0.05 deg) containing the polygon.
BoundingBox query_box_for(const MultiPolygon& area);

// Great-circle distance in metres (haversine, mean Earth radius).
double haversine_m(LatLon a, LatLon b);

// Offset in metres in a local east/north tangent plane.
struct EnuOffset {
    double east;
    double north;
};

// Local tangent-plane projection around `ref` using the WGS84 radii of
// curvature at the reference latitude. Accurate to well under a metre within
// ~50 km of `ref`, which is all the tracker needs because it re-centres its
// frame on every update.
EnuOffset to_local(LatLon ref, LatLon p);
LatLon from_local(LatLon ref, EnuOffset d);

}  // namespace sema
