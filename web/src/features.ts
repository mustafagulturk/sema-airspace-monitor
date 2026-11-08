// Builds the per-tick GeoJSON for aircraft icons and trails (pure).

import { extrapolate } from "./motion";
import { displayName } from "./format";
import type { Aircraft } from "./protocol";

/** Points kept in the short tail drawn behind every aircraft. */
export const SHORT_TAIL_POINTS = 6;

export interface AircraftProps {
  id: number;
  icao24: string;
  name: string;
  track: number;
  ground: boolean;
  /** extrapolated altitude (m), -1 when unknown */
  alt: number;
  coasting: boolean;
}

export interface TrailProps {
  icao24: string;
  ground: boolean;
  alt: number;
  coasting: boolean;
}

export interface Frame {
  points: GeoJSON.FeatureCollection<GeoJSON.Point, AircraftProps>;
  trails: GeoJSON.FeatureCollection<GeoJSON.LineString, TrailProps>;
  selectedTrail: GeoJSON.FeatureCollection<GeoJSON.LineString, TrailProps>;
}

export function buildFrame(aircraft: readonly Aircraft[], now: number, selected: string | null): Frame {
  const points: GeoJSON.Feature<GeoJSON.Point, AircraftProps>[] = [];
  const trails: GeoJSON.Feature<GeoJSON.LineString, TrailProps>[] = [];
  const selectedTrail: GeoJSON.Feature<GeoJSON.LineString, TrailProps>[] = [];
  for (const a of aircraft) {
    const p = extrapolate(a, now);
    const alt = a.ground ? 0 : p.alt ?? -1;
    const coasting = a.state === "coasting";
    points.push({
      type: "Feature",
      id: a.id,
      properties: {
        id: a.id,
        icao24: a.icao24,
        name: displayName(a.callsign, a.icao24),
        track: Number.isFinite(a.track) ? a.track : 0,
        ground: a.ground,
        alt,
        coasting,
      },
      geometry: { type: "Point", coordinates: [p.lon, p.lat] },
    });
    const here: [number, number] = [p.lon, p.lat];
    const tprops: TrailProps = { icao24: a.icao24, ground: a.ground, alt, coasting };
    const trail = Array.isArray(a.trail) ? a.trail : [];
    const tail = [...trail.slice(-SHORT_TAIL_POINTS), here];
    if (tail.length >= 2) {
      trails.push({ type: "Feature", properties: tprops, geometry: { type: "LineString", coordinates: tail } });
    }
    if (a.icao24 === selected && trail.length >= 1) {
      selectedTrail.push({
        type: "Feature",
        properties: tprops,
        geometry: { type: "LineString", coordinates: [...trail, here] },
      });
    }
  }
  return {
    points: { type: "FeatureCollection", features: points },
    trails: { type: "FeatureCollection", features: trails },
    selectedTrail: { type: "FeatureCollection", features: selectedTrail },
  };
}

/** [[minLon, minLat], [maxLon, maxLat]] of any GeoJSON, or null if it has no coordinates. */
export function geojsonBounds(g: unknown): [[number, number], [number, number]] | null {
  let minX = Infinity, minY = Infinity, maxX = -Infinity, maxY = -Infinity;
  const walk = (v: unknown): void => {
    if (!Array.isArray(v)) return;
    if (v.length >= 2 && typeof v[0] === "number" && typeof v[1] === "number") {
      const x = v[0], y = v[1];
      if (x < minX) minX = x;
      if (x > maxX) maxX = x;
      if (y < minY) minY = y;
      if (y > maxY) maxY = y;
      return;
    }
    for (const c of v) walk(c);
  };
  const visit = (o: unknown): void => {
    if (typeof o !== "object" || o === null) return;
    const r = o as Record<string, unknown>;
    if ("coordinates" in r) walk(r.coordinates);
    if (Array.isArray(r.features)) r.features.forEach(visit);
    if ("geometry" in r) visit(r.geometry);
    if (Array.isArray(r.geometries)) r.geometries.forEach(visit);
  };
  visit(g);
  if (!Number.isFinite(minX)) return null;
  return [[minX, minY], [maxX, maxY]];
}
