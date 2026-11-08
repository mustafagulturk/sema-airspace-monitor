import { describe, expect, it } from "vitest";
import { parseMessage } from "./protocol";
import { backoffDelay, BACKOFF_MAX_MS, feedUrl } from "./feed";
import { coverageLevel, coverageToGeoJSON } from "./coverage";
import { buildFrame, geojsonBounds, SHORT_TAIL_POINTS } from "./features";
import type { Aircraft } from "./protocol";

describe("parseMessage", () => {
  it("accepts hello", () => {
    const r = parseMessage('{"type":"hello","protocol":1,"source":"replay","label":"x10","attribution":"OpenSky"}');
    expect(r.ok && r.message.type === "hello" && r.message.source).toBe("replay");
  });
  it("rejects other protocol versions", () => {
    const r = parseMessage('{"type":"hello","protocol":2,"source":"live","label":"","attribution":""}');
    expect(r).toMatchObject({ ok: false, error: "protocol" });
  });
  it("rejects bad JSON and unknown types", () => {
    expect(parseMessage("{")).toMatchObject({ ok: false, error: "json" });
    expect(parseMessage('{"type":"nope","protocol":1}')).toMatchObject({ ok: false, error: "shape" });
    expect(parseMessage('{"type":"snapshot","protocol":1}')).toMatchObject({ ok: false, error: "shape" });
  });
});

describe("backoff", () => {
  it("grows from ~1 s and caps at 30 s", () => {
    const mid = () => 0.5;
    expect(backoffDelay(0, mid)).toBe(1000);
    expect(backoffDelay(1, mid)).toBe(2000);
    expect(backoffDelay(4, mid)).toBe(16000);
    expect(backoffDelay(10, mid)).toBe(30000);
    expect(backoffDelay(50, () => 1)).toBeLessThanOrEqual(BACKOFF_MAX_MS);
  });
  it("applies jitter", () => {
    expect(backoffDelay(3, () => 0)).toBe(6000);
    expect(backoffDelay(3, () => 1)).toBe(10000);
  });
  it("builds ws/wss URLs", () => {
    expect(feedUrl({ protocol: "http:", host: "localhost:5173" })).toBe("ws://localhost:5173/ws");
    expect(feedUrl({ protocol: "https:", host: "sema.example" })).toBe("wss://sema.example/ws");
  });
});

describe("coverage", () => {
  it("log-scales counts and marks gaps", () => {
    expect(coverageLevel(0, 1000)).toBe(-1);
    expect(coverageLevel(1, 1000)).toBe(0);
    expect(coverageLevel(1000, 1000)).toBe(1);
    expect(coverageLevel(31.6227766, 1000)).toBeCloseTo(0.5, 6);
    expect(coverageLevel(5, 1)).toBe(1);
  });
  it("makes closed squares of cell_deg", () => {
    const fc = coverageToGeoJSON({
      type: "coverage", protocol: 1, cell_deg: 0.5, window_h: 24, time: 0, max: 10,
      cells: [[36, 26, 0], [36.5, 26, 10]],
    });
    expect(fc.features).toHaveLength(2);
    expect(fc.features[0]?.properties.level).toBe(-1);
    expect(fc.features[0]?.geometry.coordinates[0]).toEqual([[26, 36], [26.5, 36], [26.5, 36.5], [26, 36.5], [26, 36]]);
  });
});

describe("buildFrame", () => {
  const ac = (over: Partial<Aircraft>): Aircraft => ({
    id: 1, icao24: "4b8001", callsign: "THY1", country: "Turkey", state: "confirmed",
    t: 100, fix_t: 100, lat: 39, lon: 35, speed: 200, track: 90, sigma: 50, ground: false,
    alt: 9000, vrate: null, turn: null,
    trail: Array.from({ length: 10 }, (_, i) => [34 + i * 0.05, 39] as [number, number]),
    ...over,
  });
  it("extrapolates points and appends the current position to trails", () => {
    const f = buildFrame([ac({})], 160, null);
    const pt = f.points.features[0]!;
    expect(pt.geometry.coordinates[0]).toBeGreaterThan(35);
    expect(pt.properties).toMatchObject({ name: "THY1", alt: 9000, coasting: false });
    const tail = f.trails.features[0]!.geometry.coordinates;
    expect(tail).toHaveLength(SHORT_TAIL_POINTS + 1);
    expect(tail[tail.length - 1]).toEqual(pt.geometry.coordinates);
    expect(f.selectedTrail.features).toHaveLength(0);
  });
  it("emits the full trail for the selected aircraft", () => {
    const f = buildFrame([ac({}), ac({ id: 2, icao24: "4b8002" })], 100, "4b8002");
    expect(f.selectedTrail.features).toHaveLength(1);
    expect(f.selectedTrail.features[0]!.geometry.coordinates).toHaveLength(11);
  });
  it("marks ground, unknown altitude and coasting", () => {
    const f = buildFrame([ac({ ground: true }), ac({ id: 2, icao24: "x", alt: null, state: "coasting" })], 100, null);
    expect(f.points.features[0]!.properties.alt).toBe(0);
    expect(f.points.features[1]!.properties).toMatchObject({ alt: -1, coasting: true });
  });
});

describe("geojsonBounds", () => {
  it("finds bounds of nested geometries", () => {
    const g = {
      type: "FeatureCollection",
      features: [{ type: "Feature", properties: {}, geometry: { type: "MultiPolygon", coordinates: [[[[26, 36], [45, 42], [30, 40], [26, 36]]]] } }],
    };
    expect(geojsonBounds(g)).toEqual([[26, 36], [45, 42]]);
    expect(geojsonBounds({})).toBeNull();
  });
});
