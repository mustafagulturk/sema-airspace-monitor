// MapLibre setup: base style (with offline fallback), overlay sources and layers.

import * as maplibregl from "maplibre-gl";
import {
  type ExpressionSpecification,
  type GeoJSONSource,
  type StyleSpecification,
} from "maplibre-gl";
import { createAircraftSdf, ICON_PIXEL_RATIO } from "./icon";
import type { Frame } from "./features";
import { KIND_COLOR, type EventGeometry } from "./events";

export const DEFAULT_STYLE_URL = "https://tiles.openfreemap.org/styles/positron";
export const TURKEY_CENTER: [number, number] = [35, 39];

/** Altitude colour stops (metres → colour), shared with the legend. */
export const ALT_STOPS: [number, string][] = [
  [0, "#f59f00"],
  [1500, "#f76707"],
  [4000, "#e03131"],
  [7000, "#c2255c"],
  [10000, "#862e9c"],
  [12500, "#3b5bdb"],
];
export const GROUND_COLOR = "#868e96";
export const UNKNOWN_ALT_COLOR = "#495057";
export const GAP_COLOR = "#c92a2a";
/** Sequential coverage ramp, level 0..1 (log scale). */
export const COVERAGE_STOPS: [number, string][] = [
  [0, "#d3f9d8"],
  [0.5, "#51cf66"],
  [1, "#2b8a3e"],
];

const altColor: ExpressionSpecification = [
  "case",
  ["get", "ground"],
  GROUND_COLOR,
  ["<", ["get", "alt"], 0],
  UNKNOWN_ALT_COLOR,
  ["interpolate", ["linear"], ["get", "alt"], ...ALT_STOPS.flat()] as ExpressionSpecification,
];

const EMPTY: GeoJSON.FeatureCollection = { type: "FeatureCollection", features: [] };

/** Minimal local style used when the tile style cannot be fetched (offline, blocked). */
export const FALLBACK_STYLE: StyleSpecification = {
  version: 8,
  sources: {},
  layers: [{ id: "background", type: "background", paint: { "background-color": "#e9ecef" } }],
};

export async function loadBaseStyle(url: string, timeoutMs = 8000): Promise<StyleSpecification | null> {
  const ctl = new AbortController();
  const timer = setTimeout(() => ctl.abort(), timeoutMs);
  try {
    const res = await fetch(url, { signal: ctl.signal });
    if (!res.ok) return null;
    const style = (await res.json()) as StyleSpecification;
    return style && style.version === 8 ? style : null;
  } catch {
    return null;
  } finally {
    clearTimeout(timer);
  }
}

/** A text-font already used by the base style, so its glyph server has it. */
function pickFont(style: StyleSpecification): string[] {
  for (const l of style.layers) {
    const f = (l as { layout?: Record<string, unknown> }).layout?.["text-font"];
    if (Array.isArray(f) && f.every((x) => typeof x === "string") && f.length > 0) {
      return [f[0] as string];
    }
  }
  return ["Noto Sans Regular"];
}

export interface Overlays {
  setFrame(frame: Frame): void;
  setCoverage(data: GeoJSON.FeatureCollection): void;
  setCoverageVisible(on: boolean): void;
  setBoundary(data: GeoJSON.GeoJSON): void;
  setSelected(icao24: string | null): void;
  /** Shows one anomaly event's track and markers, or clears them. */
  setEvent(geometry: EventGeometry | null): void;
}

