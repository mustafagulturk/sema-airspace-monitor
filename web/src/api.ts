// JSON API of the C++ server (GET /api/status, /api/replay, /api/stats).
// Types mirror include/sema/history.hpp and include/sema/traffic_stats.hpp.

export interface TimeRange {
  from: number;
  to: number;
}

export interface StatusResponse {
  mode: "live" | "replay";
  label: string;
  now: number;
  started_at: number;
  uptime_s: number;
  max_replay_s: number;
  max_stats_s: number;
  utc_offset_s: number;
  airports: number;
  /** operator, contact, identifier mode and retention, for the privacy page */
  site?: Record<string, unknown>;
  /** replayable window, null when there is no data yet */
  history: TimeRange | null;
  /** current unbroken stretch of data collection */
  streak: (TimeRange & { seconds: number }) | null;
}

/** [t, lat, lon, alt_m|null, speed_ms|null, track_deg|null, ground 0/1] */
export type ReplayFix = [number, number, number, number | null, number | null, number | null, number];

export interface ReplayAircraftJson {
  icao24: string;
  callsign: string;
  country: string;
  fixes: ReplayFix[];
}

export interface ReplayResponse {
  from: number;
  to: number;
  fixes: number;
  aircraft: ReplayAircraftJson[];
}

export interface DailyStats {
  date: string;
  flights: number;
  aircraft: number;
  departures: number;
  arrivals: number;
}

export interface HourStats {
  hour: number;
  total: number;
  avg: number;
}

export interface AirportStats {
  icao: string;
  iata: string;
  name: string;
  city: string;
  lat: number;
  lon: number;
  departures: number;
  arrivals: number;
  total: number;
}

export interface RouteStats {
  from: string;
  to: string;
  flights: number;
}

export interface StatsResponse {
  from: number;
  to: number;
  utc_offset_s: number;
  totals: { flights: number; aircraft: number; departures: number; arrivals: number; days: number };
  daily: DailyStats[];
  hours: HourStats[];
  peak_hour: number | null;
  busiest_day: string | null;
  airports: AirportStats[];
  routes: RouteStats[];
  /** [lat_min, lon_min, flights] */
  corridors: { cell_deg: number; max: number; cells: [number, number, number][] };
  completed_flights: number;
  open_flights: number;
  generated_at: number;
}

export class ApiError extends Error {
  constructor(
    readonly status: number,
    message: string,
  ) {
    super(message);
  }
}

function isObj(v: unknown): v is Record<string, unknown> {
  return typeof v === "object" && v !== null && !Array.isArray(v);
}

async function getJson(url: string, signal?: AbortSignal): Promise<unknown> {
  const res = await fetch(url, { signal, headers: { accept: "application/json" } });
  let body: unknown = null;
  try {
    body = await res.json();
  } catch {
    // fall through
  }
  if (!res.ok) {
    const msg = isObj(body) && typeof body.error === "string" ? body.error : `HTTP ${res.status}`;
    throw new ApiError(res.status, msg);
  }
  return body;
}

export function isStatus(v: unknown): v is StatusResponse {
  return isObj(v) && (v.mode === "live" || v.mode === "replay") && typeof v.now === "number";
}

export function isReplay(v: unknown): v is ReplayResponse {
  return isObj(v) && typeof v.from === "number" && Array.isArray(v.aircraft);
}

export function isStats(v: unknown): v is StatsResponse {
  return (
    isObj(v) &&
    isObj(v.totals) &&
    Array.isArray(v.daily) &&
    Array.isArray(v.hours) &&
    Array.isArray(v.airports) &&
    isObj(v.corridors)
  );
}

export async function fetchStatus(signal?: AbortSignal): Promise<StatusResponse> {
  const j = await getJson("/api/status", signal);
  if (!isStatus(j)) throw new ApiError(0, "beklenmeyen yanıt");
  return j;
}

export async function fetchReplay(from: number, to: number, signal?: AbortSignal): Promise<ReplayResponse> {
  const j = await getJson(`/api/replay?from=${Math.floor(from)}&to=${Math.ceil(to)}`, signal);
  if (!isReplay(j)) throw new ApiError(0, "beklenmeyen yanıt");
  return j;
}

export async function fetchStats(range: TimeRange | null, signal?: AbortSignal): Promise<StatsResponse> {
  const q = range ? `?from=${Math.floor(range.from)}&to=${Math.ceil(range.to)}` : "";
  const j = await getJson(`/api/stats${q}`, signal);
  if (!isStats(j)) throw new ApiError(0, "beklenmeyen yanıt");
  return j;
}
