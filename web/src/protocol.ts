// Server -> client message types (JSON text frames on /ws). Clients never send.

export const PROTOCOL_VERSION = 1;

export interface HelloMessage {
  type: "hello";
  protocol: number;
  source: "live" | "replay";
  label: string;
  attribution: string;
  /** What the server shows about each aircraft (SEMA_IDENTIFIERS). */
  identifiers: IdentifierMode;
}

export type IdentifierMode = "hidden" | "airline" | "full";

/** [lat_min, lon_min, count] */
export type CoverageCell = [number, number, number];

export interface CoverageMessage {
  type: "coverage";
  protocol: number;
  cell_deg: number;
  window_h: number;
  time: number;
  max: number;
  cells: CoverageCell[];
}

export type TrackState = "tentative" | "confirmed" | "coasting";

export interface Aircraft {
  id: number;
  icao24: string;
  callsign: string;
  country: string;
  state: TrackState;
  /** time of the state estimate (unix s) */
  t: number;
  /** time of the last real position fix (unix s) */
  fix_t: number;
  lat: number;
  lon: number;
  /** ground speed, m/s */
  speed: number;
  /** degrees clockwise from true north */
  track: number;
  /** 1-sigma position uncertainty, m */
  sigma: number;
  ground: boolean;
  alt: number | null;
  vrate: number | null;
  turn: number | null;
  /** [lon, lat] oldest first */
  trail: [number, number][];
}

export interface SnapshotMessage {
  type: "snapshot";
  protocol: number;
  data_time: number;
  clock: { time: number; rate: number };
  counts: { tracked: number; confirmed: number; coasting: number };
  aircraft: Aircraft[];
}

export type EventKind = "emergency" | "rapid_descent" | "holding" | "route_deviation" | "signal_loss";

/** [lon, lat, alt_m | null, t] oldest first */
export type EventTrackPoint = [number, number, number | null, number];

/** An anomaly raised by the server's detector (phase 4). */
export interface AnomalyEvent {
  id: number;
  /** kind:icao24:start, stable across server restarts */
  key: string;
  kind: EventKind;
  icao24: string;
  callsign: string;
  country: string;
  track_id: number;
  /** data time the anomaly began (unix s) */
  start: number;
  /** data time it was raised */
  detect: number;
  /** last time it held (unix s); == start while just opened */
  end: number;
  active: boolean;
  lat: number;
  lon: number;
  alt: number | null;
  /** one-line Turkish description */
  summary: string;
  /** kind-specific numbers, e.g. squawk, drop_m, radius_m, turn_deg, gap_s */
  details: Record<string, unknown>;
  track: EventTrackPoint[];
}

export interface EventsMessage {
  type: "events";
  protocol: number;
  time: number;
  /** events raised since the server started, per kind */
  counts: Partial<Record<EventKind, number>>;
  active: number;
  /** active first (newest first), then recently ended */
  events: AnomalyEvent[];
}

export type ServerMessage = HelloMessage | CoverageMessage | SnapshotMessage | EventsMessage;

export type ParseResult =
  | { ok: true; message: ServerMessage }
  | { ok: false; error: "json" | "shape" | "protocol"; detail: string };

function isObj(v: unknown): v is Record<string, unknown> {
  return typeof v === "object" && v !== null && !Array.isArray(v);
}

/** Parses and minimally validates one text frame. Unknown `type`s are a "shape" error. */
export function parseMessage(text: string): ParseResult {
  let raw: unknown;
  try {
    raw = JSON.parse(text);
  } catch (e) {
    return { ok: false, error: "json", detail: String(e) };
  }
  if (!isObj(raw)) return { ok: false, error: "shape", detail: "not an object" };
  if (raw.protocol !== PROTOCOL_VERSION) {
    return { ok: false, error: "protocol", detail: `protocol ${String(raw.protocol)}` };
  }
  switch (raw.type) {
    case "hello":
      if (raw.source !== "live" && raw.source !== "replay") break;
      return {
        ok: true,
        message: {
          type: "hello",
          protocol: PROTOCOL_VERSION,
          source: raw.source,
          label: typeof raw.label === "string" ? raw.label : "",
          attribution: typeof raw.attribution === "string" ? raw.attribution : "",
          identifiers:
            raw.identifiers === "full" || raw.identifiers === "airline" ? raw.identifiers : "hidden",
        },
      };
    case "coverage":
      if (typeof raw.cell_deg !== "number" || !Array.isArray(raw.cells)) break;
      return { ok: true, message: raw as unknown as CoverageMessage };
    case "snapshot":
      if (
        typeof raw.data_time !== "number" ||
        !isObj(raw.clock) ||
        typeof raw.clock.time !== "number" ||
        !Array.isArray(raw.aircraft)
      )
        break;
      return { ok: true, message: raw as unknown as SnapshotMessage };
    case "events":
      if (!Array.isArray(raw.events) || !isObj(raw.counts)) break;
      if (!raw.events.every((e) => isObj(e) && typeof e.key === "string" && Array.isArray(e.track))) break;
      return { ok: true, message: raw as unknown as EventsMessage };
  }
  return { ok: false, error: "shape", detail: `bad ${String(raw.type)} message` };
}
