import "maplibre-gl/dist/maplibre-gl.css";
import "./style.css";

import * as maplibregl from "maplibre-gl";
// MapLibre v6 resolves its worker relative to its own module URL, which breaks
// once bundled; ship the self-contained worker as an asset and point at it.
import maplibreWorkerUrl from "maplibre-gl/dist/maplibre-gl-worker.mjs?url";
import { Feed, feedUrl, type ConnectionState } from "./feed";
import { ServerClock, extrapolate } from "./motion";
import { buildFrame, geojsonBounds } from "./features";
import { coverageToGeoJSON } from "./coverage";
import {
  addOverlays,
  altitudeGradientCss,
  coverageGradientCss,
  DEFAULT_STYLE_URL,
  FALLBACK_STYLE,
  loadBaseStyle,
  TURKEY_CENTER,
  type Overlays,
} from "./map";
import {
  displayName,
  fmtInt,
  formatAgo,
  formatAltitude,
  formatHeading,
  formatSigma,
  formatSpeed,
  formatVerticalRate,
  trackStateLabel,
} from "./format";
import { PROTOCOL_VERSION, type Aircraft, type CoverageMessage, type HelloMessage, type SnapshotMessage } from "./protocol";

const TICK_MS = 250;
/** Data older than this (server seconds) marks the feed as stale. */
const STALE_AFTER_S = 150;
const ENDED_CLOSE_MS = 2500;

function $(id: string): HTMLElement {
  const el = document.getElementById(id);
  if (!el) throw new Error(`#${id} missing`);
  return el;
}
function setText(el: HTMLElement, text: string): void {
  if (el.textContent !== text) el.textContent = text;
}

// ---------------------------------------------------------------- state
const clock = new ServerClock();
let hello: HelloMessage | null = null;
let coverage: CoverageMessage | null = null;
let snapshot: SnapshotMessage | null = null;
let byIcao = new Map<string, Aircraft>();
let selected: string | null = null;
let selectedEnded = false;
let endedTimer: ReturnType<typeof setTimeout> | null = null;
let conn: ConnectionState = "connecting";
let retryAt = 0;
let coverageOn = false;
let overlays: Overlays | null = null;
let mapRef: maplibregl.Map | null = null;

// ---------------------------------------------------------------- elements
const el = {
  dot: $("live-dot"),
  count: $("st-count"),
  age: $("st-age"),
  conn: $("st-conn"),
  btnCoverage: $("btn-coverage") as HTMLButtonElement,
  btnAbout: $("btn-about") as HTMLButtonElement,
  bannerReplay: $("banner-replay"),
  replayLabel: $("replay-label"),
  bannerError: $("banner-error"),
  bannerStyle: $("banner-style"),
  covLegend: $("coverage-legend"),
  covMeta: $("cov-meta"),
  covMax: $("cov-max"),
  panel: $("panel"),
  panelClose: $("panel-close") as HTMLButtonElement,
  pTitle: $("p-title"),
  pSub: $("p-sub"),
  pEnded: $("p-ended"),
  pIcao: $("p-icao"),
  pCountry: $("p-country"),
  pAlt: $("p-alt"),
  pSpeed: $("p-speed"),
  pTrack: $("p-track"),
  pVrate: $("p-vrate"),
  pState: $("p-state"),
  pSeen: $("p-seen"),
  pSigma: $("p-sigma"),
  about: $("about") as HTMLDialogElement,
  aboutClose: $("about-close") as HTMLButtonElement,
  aboutAttribution: $("about-attribution"),
};
$("alt-bar").style.background = altitudeGradientCss();
$("cov-bar").style.background = coverageGradientCss();

function serverNow(): number {
  return clock.now(performance.now());
}

