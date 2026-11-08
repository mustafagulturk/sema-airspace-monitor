// Browser-side replay of recorded position reports (pure, testable).
//
// Unlike the live view, both ends of every interval are known, so aircraft are
// placed by interpolating between consecutive reports instead of
// extrapolating from the last one.

import type { ReplayResponse } from "./api";
import type { Aircraft } from "./protocol";

/** Reports further apart than this (s) are a signal gap: the aircraft is hidden in between. */
export const MAX_GAP_S = 600;
/** After the last report the aircraft is moved on this long (s), then hidden. */
export const TAIL_S = 30;
/** Past reports drawn as the trail. */
export const TRAIL_POINTS = 20;

const DEG = Math.PI / 180;

interface Track {
  id: number;
  icao24: string;
  callsign: string;
  country: string;
  t: Float64Array;
  lat: Float64Array;
  lon: Float64Array;
  alt: (number | null)[];
  speed: (number | null)[];
  track: (number | null)[];
  ground: Uint8Array;
}

/** Initial great-circle bearing from a to b, degrees clockwise from north. */
export function bearing(lat1: number, lon1: number, lat2: number, lon2: number): number {
  const p1 = lat1 * DEG;
  const p2 = lat2 * DEG;
  const dl = (lon2 - lon1) * DEG;
  const y = Math.sin(dl) * Math.cos(p2);
  const x = Math.cos(p1) * Math.sin(p2) - Math.sin(p1) * Math.cos(p2) * Math.cos(dl);
  return ((Math.atan2(y, x) / DEG) % 360 + 360) % 360;
}

/** Index of the last element <= t, or -1. */
export function lastAtOrBefore(ts: Float64Array, t: number): number {
  let lo = 0;
  let hi = ts.length - 1;
  let ans = -1;
  while (lo <= hi) {
    const mid = (lo + hi) >> 1;
    if ((ts[mid] as number) <= t) {
      ans = mid;
      lo = mid + 1;
    } else {
      hi = mid - 1;
    }
  }
  return ans;
}

export class ReplayData {
  readonly from: number;
  readonly to: number;
  readonly fixCount: number;
  private readonly tracks: Track[];

  constructor(r: ReplayResponse) {
    this.from = r.from;
    this.to = r.to;
    let n = 0;
    this.tracks = [];
    r.aircraft.forEach((a, i) => {
      const fx = a.fixes.filter((f) => Array.isArray(f) && Number.isFinite(f[0]) && Number.isFinite(f[1]) && Number.isFinite(f[2]));
      fx.sort((x, y) => x[0] - y[0]);
      if (fx.length === 0) return;
      n += fx.length;
      this.tracks.push({
        id: i + 1,
        icao24: a.icao24,
        callsign: a.callsign ?? "",
        country: a.country ?? "",
        t: Float64Array.from(fx, (f) => f[0]),
        lat: Float64Array.from(fx, (f) => f[1]),
        lon: Float64Array.from(fx, (f) => f[2]),
        alt: fx.map((f) => f[3] ?? null),
        speed: fx.map((f) => f[4] ?? null),
        track: fx.map((f) => f[5] ?? null),
        ground: Uint8Array.from(fx, (f) => (f[6] ? 1 : 0)),
      });
    });
    this.fixCount = n;
  }

  get aircraftCount(): number {
    return this.tracks.length;
  }

  /** Every aircraft visible at time `t`, as the live map's Aircraft objects estimated at `t`. */
  frameAt(t: number): Aircraft[] {
    const out: Aircraft[] = [];
    for (const k of this.tracks) {
      const i = lastAtOrBefore(k.t, t);
      if (i < 0) continue;
      const t0 = k.t[i] as number;
      const last = i === k.t.length - 1;
      let lat: number, lon: number, alt: number | null, trk: number, spd: number;
      const ground = k.ground[i] === 1;
      if (!last && (k.t[i + 1] as number) - t0 <= MAX_GAP_S) {
        const t1 = k.t[i + 1] as number;
        const f = t1 > t0 ? (t - t0) / (t1 - t0) : 0;
        const la0 = k.lat[i] as number, lo0 = k.lon[i] as number;
        const la1 = k.lat[i + 1] as number, lo1 = k.lon[i + 1] as number;
        lat = la0 + (la1 - la0) * f;
        lon = lo0 + (lo1 - lo0) * f;
        const a0 = k.alt[i] ?? null, a1 = k.alt[i + 1] ?? null;
        alt = a0 !== null && a1 !== null ? a0 + (a1 - a0) * f : a0 ?? a1;
        const moved = Math.abs(la1 - la0) + Math.abs(lo1 - lo0) > 1e-5;
        trk = moved ? bearing(la0, lo0, la1, lo1) : k.track[i] ?? 0;
        spd = k.speed[i] ?? 0;
      } else if (t - t0 <= (last ? TAIL_S : 0) || t === t0) {
        // Just after the last report (or exactly on a report before a gap).
        lat = k.lat[i] as number;
        lon = k.lon[i] as number;
        alt = k.alt[i] ?? null;
        trk = k.track[i] ?? 0;
        spd = k.speed[i] ?? 0;
      } else {
        continue; // in a signal gap or gone
      }
      const trail: [number, number][] = [];
      for (let j = Math.max(0, i - TRAIL_POINTS + 1); j <= i; ++j) {
        if (t - (k.t[j] as number) <= 900) trail.push([k.lon[j] as number, k.lat[j] as number]);
      }
      out.push({
        id: k.id,
        icao24: k.icao24,
        callsign: k.callsign,
        country: k.country,
        state: "confirmed",
        t,
        fix_t: t0,
        lat,
        lon,
        speed: spd,
        track: trk,
        sigma: Number.NaN,
        ground,
        alt,
        vrate: null,
        turn: null,
        trail,
      });
    }
    return out;
  }
}

