// Aircraft silhouette rendered to a signed distance field, for a recolourable
// (sdf: true) MapLibre icon. Nose points north; MapLibre rotates it by `track`.

/** Icon size in device pixels (drawn at pixelRatio 2 → 26 CSS px). */
export const ICON_SIZE = 52;
export const ICON_PIXEL_RATIO = 2;

/** SDF convention used by MapLibre/TinySDF: edge at 0.75, 8 px per 0.25*4 falloff. */
const SDF_RADIUS = 8;
const SDF_CUTOFF = 0.25;

function drawSilhouette(ctx: CanvasRenderingContext2D, s: number): void {
  // Coordinates in a 0..100 box, nose at top.
  const k = s / 100;
  ctx.save();
  ctx.scale(k, k);
  ctx.beginPath();
  ctx.moveTo(50, 12); // nose
  ctx.bezierCurveTo(53.5, 14, 54.5, 20, 54.5, 30);
  ctx.lineTo(54.5, 40);
  ctx.lineTo(86, 56); // right wing tip
  ctx.lineTo(86, 62);
  ctx.lineTo(54.5, 54);
  ctx.lineTo(53.5, 74);
  ctx.lineTo(65, 83); // right stabiliser
  ctx.lineTo(65, 88);
  ctx.lineTo(50, 84);
  ctx.lineTo(35, 88);
  ctx.lineTo(35, 83);
  ctx.lineTo(46.5, 74);
  ctx.lineTo(45.5, 54);
  ctx.lineTo(14, 62);
  ctx.lineTo(14, 56); // left wing tip
  ctx.lineTo(45.5, 40);
  ctx.lineTo(45.5, 30);
  ctx.bezierCurveTo(45.5, 20, 46.5, 14, 50, 12);
  ctx.closePath();
  ctx.fill();
  ctx.restore();
}

/**
 * Converts a binary mask (true = inside) of size n×n into SDF alpha values
 * (0..255) using a brute-force nearest-edge search — fine for a one-off 52² icon.
 */
export function maskToSdf(mask: boolean[], n: number): Uint8ClampedArray {
  const inside: number[] = [];
  const outside: number[] = [];
  for (let i = 0; i < n * n; i++) (mask[i] ? inside : outside).push(i);
  const out = new Uint8ClampedArray(n * n);
  for (let i = 0; i < n * n; i++) {
    const x = i % n;
    const y = (i / n) | 0;
    const others = mask[i] ? outside : inside;
    let best = Infinity;
    for (const j of others) {
      const dx = (j % n) - x;
      const dy = ((j / n) | 0) - y;
      const d2 = dx * dx + dy * dy;
      if (d2 < best) best = d2;
    }
    // Distance to the boundary between pixel centres, ≈ half a pixel inside the far cell.
    const d = Number.isFinite(best) ? Math.sqrt(best) - 0.5 : SDF_RADIUS;
    const signed = mask[i] ? -d : d; // negative inside
    out[i] = Math.round(255 * (1 - (signed / SDF_RADIUS + SDF_CUTOFF)));
  }
  return out;
}

/** RGBA ImageData whose alpha channel is the SDF; null when canvas is unavailable. */
export function createAircraftSdf(): ImageData | null {
  const n = ICON_SIZE;
  const canvas = document.createElement("canvas");
  canvas.width = n;
  canvas.height = n;
  const ctx = canvas.getContext("2d");
  if (!ctx) return null;
  ctx.fillStyle = "#000";
  // Leave room around the shape for the SDF falloff (halo).
  const pad = 6;
  ctx.translate(pad, pad);
  drawSilhouette(ctx, n - 2 * pad);
  const src = ctx.getImageData(0, 0, n, n).data;
  const mask: boolean[] = new Array<boolean>(n * n);
  for (let i = 0; i < n * n; i++) mask[i] = (src[i * 4 + 3] ?? 0) >= 128;
  const sdf = maskToSdf(mask, n);
  const img = ctx.createImageData(n, n);
  for (let i = 0; i < n * n; i++) {
    img.data[i * 4] = 255;
    img.data[i * 4 + 1] = 255;
    img.data[i * 4 + 2] = 255;
    img.data[i * 4 + 3] = sdf[i] ?? 0;
  }
  return img;
}
