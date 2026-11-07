#!/usr/bin/env python3
"""Generate data/turkey_boundary.geojson from Natural Earth admin-0 (10m).

Approximation used in phase 1:
  Turkish land + a 12 NM buffer (territorial sea), minus the land of every
  other country. In the Aegean Turkey claims 6 NM, so the polygon slightly
  over-covers sea there; this is documented in README and is good enough for
  a "is this aircraft over/near Turkey" filter.

Usage: python3 tools/make_boundary.py ne_10m_admin_0_countries.geojson data/turkey_boundary.geojson
"""
import json
import sys

from pyproj import Transformer
from shapely.geometry import mapping, shape
from shapely.ops import transform, unary_union

NM = 1852.0
TERRITORIAL_SEA_M = 12 * NM
SIMPLIFY_DEG = 0.005  # ~500 m

def main(src: str, dst: str) -> None:
    with open(src, encoding="utf-8") as f:
        countries = json.load(f)["features"]

    turkey = None
    others = []
    for feat in countries:
        iso = feat["properties"].get("ADM0_A3")
        geom = shape(feat["geometry"])
        if iso == "TUR":
            turkey = geom
        elif geom.intersects(shape({"type": "Polygon", "coordinates": [[[22, 32], [48, 32], [48, 45], [22, 45], [22, 32]]]})):
            others.append(geom)
    if turkey is None:
        sys.exit("TUR not found")

    # Lambert azimuthal equal-area centred on Turkey -> buffer in metres.
    proj = "+proj=laea +lat_0=39 +lon_0=35 +datum=WGS84 +units=m"
    fwd = Transformer.from_crs("EPSG:4326", proj, always_xy=True).transform
    inv = Transformer.from_crs(proj, "EPSG:4326", always_xy=True).transform

    buffered = transform(inv, transform(fwd, turkey).buffer(TERRITORIAL_SEA_M, resolution=8))
    area = buffered.difference(unary_union(others)).simplify(SIMPLIFY_DEG, preserve_topology=True)
    # Drop tiny slivers left after subtracting neighbours.
    if area.geom_type == "MultiPolygon":
        area = unary_union([p for p in area.geoms if p.area > 1e-4])

    out = {
        "type": "FeatureCollection",
        "features": [{
            "type": "Feature",
            "properties": {
                "name": "Turkey land + 12 NM territorial sea (approx.)",
                "source": "Natural Earth 1:10m admin-0 countries (public domain)",
                "generator": "tools/make_boundary.py",
            },
            "geometry": mapping(area),
        }],
    }
    with open(dst, "w", encoding="utf-8") as f:
        json.dump(out, f, separators=(",", ":"))
    n = sum(len(p.exterior.coords) for p in getattr(area, "geoms", [area]))
    print(f"wrote {dst}: {area.geom_type}, {n} vertices, bounds={area.bounds}")

if __name__ == "__main__":
    main(sys.argv[1], sys.argv[2])
