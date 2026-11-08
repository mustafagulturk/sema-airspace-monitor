import { describe, expect, it } from "vitest";
import { eventBounds, eventFacts, eventGeometry, eventHeadline, formatDuration, KIND_LABEL } from "./events";
import { parseMessage, type AnomalyEvent } from "./protocol";

function ev(over: Partial<AnomalyEvent> = {}): AnomalyEvent {
  return {
    id: 1,
    key: "emergency:4b8001:1762550400",
    kind: "emergency",
    icao24: "4b8001",
    callsign: "SMA700",
    country: "Turkey",
    track_id: 3,
    start: 1762550400,
    detect: 1762550465,
    end: 1762551000,
    active: false,
    lat: 39.5,
    lon: 33.2,
    alt: 10000,
    summary: "Acil durum kodu 7700 (genel acil durum)",
    details: { squawk: "7700", meaning: "genel acil durum" },
    track: [
      [33.0, 39.4, 10000, 1762550300],
      [33.2, 39.5, 10000, 1762550400],
      [33.4, 39.6, 9800, 1762550500],
    ],
    ...over,
  };
}

describe("events message", () => {
  it("parses a valid message and rejects malformed events", () => {
    const msg = { type: "events", protocol: 1, time: 1, counts: { emergency: 1 }, active: 0, events: [ev()] };
    const r = parseMessage(JSON.stringify(msg));
    expect(r.ok && r.message.type === "events" && r.message.events[0]?.callsign).toBe("SMA700");
    const bad = { ...msg, events: [{ id: 1 }] };
    expect(parseMessage(JSON.stringify(bad))).toMatchObject({ ok: false, error: "shape" });
  });
});

describe("formatting", () => {
  it("formats durations in Turkish units", () => {
    expect(formatDuration(42)).toBe("42 sn");
    expect(formatDuration(600)).toBe("10 dk");
    expect(formatDuration(3900)).toBe("1 sa 5 dk");
    expect(formatDuration(7200)).toBe("2 sa");
    expect(formatDuration(-1)).toBe("—");
  });
  it("builds kind-specific headlines", () => {
    expect(eventHeadline(ev())).toBe("7700 · genel acil durum");
    expect(eventHeadline(ev({ kind: "holding", details: { duration_s: 1200 } }))).toBe("20 dk bekleme");
    expect(eventHeadline(ev({ kind: "route_deviation", details: { turn_deg: 179 } }))).toBe("179° geri dönüş");
    expect(eventHeadline(ev({ kind: "signal_loss", details: { gap_s: 540 } }))).toBe("9 dk sinyal yok");
    expect(eventHeadline(ev({ kind: "rapid_descent", details: {} }))).toBe(KIND_LABEL.rapid_descent);
  });
  it("lists facts with the kind-specific rows", () => {
    const facts = new Map(eventFacts(ev()));
    expect(facts.get("Transponder kodu")).toBe("7700 (genel acil durum)");
    expect(facts.get("Süre")).toBe("10 dk");
    const active = new Map(eventFacts(ev({ active: true })));
    expect(active.has("Süre (sürüyor)")).toBe(true);
    const dev = new Map(eventFacts(ev({ kind: "route_deviation", details: { old_course_deg: 92, new_course_deg: 271, turn_deg: 179 } })));
    expect(dev.get("Önceki rota")).toBe("092° D");
    expect(dev.get("Yeni rota")).toBe("271° B");
  });
});

describe("geometry", () => {
  it("draws the track with markers at the event and the last fix", () => {
    const g = eventGeometry(ev());
    expect(g.track.features).toHaveLength(1);
    expect(g.track.features[0]?.geometry.coordinates).toHaveLength(3);
    expect(g.points.features.map((f) => f.properties.role)).toEqual(["event", "last"]);
    expect(g.points.features[1]?.geometry.coordinates).toEqual([33.4, 39.6]);
  });
  it("has no line for a single point and pads tiny bounds", () => {
    const one = ev({ track: [[33.2, 39.5, null, 1]] });
    expect(eventGeometry(one).track.features).toHaveLength(0);
    const [[w, s], [e, n]] = eventBounds(one);
    expect(e - w).toBeCloseTo(0.1);
    expect(n - s).toBeCloseTo(0.1);
    const [[w2], [e2]] = eventBounds(ev());
    expect(w2).toBe(33.0);
    expect(e2).toBe(33.4);
  });
});
