import { describe, expect, it } from "vitest";
import {
  clampDt,
  destination,
  distanceM,
  extrapolate,
  MAX_EXTRAPOLATION_S,
  ServerClock,
  type MotionState,
} from "./motion";

const base: MotionState = {
  lat: 39,
  lon: 35,
  t: 1000,
  speed: 230,
  track: 90,
  ground: false,
  alt: 10000,
  vrate: null,
};

describe("destination", () => {
  it("returns the start for zero distance", () => {
    expect(destination(39, 35, 123, 0)).toEqual({ lat: 39, lon: 35 });
  });
  it("one degree of latitude northwards ≈ 111.2 km", () => {
    const p = destination(0, 0, 0, 111195);
    expect(p.lat).toBeCloseTo(1, 3);
    expect(p.lon).toBeCloseTo(0, 9);
  });
  it("wraps longitude across the antimeridian", () => {
    const p = destination(0, 179.9, 90, 30000);
    expect(p.lon).toBeLessThan(-179);
  });
});

describe("extrapolate", () => {
  it("230 m/s due east for 60 s moves ≈ 13.8 km east", () => {
    const p = extrapolate(base, 1060);
    expect(p.dt).toBe(60);
    const d = distanceM(base.lat, base.lon, p.lat, p.lon);
    expect(d).toBeGreaterThan(13790);
    expect(d).toBeLessThan(13810);
    expect(p.lon).toBeGreaterThan(base.lon);
    expect(p.lat).toBeCloseTo(base.lat, 3); // great circle bends a hair towards the pole
    // flat-earth check: 13 800 m / (111 195 m * cos 39°)
    expect(p.lon - base.lon).toBeCloseTo(13800 / (111195 * Math.cos((39 * Math.PI) / 180)), 3);
  });
  it("track 0 moves latitude only", () => {
    const p = extrapolate({ ...base, track: 0, speed: 100 }, 1010);
    expect(p.lon).toBeCloseTo(base.lon, 10);
    expect(p.lat - base.lat).toBeCloseTo(1000 / 111195, 6);
  });
  it("track 180 moves south", () => {
    const p = extrapolate({ ...base, track: 180, speed: 100 }, 1010);
    expect(p.lat).toBeLessThan(base.lat);
    expect(p.lon).toBeCloseTo(base.lon, 10);
  });
  it("does not move backwards when now is before t", () => {
    const p = extrapolate(base, 900);
    expect(p.dt).toBe(0);
    expect(p.lat).toBe(base.lat);
    expect(p.lon).toBe(base.lon);
  });
  it("clamps extrapolation to 600 s", () => {
    const far = extrapolate(base, 1000 + 5000);
    const max = extrapolate(base, 1000 + MAX_EXTRAPOLATION_S);
    expect(far.dt).toBe(MAX_EXTRAPOLATION_S);
    expect(far.lat).toBeCloseTo(max.lat, 12);
    expect(far.lon).toBeCloseTo(max.lon, 12);
  });
  it("aircraft on the ground do not move", () => {
    const p = extrapolate({ ...base, ground: true, speed: 15, alt: 0 }, 1100);
    expect(p).toEqual({ lat: base.lat, lon: base.lon, alt: 0, dt: 0 });
  });
  it("extrapolates altitude with vrate and clamps at 0", () => {
    expect(extrapolate({ ...base, vrate: 10 }, 1030).alt).toBe(10300);
    expect(extrapolate({ ...base, alt: 200, vrate: -20 }, 1030).alt).toBe(0);
    expect(extrapolate({ ...base, alt: null, vrate: 5 }, 1030).alt).toBeNull();
    expect(extrapolate({ ...base, vrate: null }, 1030).alt).toBe(10000);
  });
  it("treats bad speed as stationary", () => {
    const p = extrapolate({ ...base, speed: Number.NaN }, 1030);
    expect(p.lat).toBe(base.lat);
    expect(p.lon).toBe(base.lon);
  });
});

describe("clampDt", () => {
  it("clamps to [0, 600] and handles NaN", () => {
    expect(clampDt(10, 20)).toBe(0);
    expect(clampDt(20, 10)).toBe(10);
    expect(clampDt(10000, 0)).toBe(600);
    expect(clampDt(Number.NaN, 0)).toBe(0);
  });
});

describe("ServerClock", () => {
  it("is NaN before sync", () => {
    expect(new ServerClock().now(0)).toBeNaN();
  });
  it("advances at real time with rate 1", () => {
    const c = new ServerClock();
    c.sync(1_700_000_000, 1, 5000);
    expect(c.synced).toBe(true);
    expect(c.now(5000)).toBe(1_700_000_000);
    expect(c.now(7500)).toBeCloseTo(1_700_000_002.5, 6);
  });
  it("scales by the replay rate", () => {
    const c = new ServerClock();
    c.sync(100, 10, 0);
    expect(c.now(1000)).toBe(110);
  });
  it("stands still with rate 0 (paused replay)", () => {
    const c = new ServerClock();
    c.sync(100, 0, 0);
    expect(c.now(5000)).toBe(100);
  });
  it("falls back to rate 1 for invalid rates", () => {
    const c = new ServerClock();
    c.sync(100, -2, 0);
    expect(c.now(1000)).toBe(101);
    c.sync(100, Number.NaN, 0);
    expect(c.now(1000)).toBe(101);
  });
});
