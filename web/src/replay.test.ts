import { describe, expect, it } from "vitest";
import type { ReplayResponse } from "./api";
import {
  bearing,
  formatDuration,
  formatLocalTime,
  fromLocalInput,
  lastAtOrBefore,
  MAX_GAP_S,
  Playback,
  ReplayData,
  TAIL_S,
  toLocalInput,
} from "./replay";

const T0 = 1_762_549_200; // 2025-11-07 21:00:00 UTC

function sample(): ReplayResponse {
  return {
    from: T0,
    to: T0 + 3600,
    fixes: 6,
    aircraft: [
      {
        icao24: "aaa001",
        callsign: "THY1",
        country: "Turkey",
        fixes: [
          [T0 + 60, 39.0, 32.0, 10000, 230, 90, 0],
          [T0, 39.0, 31.0, 9000, 230, 90, 0], // out of order on purpose
          [T0 + 120, 39.0, 33.0, null, 230, 90, 0],
          [T0 + 120 + MAX_GAP_S + 1, 39.0, 35.0, 11000, 230, 90, 0],
        ],
      },
      { icao24: "bbb002", callsign: "", country: "Germany", fixes: [[T0 + 30, 40.0, 30.0, null, 0, 10, 1]] },
      { icao24: "ccc003", callsign: "", country: "", fixes: [] },
    ],
  };
}

describe("lastAtOrBefore", () => {
  it("binary searches", () => {
    const a = Float64Array.from([1, 3, 5]);
    expect(lastAtOrBefore(a, 0)).toBe(-1);
    expect(lastAtOrBefore(a, 1)).toBe(0);
    expect(lastAtOrBefore(a, 4)).toBe(1);
    expect(lastAtOrBefore(a, 9)).toBe(2);
  });
});

describe("bearing", () => {
  it("points east and north", () => {
    expect(bearing(0, 0, 0, 1)).toBeCloseTo(90, 6);
    expect(bearing(0, 0, 1, 0)).toBeCloseTo(0, 6);
    expect(bearing(0, 0, -1, 0)).toBeCloseTo(180, 6);
  });
});

describe("ReplayData", () => {
  const d = new ReplayData(sample());

  it("drops aircraft without reports and counts fixes", () => {
    expect(d.aircraftCount).toBe(2);
    expect(d.fixCount).toBe(5);
  });

  it("interpolates between reports", () => {
    const f = d.frameAt(T0 + 30);
    const a = f.find((x) => x.icao24 === "aaa001")!;
    expect(a.lon).toBeCloseTo(31.5, 9);
    expect(a.alt).toBeCloseTo(9500, 9);
    expect(a.track).toBeCloseTo(bearing(39, 31, 39, 32), 9);
    expect(a.t).toBe(T0 + 30);
    expect(a.fix_t).toBe(T0);
    expect(a.trail).toEqual([[31, 39]]);
    // Unknown altitude at one end: keep the known one.
    const b = d.frameAt(T0 + 90).find((x) => x.icao24 === "aaa001")!;
    expect(b.alt).toBe(10000);
    expect(b.trail.length).toBe(2);
  });

  it("hides aircraft before the first report, in long gaps and after the tail", () => {
    expect(d.frameAt(T0 - 1).length).toBe(0);
    expect(d.frameAt(T0 + 120 + 100).some((x) => x.icao24 === "aaa001")).toBe(false);
    // The single-report ground aircraft shows for TAIL_S after its report.
    expect(d.frameAt(T0 + 30 + TAIL_S).some((x) => x.icao24 === "bbb002")).toBe(true);
    expect(d.frameAt(T0 + 31 + TAIL_S).some((x) => x.icao24 === "bbb002")).toBe(false);
    const g = d.frameAt(T0 + 30).find((x) => x.icao24 === "bbb002")!;
    expect(g.ground).toBe(true);
    // Back after the gap, exactly on the report.
    const after = d.frameAt(T0 + 120 + MAX_GAP_S + 1).find((x) => x.icao24 === "aaa001")!;
    expect(after.lon).toBe(35);
  });
});

describe("Playback", () => {
  it("plays, pauses, seeks and stops at the end", () => {
    const p = new Playback(100, 200, 10);
    expect(p.now(0)).toBe(100);
    p.play(1000);
    expect(p.now(3000)).toBe(120);
    p.setSpeed(20, 3000);
    expect(p.now(4000)).toBe(140);
    p.pause(4000);
    expect(p.now(99999)).toBe(140);
    p.seek(190, 5000);
    p.play(5000);
    expect(p.now(6000)).toBe(200);
    expect(p.ended(6000)).toBe(true);
    p.pause(6000);
    p.play(7000); // restarts from the beginning once at the end
    expect(p.now(7000)).toBe(100);
    p.seek(-5, 7000);
    expect(p.now(7000)).toBe(100);
  });
});

describe("local time helpers", () => {
  const off = 3 * 3600;
  it("formats in Turkish local time", () => {
    expect(formatLocalTime(T0, off)).toBe("08.11.2025 00:00:00");
    expect(formatLocalTime(T0 + 61, off, false)).toBe("08.11.2025 00:01");
    expect(toLocalInput(T0, off)).toBe("2025-11-08T00:00");
  });
  it("round-trips datetime-local values", () => {
    expect(fromLocalInput("2025-11-08T00:00", off)).toBe(T0);
    expect(fromLocalInput("2025-11-08T00:00:30", off)).toBe(T0 + 30);
    expect(fromLocalInput("garbage", off)).toBeNaN();
  });
  it("formats durations", () => {
    expect(formatDuration(42 * 60)).toBe("42 dk");
    expect(formatDuration(5 * 3600 + 12 * 60)).toBe("5 sa 12 dk");
    expect(formatDuration(3 * 86400 + 4 * 3600)).toBe("3 gün 4 sa");
    expect(formatDuration(2 * 86400)).toBe("2 gün");
    expect(formatDuration(-1)).toBe("—");
  });
});
