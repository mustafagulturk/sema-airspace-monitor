// Coverage grid -> GeoJSON squares with a normalised log-scale value.

import type { CoverageMessage } from "./protocol";

export interface CoverageProps {
  count: number;
  /** 0..1, log(count)/log(max); -1 for gaps (count 0) */
  level: number;
}

export function coverageLevel(count: number, max: number): number {
  if (!(count > 0)) return -1;
  if (!(max > 1)) return 1;
  return Math.max(0, Math.min(1, Math.log(count) / Math.log(max)));
}

export function coverageToGeoJSON(
  msg: CoverageMessage,
): GeoJSON.FeatureCollection<GeoJSON.Polygon, CoverageProps> {
  const d = msg.cell_deg;
  const max = msg.max > 0 ? msg.max : Math.max(0, ...msg.cells.map((c) => c[2]));
  const features: GeoJSON.Feature<GeoJSON.Polygon, CoverageProps>[] = [];
  for (const cell of msg.cells) {
    const [lat, lon, count] = cell;
    if (!Number.isFinite(lat) || !Number.isFinite(lon)) continue;
    features.push({
      type: "Feature",
      properties: { count, level: coverageLevel(count, max) },
      geometry: {
        type: "Polygon",
        coordinates: [
          [
            [lon, lat],
            [lon + d, lat],
            [lon + d, lat + d],
            [lon, lat + d],
            [lon, lat],
          ],
        ],
      },
    });
  }
  return { type: "FeatureCollection", features };
}