// ---------------------------------------------------------------- status bar
function renderStatus(): void {
  const now = serverNow();
  const age = snapshot && clock.synced ? now - snapshot.data_time : Number.NaN;
  setText(el.count, snapshot ? `${fmtInt(snapshot.counts.tracked)} uçak` : "— uçak");
  setText(el.age, Number.isFinite(age) ? `son veri ${formatAgo(age)}` : "");
  let connText: string;
  switch (conn) {
    case "connecting":
      connText = "Bağlanıyor…";
      break;
    case "open":
      connText = "Bağlı";
      break;
    case "retrying": {
      const s = Math.max(0, Math.ceil((retryAt - performance.now()) / 1000));
      connText = `Bağlantı koptu, yeniden deneniyor${s > 0 ? ` (${s} sn)` : "…"}`;
      break;
    }
  }
  setText(el.conn, connText);
  el.conn.classList.toggle("down", conn === "retrying");
  el.dot.classList.toggle("live", conn === "open" && age < STALE_AFTER_S);
  el.dot.classList.toggle("stale", conn === "open" && !(age < STALE_AFTER_S));
  el.dot.classList.toggle("down", conn === "retrying");
  el.dot.title = conn === "open" ? (age < STALE_AFTER_S ? "Canlı" : "Veri gecikmeli") : "Bağlı değil";
}

// ---------------------------------------------------------------- detail panel
function renderPanel(): void {
  if (selected === null) return;
  const a = byIcao.get(selected);
  if (!a) return;
  const now = serverNow();
  const p = extrapolate(a, now);
  setText(el.pTitle, displayName(a.callsign, a.icao24));
  setText(el.pSub, a.callsign.trim() ? a.icao24.toUpperCase() : "Çağrı kodu yok");
  setText(el.pIcao, a.icao24.toUpperCase());
  setText(el.pCountry, a.country || "—");
  setText(el.pAlt, formatAltitude(p.alt, a.ground));
  setText(el.pSpeed, formatSpeed(a.speed));
  setText(el.pTrack, formatHeading(a.track));
  setText(el.pVrate, a.ground ? "—" : formatVerticalRate(a.vrate));
  setText(el.pState, trackStateLabel(a.state));
  setText(el.pSeen, formatAgo(now - a.fix_t));
  setText(el.pSigma, formatSigma(a.sigma));
}

function openPanel(icao24: string): void {
  if (endedTimer) clearTimeout(endedTimer);
  endedTimer = null;
  selected = icao24;
  selectedEnded = false;
  el.pEnded.hidden = true;
  el.panel.classList.remove("ended-state");
  const wasHidden = el.panel.hidden;
  el.panel.hidden = false;
  document.body.classList.add("panel-open");
  renderPanel();
  overlays?.setSelected(icao24);
  update();
  if (wasHidden) el.panel.focus({ preventScroll: true });
  keepSelectedVisible();
}

/** On phones the bottom sheet covers the lower map; pan the aircraft above it. */
function keepSelectedVisible(): void {
  const a = selected !== null ? byIcao.get(selected) : undefined;
  if (!mapRef || !a || !window.matchMedia("(max-width: 640px)").matches) return;
  const p = extrapolate(a, serverNow());
  const pt = mapRef.project([p.lon, p.lat]);
  const sheetTop = el.panel.getBoundingClientRect().top;
  const topbar = 60;
  if (pt.y > sheetTop - 30 || pt.y < topbar) {
    const targetY = topbar + (sheetTop - topbar) / 2;
    mapRef.panBy([0, pt.y - targetY], { duration: 300 });
  }
}

function closePanel(): void {
  if (endedTimer) clearTimeout(endedTimer);
  endedTimer = null;
  const hadFocus = el.panel.contains(document.activeElement);
  selected = null;
  selectedEnded = false;
  el.panel.hidden = true;
  document.body.classList.remove("panel-open");
  overlays?.setSelected(null);
  update();
  if (hadFocus) (document.querySelector(".maplibregl-canvas") as HTMLElement | null)?.focus({ preventScroll: true });
}

