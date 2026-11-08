// Turkish display formatting helpers (pure).

export const M_TO_FT = 3.28084;
export const MS_TO_KT = 1.943844;
export const MS_TO_KMH = 3.6;
export const MS_TO_FPM = 196.8504;

const nf0 = new Intl.NumberFormat("tr-TR", { maximumFractionDigits: 0 });

export function fmtInt(n: number): string {
  // Avoid "-0".
  const r = Math.round(n);
  return nf0.format(r === 0 ? 0 : r);
}

export function msToKt(ms: number): number {
  return ms * MS_TO_KT;
}
export function mToFt(m: number): number {
  return m * M_TO_FT;
}
export function msToFpm(ms: number): number {
  return ms * MS_TO_FPM;
}

/** "36.000 ft (10.973 m)", "yerde", or "—" when unknown. */
export function formatAltitude(altM: number | null, ground: boolean): string {
  if (ground) return "yerde";
  if (altM === null || !Number.isFinite(altM)) return "—";
  return `${fmtInt(mToFt(altM))} ft (${fmtInt(altM)} m)`;
}

/** "450 kt (833 km/sa)" — km/h is written "km/sa" in Turkish. */
export function formatSpeed(ms: number): string {
  if (!Number.isFinite(ms)) return "—";
  return `${fmtInt(msToKt(ms))} kt (${fmtInt(ms * MS_TO_KMH)} km/sa)`;
}

/** "+1.200 ft/dk", "−800 ft/dk", "0 ft/dk" (dk = dakika). */
export function formatVerticalRate(ms: number | null): string {
  if (ms === null || !Number.isFinite(ms)) return "—";
  const fpm = Math.round(msToFpm(ms) / 10) * 10;
  if (fpm === 0) return "0 ft/dk";
  const sign = fpm > 0 ? "+" : "−";
  return `${sign}${fmtInt(Math.abs(fpm))} ft/dk`;
}

/** 16-point Turkish compass: K (kuzey), D (doğu), G (güney), B (batı). */
export const COMPASS_TR = [
  "K", "KKD", "KD", "DKD", "D", "DGD", "GD", "GGD",
  "G", "GGB", "GB", "BGB", "B", "BKB", "KB", "KKB",
] as const;

export function normalizeDeg(deg: number): number {
  return ((deg % 360) + 360) % 360;
}

export function compassTr(deg: number): string {
  const i = Math.round(normalizeDeg(deg) / 22.5) % 16;
  return COMPASS_TR[i] ?? "K";
}

/** "045° KD" */
export function formatHeading(deg: number): string {
  if (!Number.isFinite(deg)) return "—";
  const d = Math.round(normalizeDeg(deg)) % 360;
  return `${String(d).padStart(3, "0")}° ${compassTr(d)}`;
}

/** "az önce", "12 sn önce", "3 dk önce", "2 sa önce". */
export function formatAgo(seconds: number): string {
  if (!Number.isFinite(seconds)) return "—";
  const s = Math.max(0, Math.floor(seconds));
  if (s < 1) return "az önce";
  if (s < 60) return `${s} sn önce`;
  const m = Math.floor(s / 60);
  if (m < 60) return `${m} dk önce`;
  const h = Math.floor(m / 60);
  return `${h} sa önce`;
}

export function trackStateLabel(state: string): string {
  switch (state) {
    case "confirmed":
      return "Onaylı";
    case "tentative":
      return "Geçici";
    case "coasting":
      return "Tahmin (sinyal yok)";
    default:
      return state;
  }
}

/** "±120 m", "±1,2 km" */
export function formatSigma(m: number): string {
  if (!Number.isFinite(m)) return "—";
  if (m >= 1000) {
    return `±${new Intl.NumberFormat("tr-TR", { maximumFractionDigits: 1 }).format(m / 1000)} km`;
  }
  return `±${fmtInt(m)} m`;
}

export function displayName(callsign: string, icao24: string): string {
  const c = callsign.trim();
  return c !== "" ? c : icao24.toUpperCase();
}
