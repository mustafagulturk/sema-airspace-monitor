import { describe, expect, it } from "vitest";
import {
  compassTr,
  COMPASS_TR,
  displayName,
  formatAgo,
  formatAltitude,
  formatHeading,
  formatSigma,
  formatSpeed,
  formatVerticalRate,
  msToFpm,
  msToKt,
  mToFt,
  trackStateLabel,
} from "./format";

describe("unit conversions", () => {
  it("m/s to knots", () => {
    expect(msToKt(1)).toBeCloseTo(1.943844, 5);
    expect(msToKt(230)).toBeCloseTo(447.1, 1);
  });
  it("metres to feet", () => {
    expect(mToFt(10000)).toBeCloseTo(32808.4, 1);
  });
  it("m/s to ft/min", () => {
    expect(msToFpm(5.08)).toBeCloseTo(1000, 0);
  });
});

describe("formatAltitude", () => {
  it("formats ft and m with Turkish grouping", () => {
    expect(formatAltitude(10973, false)).toBe("36.001 ft (10.973 m)");
  });
  it("says yerde on the ground", () => {
    expect(formatAltitude(120, true)).toBe("yerde");
  });
  it("dash when unknown", () => {
    expect(formatAltitude(null, false)).toBe("—");
  });
});

describe("formatSpeed", () => {
  it("knots and km/sa", () => {
    expect(formatSpeed(231.5)).toBe("450 kt (833 km/sa)");
  });
});

describe("formatVerticalRate", () => {
  it("climb, descent, level, unknown", () => {
    expect(formatVerticalRate(5.08)).toBe("+1.000 ft/dk");
    expect(formatVerticalRate(-10.16)).toBe("−2.000 ft/dk");
    expect(formatVerticalRate(0.01)).toBe("0 ft/dk");
    expect(formatVerticalRate(null)).toBe("—");
  });
});

describe("compassTr", () => {
  it("has 16 points", () => {
    expect(COMPASS_TR).toHaveLength(16);
  });
  it.each([
    [0, "K"], [22.5, "KKD"], [45, "KD"], [67.5, "DKD"], [90, "D"], [112.5, "DGD"],
    [135, "GD"], [157.5, "GGD"], [180, "G"], [202.5, "GGB"], [225, "GB"], [247.5, "BGB"],
    [270, "B"], [292.5, "BKB"], [315, "KB"], [337.5, "KKB"], [359, "K"], [360, "K"], [-90, "B"], [11, "K"], [12, "KKD"],
  ])("%s° → %s", (deg, label) => {
    expect(compassTr(deg)).toBe(label);
  });
  it("formats heading with padding", () => {
    expect(formatHeading(45)).toBe("045° KD");
    expect(formatHeading(359.7)).toBe("000° K");
    expect(formatHeading(271)).toBe("271° B");
  });
});

describe("formatAgo", () => {
  it.each([
    [0, "az önce"],
    [0.4, "az önce"],
    [12, "12 sn önce"],
    [12.9, "12 sn önce"],
    [59, "59 sn önce"],
    [60, "1 dk önce"],
    [185, "3 dk önce"],
    [3600, "1 sa önce"],
    [-5, "az önce"],
  ])("%s s → %s", (s, text) => {
    expect(formatAgo(s)).toBe(text);
  });
  it("dash for NaN", () => {
    expect(formatAgo(Number.NaN)).toBe("—");
  });
});

describe("misc", () => {
  it("track state labels", () => {
    expect(trackStateLabel("confirmed")).toBe("Onaylı");
    expect(trackStateLabel("tentative")).toBe("Geçici");
    expect(trackStateLabel("coasting")).toBe("Tahmin (sinyal yok)");
  });
  it("sigma", () => {
    expect(formatSigma(120)).toBe("±120 m");
    expect(formatSigma(1234)).toBe("±1,2 km");
  });
  it("display name falls back to ICAO24", () => {
    expect(displayName("THY1  ", "4b8001")).toBe("THY1");
    expect(displayName("  ", "4b8001")).toBe("4B8001");
  });
});