function markSelectedEnded(): void {
  if (selectedEnded) return;
  selectedEnded = true;
  el.pEnded.hidden = false;
  el.panel.classList.add("ended-state");
  overlays?.setSelected(null);
  endedTimer = setTimeout(closePanel, ENDED_CLOSE_MS);
}

el.panelClose.addEventListener("click", closePanel);
document.addEventListener("keydown", (e) => {
  if (e.key === "Escape" && !el.about.open && selected !== null) {
    e.preventDefault();
    closePanel();
  }
});

// ---------------------------------------------------------------- about / coverage buttons
el.btnAbout.addEventListener("click", () => el.about.showModal());
el.aboutClose.addEventListener("click", () => el.about.close());
el.about.addEventListener("click", (e) => {
  if (e.target === el.about) el.about.close(); // backdrop click
});

el.btnCoverage.addEventListener("click", () => {
  coverageOn = !coverageOn;
  el.btnCoverage.setAttribute("aria-pressed", String(coverageOn));
  el.covLegend.hidden = !coverageOn;
  document.body.classList.toggle("coverage-on", coverageOn);
  overlays?.setCoverageVisible(coverageOn);
});

// ---------------------------------------------------------------- message handling
function onHello(m: HelloMessage): void {
  hello = m;
  const replay = m.source === "replay";
  el.bannerReplay.hidden = !replay;
  setText(el.replayLabel, replay && m.label ? `— ${m.label}` : "");
  if (m.attribution) setText(el.aboutAttribution, m.attribution);
  document.title = replay ? "Semâ — Kayıttan oynatma" : "Semâ — Canlı Türkiye Hava Sahası";
}

function applyCoverage(): void {
  if (!coverage || !overlays) return;
  overlays.setCoverage(coverageToGeoJSON(coverage));
}

function onCoverage(m: CoverageMessage): void {
  coverage = m;
  const deg = new Intl.NumberFormat("tr-TR", { maximumFractionDigits: 2 }).format(m.cell_deg);
  setText(el.covMeta, `· ${deg}° hücre, son ${m.window_h} sa`);
  setText(el.covMax, `${fmtInt(m.max)} konum`);
  applyCoverage();
}

function onSnapshot(m: SnapshotMessage, recvAt: number): void {
  snapshot = m;
  clock.sync(m.clock.time, m.clock.rate, recvAt);
  byIcao = new Map(m.aircraft.map((a) => [a.icao24, a]));
  if (selected !== null) {
    if (byIcao.has(selected)) {
      if (selectedEnded) openPanel(selected); // came back before the panel closed
    } else {
      markSelectedEnded();
    }
  }
  update();
}

function showError(text: string | null): void {
  el.bannerError.hidden = text === null;
  if (text !== null) setText(el.bannerError, text);
}

const feed = new Feed(feedUrl(window.location), {
  onMessage(msg, recvAt) {
    showError(null);
    switch (msg.type) {
      case "hello":
        onHello(msg);
        break;
      case "coverage":
        onCoverage(msg);
        break;
      case "snapshot":
        onSnapshot(msg, recvAt);
        break;
    }
  },
  onState(state, retryInMs) {
    conn = state;
    if (state === "retrying") retryAt = performance.now() + (retryInMs ?? 0);
    renderStatus();
  },
  onProtocolError(kind, detail) {
    if (kind === "protocol") {
      showError(`Sunucu protokolü uyumsuz (${detail}, beklenen ${PROTOCOL_VERSION}). Sayfayı yenilemeyi deneyin.`);
    } else {
      console.warn("Semâ: ignored frame:", kind, detail);
    }
  },
});

// ---------------------------------------------------------------- tick loop
function update(): void {
  renderStatus();
  if (!snapshot || !clock.synced) return;
  const now = serverNow();
  if (overlays) overlays.setFrame(buildFrame(snapshot.aircraft, now, selectedEnded ? null : selected));
  if (selected !== null && !selectedEnded) renderPanel();
}

