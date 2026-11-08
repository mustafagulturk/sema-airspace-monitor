// Anomaly events (phase 4): labels, formatting and map geometry (pure).

import type { AnomalyEvent, EventKind } from "./protocol";
import { fmtInt, formatAltitude, formatHeading } from "./format";

export const KIND_LABEL: Record<EventKind, string> = {
  emergency: "Acil durum kodu",
  rapid_descent: "Ani irtifa kaybı",
  holding: "Bekleme / daire çizme",
  route_deviation: "Rota sapması",
  signal_loss: "Uzun sinyal kaybı",
};

/** Badge colours, readable on light and dark backgrounds with white text. */
export const KIND_COLOR: Record<EventKind, string> = {
  emergency: "#c92a2a",
  rapid_descent: "#d9480f",
  holding: "#1864ab",
  route_deviation: "#5f3dc4",
  signal_loss: "#495057",
};

export const KIND_ORDER: EventKind[] = ["emergency", "rapid_descent", "holding", "route_deviation", "signal_loss"];

const SQUAWK_MEANING: Record<string, string> = {
  "7500": "yasa dışı müdahale",
  "7600": "telsiz arızası",
  "7700": "genel acil durum",
};

function num(d: Record<string, unknown>, key: string): number | null {
  const v = d[key];
  return typeof v === "number" && Number.isFinite(v) ? v : null;
}

/** "45 sn", "12 dk", "1 sa 5 dk" */
export function formatDuration(seconds: number): string {
  if (!Number.isFinite(seconds) || seconds < 0) return "—";
  const s = Math.round(seconds);
  if (s < 60) return `${s} sn`;
  const m = Math.round(s / 60);
  if (m < 60) return `${m} dk`;
  const h = Math.floor(m / 60);
  const rest = m % 60;
  return rest === 0 ? `${h} sa` : `${h} sa ${rest} dk`;
}

const timeFmt = new Intl.DateTimeFormat("tr-TR", {
  timeZone: "Europe/Istanbul",
  day: "numeric",
  month: "short",
  hour: "2-digit",
  minute: "2-digit",
});

/** "7 Kas 21:25" in Turkey time (TRT). */
export function formatEventTime(t: number): string {
  return Number.isFinite(t) ? timeFmt.format(new Date(t * 1000)) : "—";
}

/** Short kind-specific headline for the list, e.g. "7700 · genel acil durum". */
export function eventHeadline(e: AnomalyEvent): string {
  const d = e.details;
  switch (e.kind) {
    case "emergency": {
      const code = typeof d.squawk === "string" ? d.squawk : "";
      return `${code} · ${SQUAWK_MEANING[code] ?? "acil durum"}`;
    }
    case "rapid_descent": {
      const drop = num(d, "drop_m");
      const dur = num(d, "duration_s");
      return drop !== null && dur !== null
        ? `${fmtInt(drop * 3.28084)} ft / ${formatDuration(dur)}`
        : KIND_LABEL[e.kind];
    }
    case "holding": {
      const dur = num(d, "duration_s");
      return dur !== null ? `${formatDuration(dur)} bekleme` : KIND_LABEL[e.kind];
    }
    case "route_deviation": {
      const turn = num(d, "turn_deg");
      return turn !== null ? `${fmtInt(turn)}° geri dönüş` : KIND_LABEL[e.kind];
    }
    case "signal_loss": {
      const gap = num(d, "gap_s");
      return gap !== null ? `${formatDuration(gap)} sinyal yok` : KIND_LABEL[e.kind];
    }
  }
}