/** Adds all Semâ sources and layers. Call after the style has loaded. */
export function addOverlays(map: maplibregl.Map, style: StyleSpecification): Overlays {
  const sdf = createAircraftSdf();
  if (sdf) map.addImage("aircraft", sdf, { sdf: true, pixelRatio: ICON_PIXEL_RATIO });

  // Put overlays under the base map's place labels when there are any.
  const firstSymbol = style.layers.find((l) => l.type === "symbol")?.id;

  map.addSource("coverage", { type: "geojson", data: EMPTY });
  map.addSource("boundary", { type: "geojson", data: EMPTY });
  map.addSource("trails", { type: "geojson", data: EMPTY });
  map.addSource("trail-selected", { type: "geojson", data: EMPTY });
  map.addSource("event-track", { type: "geojson", data: EMPTY });
  map.addSource("event-points", { type: "geojson", data: EMPTY });
  map.addSource("aircraft", { type: "geojson", data: EMPTY });

  map.addLayer(
    {
      id: "coverage-fill",
      type: "fill",
      source: "coverage",
      layout: { visibility: "none" },
      paint: {
        "fill-color": [
          "case",
          ["<", ["get", "level"], 0],
          GAP_COLOR,
          ["interpolate", ["linear"], ["get", "level"], ...COVERAGE_STOPS.flat()],
        ] as ExpressionSpecification,
        "fill-opacity": ["case", ["<", ["get", "level"], 0], 0.32, 0.45],
        "fill-outline-color": ["case", ["<", ["get", "level"], 0], "rgba(201,42,42,0.6)", "rgba(43,138,62,0.25)"],
      },
    },
    firstSymbol,
  );
  map.addLayer(
    {
      id: "boundary-line",
      type: "line",
      source: "boundary",
      paint: { "line-color": "#1c7ed6", "line-width": 1.2, "line-opacity": 0.7 },
    },
    firstSymbol,
  );
  map.addLayer({
    id: "trails-line",
    type: "line",
    source: "trails",
    layout: { "line-cap": "round", "line-join": "round" },
    paint: {
      "line-color": altColor,
      "line-width": 1.5,
      "line-opacity": ["case", ["get", "coasting"], 0.2, 0.4],
    },
  });
  map.addLayer({
    id: "trail-selected-casing",
    type: "line",
    source: "trail-selected",
    layout: { "line-cap": "round", "line-join": "round" },
    paint: { "line-color": "#ffffff", "line-width": 5, "line-opacity": 0.85 },
  });
  map.addLayer({
    id: "trail-selected-line",
    type: "line",
    source: "trail-selected",
    layout: { "line-cap": "round", "line-join": "round" },
    paint: { "line-color": "#1864ab", "line-width": 2.5 },
  });
  const kindColor: ExpressionSpecification = [
    "match",
    ["get", "kind"],
    ...Object.entries(KIND_COLOR).flat(),
    "#c92a2a",
  ] as unknown as ExpressionSpecification;
  map.addLayer({
    id: "event-track-casing",
    type: "line",
    source: "event-track",
    layout: { "line-cap": "round", "line-join": "round" },
    paint: { "line-color": "#ffffff", "line-width": 6, "line-opacity": 0.9 },
  });
  map.addLayer({
    id: "event-track-line",
    type: "line",
    source: "event-track",
    layout: { "line-cap": "round", "line-join": "round" },
    paint: { "line-color": kindColor, "line-width": 3, "line-dasharray": [2, 1] },
  });
  map.addLayer({
    id: "event-points",
    type: "circle",
    source: "event-points",
    paint: {
      "circle-radius": ["case", ["==", ["get", "role"], "event"], 8, 4],
      "circle-color": ["case", ["==", ["get", "role"], "event"], kindColor, "#ffffff"],
      "circle-stroke-color": ["case", ["==", ["get", "role"], "event"], "#ffffff", "#c92a2a"],
      "circle-stroke-width": 2,
    },
  });
  map.addLayer({
    id: "aircraft-selected-halo",
    type: "circle",
    source: "aircraft",
    filter: ["==", ["get", "icao24"], ""],
    paint: {
      "circle-radius": 15,
      "circle-color": "rgba(24,100,171,0.15)",
      "circle-stroke-color": "#1864ab",
      "circle-stroke-width": 2,
    },
  });
  map.addLayer({
    id: "aircraft-icon",
    type: "symbol",
    source: "aircraft",
    layout: {
      "icon-image": "aircraft",
      "icon-size": ["interpolate", ["linear"], ["zoom"], 4, 0.75, 8, 1, 11, 1.2],
      "icon-rotate": ["get", "track"],
      "icon-rotation-alignment": "map",
      "icon-pitch-alignment": "map",
      "icon-allow-overlap": true,
      "icon-ignore-placement": true,
      "symbol-sort-key": ["case", ["get", "ground"], 0, ["max", 0, ["get", "alt"]]],
    },
    paint: {
      "icon-color": altColor,
      "icon-opacity": ["case", ["get", "coasting"], 0.45, 1],
      "icon-halo-color": "rgba(255,255,255,0.9)",
      "icon-halo-width": 1.2,
      "icon-halo-blur": 0.3,
    },
  });
  if (style.glyphs) {
    map.addLayer({
      id: "aircraft-label",
      type: "symbol",
      source: "aircraft",
      minzoom: 7,
      layout: {
        "text-field": ["get", "name"],
        "text-font": pickFont(style),
        "text-size": 11,
        "text-offset": [0, 1.5],
        "text-anchor": "top",
        "text-optional": true,
        "text-padding": 2,
      },
      paint: {
        "text-color": "#212529",
        "text-halo-color": "rgba(255,255,255,0.95)",
        "text-halo-width": 1.4,
        "text-opacity": ["case", ["get", "coasting"], 0.55, 1],
      },
    });
  }

  const src = (id: string) => map.getSource<GeoJSONSource>(id);
  return {
    setFrame(frame) {
      void src("aircraft")?.setData(frame.points);
      void src("trails")?.setData(frame.trails);
      void src("trail-selected")?.setData(frame.selectedTrail);
    },
    setCoverage(data) {
      void src("coverage")?.setData(data);
    },
    setCoverageVisible(on) {
      map.setLayoutProperty("coverage-fill", "visibility", on ? "visible" : "none");
    },
    setBoundary(data) {
      void src("boundary")?.setData(data);
    },
    setEvent(geometry) {
      void src("event-track")?.setData(geometry?.track ?? EMPTY);
      void src("event-points")?.setData(geometry?.points ?? EMPTY);
    },
    setSelected(icao24) {
      map.setFilter("aircraft-selected-halo", ["==", ["get", "icao24"], icao24 ?? ""]);
    },
  };
}

export function altitudeGradientCss(): string {
  const max = ALT_STOPS[ALT_STOPS.length - 1]?.[0] ?? 1;
  return `linear-gradient(to right, ${ALT_STOPS.map(([m, c]) => `${c} ${((m / max) * 100).toFixed(0)}%`).join(", ")})`;
}

export function coverageGradientCss(): string {
  return `linear-gradient(to right, ${COVERAGE_STOPS.map(([v, c]) => `${c} ${v * 100}%`).join(", ")})`;
}