/** Playback clock: data time advancing at `speed` data seconds per real second. */
export class Playback {
  private base: number;
  private startedAt: number | null = null;
  speed: number;

  constructor(
    readonly from: number,
    readonly to: number,
    speed = 30,
  ) {
    this.base = from;
    this.speed = speed;
  }

  get playing(): boolean {
    return this.startedAt !== null;
  }

  now(nowMs: number): number {
    if (this.startedAt === null) return this.base;
    const t = this.base + ((nowMs - this.startedAt) / 1000) * this.speed;
    return Math.min(this.to, t);
  }

  /** True once the end has been reached (playback stops there). */
  ended(nowMs: number): boolean {
    return this.now(nowMs) >= this.to;
  }

  play(nowMs: number): void {
    if (this.playing) return;
    if (this.base >= this.to) this.base = this.from;
    this.startedAt = nowMs;
  }

  pause(nowMs: number): void {
    this.base = this.now(nowMs);
    this.startedAt = null;
  }

  seek(t: number, nowMs: number): void {
    this.base = Math.max(this.from, Math.min(this.to, t));
    if (this.startedAt !== null) this.startedAt = nowMs;
  }

  setSpeed(speed: number, nowMs: number): void {
    this.base = this.now(nowMs);
    if (this.startedAt !== null) this.startedAt = nowMs;
    this.speed = speed;
  }
}

function pad(n: number): string {
  return String(n).padStart(2, "0");
}

/** Wall-clock parts of Unix time `t` at a fixed UTC offset. */
function parts(t: number, offsetS: number): Date {
  return new Date((t + offsetS) * 1000);
}

/** "08.11.2026 14:05:12" at the given UTC offset (Turkey: +3 h, no DST). */
export function formatLocalTime(t: number, offsetS: number, withSeconds = true): string {
  const d = parts(t, offsetS);
  const time = `${pad(d.getUTCHours())}:${pad(d.getUTCMinutes())}${withSeconds ? `:${pad(d.getUTCSeconds())}` : ""}`;
  return `${pad(d.getUTCDate())}.${pad(d.getUTCMonth() + 1)}.${d.getUTCFullYear()} ${time}`;
}

/** Value for <input type="datetime-local">: "2026-11-08T14:05". */
export function toLocalInput(t: number, offsetS: number): string {
  const d = parts(t, offsetS);
  return `${d.getUTCFullYear()}-${pad(d.getUTCMonth() + 1)}-${pad(d.getUTCDate())}T${pad(d.getUTCHours())}:${pad(d.getUTCMinutes())}`;
}

/** Inverse of toLocalInput; NaN if the value is not a date-time. */
export function fromLocalInput(v: string, offsetS: number): number {
  const m = /^(\d{4})-(\d{2})-(\d{2})T(\d{2}):(\d{2})(?::(\d{2}))?$/.exec(v.trim());
  if (!m) return Number.NaN;
  const [, y, mo, d, h, mi, s] = m;
  return Date.UTC(+y!, +mo! - 1, +d!, +h!, +mi!, s ? +s : 0) / 1000 - offsetS;
}

/** "3 gün 4 sa", "5 sa 12 dk", "42 dk" */
export function formatDuration(seconds: number): string {
  if (!Number.isFinite(seconds) || seconds < 0) return "—";
  const d = Math.floor(seconds / 86400);
  const h = Math.floor((seconds % 86400) / 3600);
  const m = Math.floor((seconds % 3600) / 60);
  if (d > 0) return `${d} gün${h > 0 ? ` ${h} sa` : ""}`;
  if (h > 0) return `${h} sa${m > 0 ? ` ${m} dk` : ""}`;
  return `${m} dk`;
}