let tickTimer: ReturnType<typeof setInterval> | null = null;
function startTicking(): void {
  if (tickTimer === null) tickTimer = setInterval(update, TICK_MS);
}
function stopTicking(): void {
  if (tickTimer !== null) clearInterval(tickTimer);
  tickTimer = null;
}
document.addEventListener("visibilitychange", () => {
  if (document.visibilityState === "visible") {
    update();
    startTicking();
  } else {
    stopTicking();
  }
});

// ---------------------------------------------------------------- map
async function initMap(): Promise<void> {
  maplibregl.setWorkerUrl(maplibreWorkerUrl);
  const styleUrl = (import.meta.env.VITE_MAP_STYLE ?? "").trim() || DEFAULT_STYLE_URL;
  const fetched = await loadBaseStyle(styleUrl);
  if (!fetched) el.bannerStyle.hidden = false;

  const map = new maplibregl.Map({
    container: "map",
    style: fetched ? styleUrl : FALLBACK_STYLE,
    center: TURKEY_CENTER,
    zoom: 5,
    minZoom: 3,
    maxZoom: 14,
    maxPitch: 0,
    dragRotate: false,
    attributionControl: {
      compact: true,
      customAttribution:
        '<a href="https://opensky-network.org" target="_blank" rel="noopener">Veri: OpenSky Network</a>',
    },
  });
  mapRef = map;
  map.touchZoomRotate.disableRotation();
  map.keyboard.disableRotation();
  map.addControl(new maplibregl.NavigationControl({ showCompass: false }), "top-right");
  map.addControl(new maplibregl.ScaleControl({ unit: "metric" }), "bottom-right");

  map.on("load", () => {
    overlays = addOverlays(map, map.getStyle());
    overlays.setCoverageVisible(coverageOn);
    applyCoverage();
    update();
    void loadBoundary(map);
  });

  // Tap/click: pick the nearest aircraft within a finger-sized box.
  map.on("click", (e) => {
    if (!overlays) return;
    const r = 18;
    const hits = map.queryRenderedFeatures(
      [
        [e.point.x - r, e.point.y - r],
        [e.point.x + r, e.point.y + r],
      ],
      { layers: ["aircraft-icon"] },
    );
    let best: string | null = null;
    let bestD = Infinity;
    for (const f of hits) {
      if (f.geometry.type !== "Point") continue;
      const pt = map.project(f.geometry.coordinates as [number, number]);
      const d = (pt.x - e.point.x) ** 2 + (pt.y - e.point.y) ** 2;
      const icao = f.properties?.icao24;
      if (typeof icao === "string" && d < bestD) {
        bestD = d;
        best = icao;
      }
    }
    if (best !== null) openPanel(best);
    else if (selected !== null) closePanel();
  });
  map.on("mouseenter", "aircraft-icon", () => (map.getCanvas().style.cursor = "pointer"));
  map.on("mouseleave", "aircraft-icon", () => (map.getCanvas().style.cursor = ""));
}

async function loadBoundary(map: maplibregl.Map): Promise<void> {
  try {
    const res = await fetch("/boundary.geojson");
    if (!res.ok) return;
    const g = (await res.json()) as GeoJSON.GeoJSON;
    overlays?.setBoundary(g);
    const b = geojsonBounds(g);
    if (b) {
      const top = el.bannerReplay.hidden ? 70 : 100;
      // Keep the country clear of the legends on short/narrow screens.
      const legends = document.getElementById("legends");
      const narrow = window.matchMedia("(max-width: 640px)").matches;
      const bottom = narrow && legends ? legends.offsetHeight + 40 : 30;
      map.fitBounds(b, { padding: { top, bottom, left: 16, right: 16 }, duration: 0 });
    }
  } catch {
    // Boundary is decorative; keep the default view.
  }
}

renderStatus();
void initMap();
feed.start();
startTicking();

// Exposed for debugging in the console only.
(window as unknown as { sema?: unknown }).sema = {
  get hello() { return hello; },
  get map() { return mapRef; },
  serverNow,
};
