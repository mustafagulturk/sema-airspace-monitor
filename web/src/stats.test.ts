import { describe, expect, it } from "vitest";
import type { StatsResponse } from "./api";
import { isStats } from "./api";
import { airportLabel, barChartSvg, corridorsToGeoJSON, dailyBars, escapeXml, hourBars, niceMax, shortDate } from "./stats";

function stats(): StatsResponse {
  return {
    from: 0,
    to: 86400,
    utc_offset_s: 10800,
    totals: { flights: 3, aircraft: 2, departures: 2, arrivals: 1, days: 2 },
    daily: [
      { date: "2026-11-07", flights: 2, aircraft: 2, departures: 2, arrivals: 1 },
      { date: "2026-11-08", flights: 1, aircraft: 1, departures: 0, arrivals: 0 },
    ],
    hours: Array.from({ length: 24 }, (_, h) => ({ hour: h, total: h === 9 ? 4 : 0, avg: h === 9 ? 2 : 0 })),
    peak_hour: 9,
    busiest_day: "2026-11-07",
    airports: [{ icao: "LTFJ", iata: "SAW", name: "Sabiha Gökçen", city: "Pendik, Istanbul", lat: 40.9, lon: 29.3, departures: 2, arrivals: 1, total: 3 }],
    routes: [{ from: "LTFJ", to: "LTAC", flights: 1 }],
    corridors: { cell_deg: 0.25, max: 4, cells: [[40.5, 30.75, 4], [39, 31, 1], [39, 32, 0]] },
    completed_flights: 3,
    open_flights: 0,
    generated_at: 0,
  };
}

describe("niceMax", () => {
  it("rounds up to 1/2/2.5/5 x 10^n", () => {
    expect(niceMax(0)).toBe(1);
    expect(niceMax(3)).toBe(5);
    expect(niceMax(18)).toBe(20);
    expect(niceMax(21)).toBe(25);
    expect(niceMax(100)).toBe(100);
    expect(niceMax(101)).toBe(200);
  });
});

describe("barChartSvg", () => {
  it("draws one bar per non-zero value, labels and a highlighted value", () => {
    const svg = barChartSvg(hourBars(stats()), { ariaLabel: "Saat <test>", labelEvery: 6, highlight: 9 });
    expect(svg.startsWith("<svg")).toBe(true);
    expect(svg).toContain('aria-label="Saat &lt;test&gt;"');
    expect((svg.match(/class="chart-bar/g) ?? []).length).toBe(1);
    expect((svg.match(/class="chart-hit"/g) ?? []).length).toBe(24);
    expect(svg).toContain('class="chart-bar hi"');
    expect(svg).toContain(">00<");
    expect(svg).toContain(">18<");
    expect(svg).not.toContain(">09<"); // only every 6th label
    expect(svg).toContain("<title>09:00–10:00 · günde ortalama 2 uçuş havada (toplam 4)</title>");
  });
});

describe("helpers", () => {
  it("escapes XML", () => {
    expect(escapeXml(`a<b>&"c'`)).toBe("a&lt;b&gt;&amp;&quot;c&#39;");
  });
  it("short dates in Turkish", () => {
    expect(shortDate("2026-11-08")).toBe("8 Kas");
    expect(shortDate("bad")).toBe("bad");
    expect(dailyBars(stats())[0]!.title).toContain("2 uçuş, 2 farklı uçak");
  });
  it("labels airports by city and IATA code", () => {
    expect(airportLabel(stats().airports[0]!)).toBe("Istanbul (SAW)");
    expect(airportLabel({ icao: "LTXX", iata: "", city: "", name: "" })).toBe("LTXX");
  });
  it("builds corridor squares on a sqrt scale and skips empty cells", () => {
    const g = corridorsToGeoJSON(stats());
    expect(g.features.length).toBe(2);
    expect(g.features[0]!.properties.level).toBe(1);
    expect(g.features[1]!.properties.level).toBeCloseTo(0.5, 9);
    expect(g.features[0]!.geometry.coordinates[0]![2]).toEqual([31, 40.75]);
  });
  it("validates stats responses", () => {
    expect(isStats(stats())).toBe(true);
    expect(isStats({ totals: {} })).toBe(false);
  });
});
