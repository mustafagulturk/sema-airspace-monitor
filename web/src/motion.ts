// Pure motion helpers: server clock tracking and constant-velocity extrapolation.

/** Mean Earth radius (m), same as the backend's geo.cpp. */
export const EARTH_RADIUS_M = 6371008.8;
/** Never extrapolate further than this (s). */
export const MAX_EXTRAPOLATION_S = 600;

const DEG = Math.PI / 180;

/**
 * Maps the local monotonic clock (performance.now(), ms) to server time (unix s).
 * Replay servers may run faster than real time (`rate`).
 */
export class ServerClock {
  private base: number | null = null;
  private recvAtMs = 0;
  private rate = 1;

  /** Call with each snapshot's `clock` and the local receive time. */
  sync(serverTime: number, rate: number, localNowMs: number): void {
    this.base = serverTime;
    // 0 = paused (end of a replay); negative or non-numeric falls back to real time.
    this.rate = Number.isFinite(rate) && rate >= 0 ? rate : 1;
    this.recvAtMs = localNowMs;
  }

  get synced(): boolean {
    return this.base !== null;
  }

  /** Server time (unix s) at the given local time, or NaN before the first sync. */
  now(localNowMs: number): number {
    if (this.base === null) return Number.NaN;
    return this.base + ((localNowMs - this.recvAtMs) / 1000) * this.rate;
  }
}

/** Great-circle destination from (lat, lon) going `distM` metres on `bearingDeg`. */
export function destination(
  lat: number,
  lon: number,
  bearingDeg: number,
  distM: number,
): { lat: number; lon: number } {
  if (distM === 0) return { lat, lon };
  const d = distM / EARTH_RADIUS_M;
  const th = bearingDeg * DEG;
  const p1 = lat * DEG;
  const l1 = lon * DEG;
  const sinP2 = Math.sin(p1) * Math.cos(d) + Math.cos(p1) * Math.sin(d) * Math.cos(th);
  const p2 = Math.asin(Math.max(-1, Math.min(1, sinP2)));
  const l2 =
    l1 + Math.atan2(Math.sin(th) * Math.sin(d) * Math.cos(p1), Math.cos(d) - Math.sin(p1) * sinP2);
  let lonOut = l2 / DEG;
  lonOut = ((((lonOut + 180) % 360) + 360) % 360) - 180;
  return { lat: p2 / DEG, lon: lonOut };
}

export interface MotionState {
  lat: number;
  lon: number;
  /** time of the estimate (unix s) */
  t: number;
  /** m/s */
  speed: number;
  /** deg clockwise from north */
  track: number;
  ground: boolean;
  alt: number | null;
  vrate: number | null;
}

export interface Extrapolated {
  lat: number;
  lon: number;
  alt: number | null;
  /** extrapolation interval actually applied (s) */
  dt: number;
}

/** Clamped extrapolation interval for an estimate made at `t`, evaluated at `now`. */
export function clampDt(now: number, t: number): number {
  const dt = now - t;
  if (!Number.isFinite(dt) || dt <= 0) return 0;
  return Math.min(dt, MAX_EXTRAPOLATION_S);
}

/**
 * Constant-velocity prediction at server time `now` — the same model as the
 * backend's Kalman CV prediction. Aircraft on the ground are not moved.
 */
export function extrapolate(s: MotionState, now: number): Extrapolated {
  const dt = clampDt(now, s.t);
  if (s.ground) return { lat: s.lat, lon: s.lon, alt: s.alt, dt: 0 };
  const speed = Number.isFinite(s.speed) && s.speed > 0 ? s.speed : 0;
  const track = Number.isFinite(s.track) ? s.track : 0;
  const pos = destination(s.lat, s.lon, track, speed * dt);
  let alt = s.alt;
  if (alt !== null && s.vrate !== null && Number.isFinite(s.vrate)) {
    alt = Math.max(0, alt + s.vrate * dt);
  }
  return { lat: pos.lat, lon: pos.lon, alt, dt };
}

/** Haversine distance in metres (used by tests and diagnostics). */
export function distanceM(lat1: number, lon1: number, lat2: number, lon2: number): number {
  const dp = (lat2 - lat1) * DEG;
  const dl = (lon2 - lon1) * DEG;
  const a =
    Math.sin(dp / 2) ** 2 + Math.cos(lat1 * DEG) * Math.cos(lat2 * DEG) * Math.sin(dl / 2) ** 2;
  return 2 * EARTH_RADIUS_M * Math.asin(Math.min(1, Math.sqrt(a)));
}
