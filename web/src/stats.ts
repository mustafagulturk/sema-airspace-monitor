// Statistics panel helpers (pure): small SVG bar charts and the corridor layer.

import type { StatsResponse } from "./api";

export interface Bar {
  /** axis label, shown for some bars */
  label: string;
  value: number;
  /** native hover tooltip */
  title: string;
}

export function escapeXml(s: string): string {
  return s.replace(/[&<>"']/g, (c) => `&${({ "&": "amp", "<": "lt", ">": "gt", '"': "quot", "'": "#39" } as const)[c as "&"]};`);
}

/** A "nice" axis maximum: 1, 2, 2.5 or 5 times a power of ten, at least `v`. */
export function niceMax(v: number): number {
  if (!(v > 0)) return 1;
  const p = 10 ** Math.floor(Math.log10(v));
  for (const m of [1, 2, 2.5, 5, 10]) if (m * p >= v) return m * p;
  return 10 * p;
}

export interface BarChartOptions {
  width?: number;
  height?: number;
  /** show the label of every n-th bar */
  labelEvery?: number;
  /** index of a bar to emphasise and label with its value */
  highlight?: number | null;
  formatValue?: (v: number) => string;
  ariaLabel: string;
}

/**
 * Single-series vertical bar chart as SVG markup. One hue (the accent colour
 * via CSS class), thin bars with a 2px gap, a recessive baseline and gridline,
 * value on the emphasised bar only, and a <title> per bar for hover.
 */
export function barChartSvg(bars: readonly Bar[], o: BarChartOptions): string {
  const W = o.width ?? 320;
  const H = o.height ?? 120;
  const top = 16;
  const bottom = 18;
  const left = 2;
  const plotH = H - top - bottom;
  const n = Math.max(1, bars.length);
  const step = (W - left) / n;
  const gap = Math.min(2, step * 0.25);
  const bw = Math.max(1, step - gap);
  const max = niceMax(Math.max(0, ...bars.map((b) => b.value)));
  const fmt = o.formatValue ?? ((v: number) => String(Math.round(v)));
  const every = Math.max(1, o.labelEvery ?? 1);
  const parts: string[] = [];
  parts.push(
    `<svg viewBox="0 0 ${W} ${H}" width="100%" role="img" aria-label="${escapeXml(o.ariaLabel)}" class="chart" preserveAspectRatio="none">`,
  );
  // Gridline at the axis maximum, then the baseline.
  parts.push(`<line class="chart-grid" x1="0" x2="${W}" y1="${top}" y2="${top}"/>`);
  parts.push(`<text class="chart-axis" x="${W}" y="${top - 4}" text-anchor="end">${escapeXml(fmt(max))}</text>`);
  bars.forEach((b, i) => {
    const h = b.value > 0 ? Math.max(1, (b.value / max) * plotH) : 0;
    const x = left + i * step;
    const y = top + plotH - h;
    const cls = i === o.highlight ? "chart-bar hi" : "chart-bar";
    // Hit target spans the whole column, bigger than the mark.
    parts.push(
      `<g class="chart-col"><title>${escapeXml(b.title)}</title>` +
        `<rect class="chart-hit" x="${x.toFixed(2)}" y="${top}" width="${step.toFixed(2)}" height="${plotH}"/>` +
        (h > 0 ? `<rect class="${cls}" x="${x.toFixed(2)}" y="${y.toFixed(2)}" width="${bw.toFixed(2)}" height="${h.toFixed(2)}" rx="${Math.min(2, bw / 2).toFixed(2)}"/>` : "") +
        `</g>`,
    );
    if (i === o.highlight && b.value > 0) {
      const cx = Math.min(W - 12, Math.max(12, x + bw / 2));
      parts.push(`<text class="chart-value" x="${cx.toFixed(2)}" y="${(y - 3).toFixed(2)}" text-anchor="middle">${escapeXml(fmt(b.value))}</text>`);
    }
    if (i % every === 0) {
      parts.push(`<text class="chart-axis" x="${(x + bw / 2).toFixed(2)}" y="${H - 4}" text-anchor="middle">${escapeXml(b.label)}</text>`);
    }
  });
  parts.push(`<line class="chart-base" x1="0" x2="${W}" y1="${top + plotH}" y2="${top + plotH}"/>`);
  parts.push(`</svg>`);
  return parts.join("");
}

const nf = new Intl.NumberFormat("tr-TR", { maximumFractionDigits: 1 });

/** Bars for flights in the air per local hour of day (average per day). */
export function hourBars(s: StatsResponse): Bar[] {
  return s.hours.map((h) => ({
    label: String(h.hour).padStart(2, "0"),
    value: h.avg,
    title: `${String(h.hour).padStart(2, "0")}:00–${String((h.hour + 1) % 24).padStart(2, "0")}:00 · günde ortalama ${nf.format(h.avg)} uçuş havada (toplam ${h.total})`,
  }));
}

/** "2026-11-08" -> "8 Kas" */
export function shortDate(iso: string): string {
  const m = /^(\d{4})-(\d{2})-(\d{2})$/.exec(iso);
  if (!m) return iso;
  const months = ["Oca", "Şub", "Mar", "Nis", "May", "Haz", "Tem", "Ağu", "Eyl", "Eki", "Kas", "Ara"];
  return `${Number(m[3])} ${months[Number(m[2]) - 1] ?? ""}`;
}

export function dailyBars(s: StatsResponse): Bar[] {
  return s.daily.map((d) => ({
    label: shortDate(d.date),
    value: d.flights,
    title: `${shortDate(d.date)}: ${d.flights} uçuş, ${d.aircraft} farklı uçak, ${d.departures} kalkış, ${d.arrivals} iniş`,
  }));
}

export interface CorridorProps {
  flights: number;
  /** 0..1 on a square-root scale */
  level: number;
}

/** Corridor cells as GeoJSON squares with a normalised level for the sequential ramp. */
export function corridorsToGeoJSON(s: StatsResponse): GeoJSON.FeatureCollection<GeoJSON.Polygon, CorridorProps> {
  const d = s.corridors.cell_deg;
  const max = s.corridors.max > 0 ? s.corridors.max : 1;
  const features: GeoJSON.Feature<GeoJSON.Polygon, CorridorProps>[] = [];
  for (const [lat, lon, n] of s.corridors.cells) {
    if (!Number.isFinite(lat) || !Number.isFinite(lon) || !(n > 0)) continue;
    features.push({
      type: "Feature",
      properties: { flights: n, level: Math.sqrt(n / max) },
      geometry: {
        type: "Polygon",
        coordinates: [
          [
            [lon, lat],
            [lon + d, lat],
            [lon + d, lat + d],
            [lon, lat + d],
            [lon, lat],
          ],
        ],
      },
    });
  }
  return { type: "FeatureCollection", features };
}

/** Airport label: "İstanbul (IST)" falling back to the ICAO code. */
export function airportLabel(a: { icao: string; iata: string; city: string; name: string }): string {
  const city = (a.city || a.name).split(",").pop()?.trim() ?? "";
  const code = a.iata || a.icao;
  return city ? `${city} (${code})` : code;
}