/** [label, value] rows for the event detail view. */
export function eventFacts(e: AnomalyEvent): [string, string][] {
  const d = e.details;
  const rows: [string, string][] = [
    ["Tür", KIND_LABEL[e.kind]],
    ["ICAO24", e.icao24.toUpperCase()],
    ["Tescil ülkesi", e.country || "—"],
    ["Başlangıç", formatEventTime(e.start)],
    [e.active ? "Süre (sürüyor)" : "Süre", formatDuration(e.end - e.start)],
    ["İrtifa (tespit anı)", formatAltitude(e.alt, false)],
  ];
  const add = (label: string, v: string | null) => {
    if (v !== null) rows.push([label, v]);
  };
  const fmtNum = (key: string, f: (n: number) => string) => {
    const n = num(d, key);
    return n === null ? null : f(n);
  };
  switch (e.kind) {
    case "emergency": {
      const code = typeof d.squawk === "string" ? d.squawk : null;
      add("Transponder kodu", code === null ? null : `${code} (${SQUAWK_MEANING[code] ?? "?"})`);
      break;
    }
    case "rapid_descent":
      add("Başlangıç irtifası", fmtNum("from_alt_m", (n) => formatAltitude(n, false)));
      add("Son irtifa", fmtNum("to_alt_m", (n) => formatAltitude(n, false)));
      add("Ortalama alçalma", fmtNum("rate_ms", (n) => `${fmtInt(n * 196.8504)} ft/dk`));
      break;
    case "holding":
      add("Yarıçap", fmtNum("radius_m", (n) => `${new Intl.NumberFormat("tr-TR", { maximumFractionDigits: 1 }).format(n / 1000)} km`));
      add("Uçulan yol", fmtNum("path_km", (n) => `${fmtInt(n)} km`));
      break;
    case "route_deviation":
      add("Önceki rota", fmtNum("old_course_deg", formatHeading));
      add("Yeni rota", fmtNum("new_course_deg", formatHeading));
      add("Dönüş", fmtNum("turn_deg", (n) => `${fmtInt(n)}°`));
      break;
    case "signal_loss":
      add("Sinyalsiz süre", fmtNum("gap_s", formatDuration));
      if (typeof d.reacquired === "boolean") add("Sinyal geri geldi mi", d.reacquired ? "Evet" : "Hayır");
      break;
  }
  return rows;
}

export interface EventGeometry {
  track: GeoJSON.FeatureCollection<GeoJSON.LineString, { kind: EventKind }>;
  points: GeoJSON.FeatureCollection<GeoJSON.Point, { role: "event" | "last"; kind: EventKind }>;
}

/** The aircraft's track around the event plus a marker where it was raised and one at its last fix. */
export function eventGeometry(e: AnomalyEvent): EventGeometry {
  const coords = e.track.map((p) => [p[0], p[1]] as [number, number]);
  const track: EventGeometry["track"] = {
    type: "FeatureCollection",
    features:
      coords.length >= 2 ? [{ type: "Feature", properties: { kind: e.kind }, geometry: { type: "LineString", coordinates: coords } }] : [],
  };
  const points: EventGeometry["points"] = {
    type: "FeatureCollection",
    features: [
      { type: "Feature", properties: { role: "event", kind: e.kind }, geometry: { type: "Point", coordinates: [e.lon, e.lat] } },
    ],
  };
  const last = coords[coords.length - 1];
  if (last) {
    points.features.push({ type: "Feature", properties: { role: "last", kind: e.kind }, geometry: { type: "Point", coordinates: last } });
  }
  return { track, points };
}

/** Bounds of the event track and marker, padded to at least ~0.1 degree. */
export function eventBounds(e: AnomalyEvent): [[number, number], [number, number]] {
  let w = e.lon, s = e.lat, east = e.lon, n = e.lat;
  for (const [lon, lat] of e.track) {
    w = Math.min(w, lon);
    east = Math.max(east, lon);
    s = Math.min(s, lat);
    n = Math.max(n, lat);
  }
  const pad = 0.05;
  if (east - w < 2 * pad) {
    w -= pad;
    east += pad;
  }
  if (n - s < 2 * pad) {
    s -= pad;
    n += pad;
  }
  return [[w, s], [east, n]];
}
