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
import { fetchReplay, fetchStats, fetchStatus, type StatsResponse, type StatusResponse } from "./api";
import {
  formatDuration,
  formatLocalTime,
  fromLocalInput,
  Playback,
  ReplayData,
  toLocalInput,
} from "./replay";
import { airportLabel, barChartSvg, corridorsToGeoJSON, dailyBars, hourBars } from "./stats";
import {
  addOverlays,
  altitudeGradientCss,
  corridorGradientCss,
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
  formatIcao,
  formatSigma,
  formatSpeed,
  formatVerticalRate,
  trackStateLabel,
} from "./format";
import {
  PROTOCOL_VERSION,
  type Aircraft,
  type AnomalyEvent,
  type CoverageMessage,
  type EventsMessage,
  type HelloMessage,
  type IdentifierMode,
  type SnapshotMessage,
} from "./protocol";
import {
  eventBounds,
  eventFacts,
  eventGeometry,
  eventHeadline,
  formatEventTime,
  KIND_COLOR,
  KIND_LABEL,
} from "./events";

const TICK_MS = 250;
/** Faster ticks while replaying history, where time runs 10-120x. */
const HISTORY_TICK_MS = 100;
const DEFAULT_UTC_OFFSET_S = 3 * 3600;
/** Data older than this (server seconds) marks the feed as stale. */
const STALE_AFTER_S = 150;
const ENDED_CLOSE_MS = 2500;
const IDENTIFIER_NOTE: Record<IdentifierMode, string> = {
  hidden:
    "Uçak kimlikleri gizlidir: ICAO24 adresi yerine her gün değişen anonim bir kimlik kullanılır, çağrı kodu gösterilmez.",
  airline:
    "ICAO24 adresi yerine her gün değişen anonim bir kimlik kullanılır; yalnızca havayolu sefer kodları (ör. THY7) gösterilir, özel uçakların tescili gösterilmez.",
  full: "",
};

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
let eventsMsg: EventsMessage | null = null;
let eventsOpen = false;
/** Key of the event shown in detail, and the last copy of it (kept if it drops off the list). */
let selectedEvent: AnomalyEvent | null = null;

type Mode = "live" | "history" | "stats";
let mode: Mode = "live";
let status: StatusResponse | null = null;
let replayData: ReplayData | null = null;
let playback: Playback | null = null;
let replayAbort: AbortController | null = null;
let stats: StatsResponse | null = null;
let statsRange = 604800;
let statsAbort: AbortController | null = null;
let corridorsOn = true;

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
  aboutIdentifiers: $("about-identifiers"),
  btnEvents: $("btn-events") as HTMLButtonElement,
  eventsBadge: $("events-badge"),
  events: $("events"),
  eventsClose: $("events-close") as HTMLButtonElement,
  evSub: $("ev-sub"),
  evListView: $("ev-list-view"),
  evList: $("ev-list"),
  evEmpty: $("ev-empty"),
  evDetail: $("ev-detail"),
  evBack: $("ev-back") as HTMLButtonElement,
  evdKind: $("evd-kind"),
  evdTitle: $("evd-title"),
  evdSummary: $("evd-summary"),
  evdFacts: $("evd-facts"),
  evdAircraft: $("evd-aircraft") as HTMLButtonElement,
  aboutUptime: $("about-uptime"),
  modeLive: $("mode-live") as HTMLButtonElement,
  modeHistory: $("mode-history") as HTMLButtonElement,
  modeStats: $("mode-stats") as HTMLButtonElement,
  hbar: $("history-bar"),
  hStart: $("h-start") as HTMLInputElement,
  hDur: $("h-dur") as HTMLSelectElement,
  hLoad: $("h-load") as HTMLButtonElement,
  hInfo: $("h-info"),
  hPlay: $("h-play") as HTMLButtonElement,
  hSeek: $("h-seek") as HTMLInputElement,
  hSpeed: $("h-speed") as HTMLSelectElement,
  hTime: $("h-time"),
  stats: $("stats"),
  statsClose: $("stats-close") as HTMLButtonElement,
  sSub: $("s-sub"),
  sNote: $("s-note"),
  sBody: $("s-body"),
  kFlights: $("k-flights"),
  kAircraft: $("k-aircraft"),
  kPerDay: $("k-perday"),
  kPeak: $("k-peak"),
  sDaily: $("s-daily"),
  sHours: $("s-hours"),
  sAirports: document.querySelector("#s-airports tbody") as HTMLTableSectionElement,
  sRoutes: document.querySelector("#s-routes tbody") as HTMLTableSectionElement,
  sUptime: $("s-uptime"),
  btnCorridors: $("btn-corridors") as HTMLButtonElement,
  corrMax: $("corr-max"),
};
$("alt-bar").style.background = altitudeGradientCss();
$("cov-bar").style.background = coverageGradientCss();
$("corr-bar").style.background = corridorGradientCss();

function serverNow(): number {
  return clock.now(performance.now());
}

/** Time the map shows: the replay clock in history mode, else the server's. */
function viewNow(): number {
  return mode === "history" && playback ? playback.now(performance.now()) : serverNow();
}

function utcOffset(): number {
  return status?.utc_offset_s ?? DEFAULT_UTC_OFFSET_S;
}

// ---------------------------------------------------------------- status bar
function renderStatus(): void {
  if (mode === "history") {
    setText(el.count, playback ? `${fmtInt(byIcao.size)} uçak` : "— uçak");
    setText(el.age, playback ? formatLocalTime(viewNow(), utcOffset()) : "geçmiş");
  }
  const now = serverNow();
  const age = snapshot && clock.synced ? now - snapshot.data_time : Number.NaN;
  if (mode !== "history") {
    setText(el.count, snapshot ? `${fmtInt(snapshot.counts.tracked)} uçak` : "— uçak");
    setText(el.age, Number.isFinite(age) ? `son veri ${formatAgo(age)}` : "");
  }
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
  const now = viewNow();
  const p = extrapolate(a, now);
  setText(el.pTitle, displayName(a.callsign, a.icao24));
  setText(el.pSub, a.callsign.trim() ? formatIcao(a.icao24) : "Çağrı kodu gösterilmiyor");
  setText(el.pIcao, formatIcao(a.icao24));
  setText(el.pCountry, a.country || "—");
  setText(el.pAlt, formatAltitude(p.alt, a.ground));
  setText(el.pSpeed, formatSpeed(a.speed));
  setText(el.pTrack, formatHeading(a.track));
  setText(el.pVrate, a.ground ? "—" : formatVerticalRate(a.vrate));
  setText(el.pState, trackStateLabel(a.state));
  setText(el.pSeen, formatAgo(now - a.fix_t));
  setText(el.pSigma, formatSigma(a.sigma));
}

function isNarrow(): boolean {
  return window.matchMedia("(max-width: 640px)").matches;
}

function openPanel(icao24: string): void {
  // Phones have room for one bottom sheet at a time.
  if (isNarrow() && eventsOpen) closeEvents();
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
  const p = extrapolate(a, viewNow());
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
  if (e.key !== "Escape" || el.about.open) return;
  if (selected !== null) {
    e.preventDefault();
    closePanel();
  } else if (eventsOpen) {
    e.preventDefault();
    if (selectedEvent) showEventList();
    else closeEvents();
  }
});

// ---------------------------------------------------------------- events panel (phase 4)
function renderEventsBadge(): void {
  const active = eventsMsg?.active ?? 0;
  el.eventsBadge.hidden = active === 0;
  setText(el.eventsBadge, String(active));
  const emergency = eventsMsg?.events.some((e) => e.active && e.kind === "emergency") ?? false;
  el.eventsBadge.classList.toggle("badge-alert", emergency);
  el.btnEvents.title = active > 0 ? `${active} süren olay` : "Olay listesi";
}

function kindBadge(kind: AnomalyEvent["kind"]): HTMLElement {
  const b = document.createElement("span");
  b.className = "kind-badge";
  b.style.background = KIND_COLOR[kind];
  b.textContent = KIND_LABEL[kind];
  return b;
}

function renderEventList(): void {
  const events = eventsMsg?.events ?? [];
  const total = eventsMsg ? Object.values(eventsMsg.counts).reduce((a, b) => a + (b ?? 0), 0) : 0;
  setText(
    el.evSub,
    eventsMsg ? `${fmtInt(eventsMsg.active)} süren · toplam ${fmtInt(total)} (sunucu açıldığından beri)` : "",
  );
  el.evEmpty.hidden = events.length > 0;
  const items = events.map((e) => {
    const li = document.createElement("li");
    const btn = document.createElement("button");
    btn.type = "button";
    btn.className = "ev-item";
    if (e.active) btn.classList.add("ev-active");
    btn.addEventListener("click", () => selectEvent(e));
    const top = document.createElement("div");
    top.className = "ev-item-top";
    top.append(kindBadge(e.kind));
    const when = document.createElement("span");
    when.className = "muted small";
    when.textContent = e.active ? "sürüyor" : formatEventTime(e.start);
    top.append(when);
    const name = document.createElement("div");
    name.className = "ev-item-name";
    name.textContent = displayName(e.callsign, e.icao24);
    const line = document.createElement("div");
    line.className = "muted small";
    line.textContent = eventHeadline(e);
    btn.append(top, name, line);
    li.append(btn);
    return li;
  });
  el.evList.replaceChildren(...items);
}

function renderEventDetail(): void {
  const e = selectedEvent;
  if (!e) return;
  el.evdKind.style.background = KIND_COLOR[e.kind];
  setText(el.evdKind, KIND_LABEL[e.kind]);
  setText(el.evdTitle, displayName(e.callsign, e.icao24));
  setText(el.evdSummary, e.summary);
  const rows = eventFacts(e).map(([k, v]) => {
    const div = document.createElement("div");
    const dt = document.createElement("dt");
    dt.textContent = k;
    const dd = document.createElement("dd");
    dd.textContent = v;
    div.append(dt, dd);
    return div;
  });
  el.evdFacts.replaceChildren(...rows);
  el.evdAircraft.hidden = !byIcao.has(e.icao24);
}

function showEventList(): void {
  selectedEvent = null;
  overlays?.setEvent(null);
  el.evDetail.hidden = true;
  el.evListView.hidden = false;
  renderEventList();
}

function selectEvent(e: AnomalyEvent): void {
  selectedEvent = e;
  el.evListView.hidden = true;
  el.evDetail.hidden = false;
  renderEventDetail();
  overlays?.setEvent(eventGeometry(e));
  if (mapRef) {
    const narrow = isNarrow();
    const panelW = el.events.offsetWidth;
    mapRef.fitBounds(eventBounds(e), {
      padding: narrow
        ? { top: 70, bottom: el.events.offsetHeight + 20, left: 20, right: 20 }
        : { top: 80, bottom: 40, left: panelW + 40, right: 40 },
      maxZoom: 10,
      duration: 400,
    });
  }
  el.evBack.focus({ preventScroll: true });
}

function openEvents(): void {
  // Events belong to the live picture.
  if (mode !== "live") setMode("live");
  if (isNarrow() && selected !== null) closePanel();
  eventsOpen = true;
  el.events.hidden = false;
  el.btnEvents.setAttribute("aria-pressed", "true");
  document.body.classList.add("events-open");
  if (selectedEvent) renderEventDetail();
  else renderEventList();
  el.events.focus({ preventScroll: true });
}

function closeEvents(): void {
  eventsOpen = false;
  selectedEvent = null;
  overlays?.setEvent(null);
  el.events.hidden = true;
  el.evDetail.hidden = true;
  el.evListView.hidden = false;
  el.btnEvents.setAttribute("aria-pressed", "false");
  document.body.classList.remove("events-open");
}

el.btnEvents.addEventListener("click", () => (eventsOpen ? closeEvents() : openEvents()));
el.eventsClose.addEventListener("click", closeEvents);
el.evBack.addEventListener("click", showEventList);
el.evdAircraft.addEventListener("click", () => {
  if (selectedEvent) openPanel(selectedEvent.icao24);
});

function onEvents(m: EventsMessage): void {
  eventsMsg = m;
  renderEventsBadge();
  if (selectedEvent) {
    // Keep showing the selected event, with its latest state if it is still listed.
    const fresh = m.events.find((e) => e.key === selectedEvent?.key);
    if (fresh) {
      selectedEvent = fresh;
      overlays?.setEvent(eventGeometry(fresh));
    }
    if (eventsOpen) renderEventDetail();
  } else if (eventsOpen) {
    renderEventList();
  }
}

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
  setText(el.aboutIdentifiers, IDENTIFIER_NOTE[m.identifiers]);
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
  if (mode === "history") return; // the map shows the replay
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
      case "events":
        onEvents(msg);
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
  if (mode === "history") {
    updateHistory();
    return;
  }
  renderStatus();
  if (!snapshot || !clock.synced) return;
  const now = serverNow();
  if (overlays) overlays.setFrame(buildFrame(snapshot.aircraft, now, selectedEnded ? null : selected));
  if (selected !== null && !selectedEnded) renderPanel();
}

let tickTimer: ReturnType<typeof setInterval> | null = null;
function startTicking(): void {
  if (tickTimer === null) tickTimer = setInterval(update, mode === "history" ? HISTORY_TICK_MS : TICK_MS);
}
function restartTicking(): void {
  stopTicking();
  if (document.visibilityState === "visible") startTicking();
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
        '<a href="https://opensky-network.org" target="_blank" rel="noopener">Veri: OpenSky Network</a> | <a href="/gizlilik.html">Gizlilik ve künye</a>',
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
    if (selectedEvent) overlays.setEvent(eventGeometry(selectedEvent));
    if (stats) overlays.setCorridors(corridorsToGeoJSON(stats));
    overlays.setCorridorsVisible(mode === "stats" && corridorsOn);
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

// ---------------------------------------------------------------- modes
function setMode(next: Mode): void {
  if (next === mode) return;
  const prev = mode;
  mode = next;
  el.modeLive.setAttribute("aria-pressed", String(next === "live"));
  el.modeHistory.setAttribute("aria-pressed", String(next === "history"));
  el.modeStats.setAttribute("aria-pressed", String(next === "stats"));
  document.body.classList.toggle("history-on", next === "history");
  document.body.classList.toggle("stats-on", next === "stats");
  el.hbar.hidden = next !== "history";
  el.stats.hidden = next !== "stats";
  overlays?.setCorridorsVisible(next === "stats" && corridorsOn);
  if (selected !== null) closePanel();
  if (next !== "live" && eventsOpen) closeEvents();
  if (prev === "history") {
    // Back to the live picture.
    playback?.pause(performance.now());
    if (snapshot) byIcao = new Map(snapshot.aircraft.map((a) => [a.icao24, a]));
  }
  if (next === "history") {
    byIcao = new Map();
    void enterHistory();
  }
  if (next === "stats") void loadStats();
  document.title =
    next === "history" ? "Semâ — Geçmiş" : next === "stats" ? "Semâ — İstatistik" : hello?.source === "replay" ? "Semâ — Kayıttan oynatma" : "Semâ — Canlı Türkiye Hava Sahası";
  restartTicking();
  update();
}

el.modeLive.addEventListener("click", () => setMode("live"));
el.modeHistory.addEventListener("click", () => setMode("history"));
el.modeStats.addEventListener("click", () => setMode("stats"));
el.statsClose.addEventListener("click", () => setMode("live"));

async function refreshStatus(): Promise<StatusResponse | null> {
  try {
    status = await fetchStatus();
  } catch {
    return status;
  }
  const streak = status.streak;
  const text = streak
    ? `Kesintisiz veri toplama: ${formatDuration(streak.seconds)} (15 dakikadan uzun ara yok).`
    : "Kesintisiz çalışma süresi henüz yok.";
  setText(el.aboutUptime, text);
  setText(el.sUptime, text);
  return status;
}

// ---------------------------------------------------------------- history (replay a chosen range)
function setHistoryInfo(text: string, error = false): void {
  setText(el.hInfo, text);
  el.hInfo.classList.toggle("err", error);
}

async function enterHistory(): Promise<void> {
  const st = await refreshStatus();
  if (mode !== "history") return;
  if (!st) {
    setHistoryInfo("Sunucuya ulaşılamadı.", true);
    return;
  }
  if (!st.history) {
    setHistoryInfo("Henüz kayıtlı veri yok.", true);
    el.hLoad.disabled = true;
    return;
  }
  el.hLoad.disabled = false;
  const off = utcOffset();
  const { from, to } = st.history;
  el.hStart.min = toLocalInput(from, off);
  el.hStart.max = toLocalInput(to, off);
  for (const o of Array.from(el.hDur.options)) o.disabled = Number(o.value) > st.max_replay_s;
  if (!el.hStart.value) {
    const dur = Math.min(Number(el.hDur.value), st.max_replay_s);
    el.hStart.value = toLocalInput(Math.max(from, to - dur), off);
  }
  if (!replayData) {
    setHistoryInfo(`Kayıtlı aralık: ${formatLocalTime(from, off, false)} – ${formatLocalTime(to, off, false)}`);
    void loadReplay();
  }
}

async function loadReplay(): Promise<void> {
  const off = utcOffset();
  const start = fromLocalInput(el.hStart.value, off);
  const dur = Number(el.hDur.value);
  if (!Number.isFinite(start)) {
    setHistoryInfo("Başlangıç zamanını seçin.", true);
    return;
  }
  replayAbort?.abort();
  const ctl = new AbortController();
  replayAbort = ctl;
  el.hLoad.disabled = true;
  setHistoryInfo("Yükleniyor…");
  try {
    const r = await fetchReplay(start, start + dur, ctl.signal);
    if (ctl.signal.aborted) return;
    replayData = new ReplayData(r);
    const speed = Number(el.hSpeed.value) || 30;
    playback = new Playback(r.from, r.to, speed);
    el.hSeek.min = String(Math.floor(r.from));
    el.hSeek.max = String(Math.ceil(r.to));
    el.hSeek.value = el.hSeek.min;
    el.hSeek.disabled = false;
    el.hPlay.disabled = false;
    setHistoryInfo(
      replayData.fixCount > 0
        ? `${fmtInt(replayData.aircraftCount)} uçak, ${fmtInt(replayData.fixCount)} konum`
        : "Bu aralıkta veri yok.",
      replayData.fixCount === 0,
    );
    if (replayData.fixCount > 0) playback.play(performance.now());
  } catch (e) {
    if (!ctl.signal.aborted) setHistoryInfo(`Yüklenemedi: ${e instanceof Error ? e.message : String(e)}`, true);
  } finally {
    if (replayAbort === ctl) el.hLoad.disabled = false;
  }
  update();
}

function updateHistory(): void {
  const nowMs = performance.now();
  if (playback && replayData) {
    if (playback.playing && playback.ended(nowMs)) playback.pause(nowMs);
    const t = playback.now(nowMs);
    const frame = replayData.frameAt(t);
    byIcao = new Map(frame.map((a) => [a.icao24, a]));
    if (selected !== null && !byIcao.has(selected)) markSelectedEnded();
    if (overlays) overlays.setFrame(buildFrame(frame, t, selectedEnded ? null : selected));
    if (selected !== null && !selectedEnded) renderPanel();
    if (document.activeElement !== el.hSeek) el.hSeek.value = String(Math.round(t));
    setText(el.hTime, formatLocalTime(t, utcOffset()));
    const playing = playback.playing;
    setText(el.hPlay, playing ? "❚❚" : "▶");
    el.hPlay.setAttribute("aria-label", playing ? "Duraklat" : "Oynat");
  } else if (overlays) {
    overlays.setFrame(buildFrame([], 0, null));
  }
  renderStatus();
}

el.hLoad.addEventListener("click", () => void loadReplay());
el.hDur.addEventListener("change", () => void loadReplay());
el.hStart.addEventListener("change", () => void loadReplay());
el.hPlay.addEventListener("click", () => {
  if (!playback) return;
  const now = performance.now();
  if (playback.playing) playback.pause(now);
  else playback.play(now);
  update();
});
el.hSeek.addEventListener("input", () => {
  playback?.seek(Number(el.hSeek.value), performance.now());
  update();
});
el.hSpeed.addEventListener("change", () => {
  playback?.setSpeed(Number(el.hSpeed.value) || 30, performance.now());
});

// ---------------------------------------------------------------- statistics
const nf1 = new Intl.NumberFormat("tr-TR", { maximumFractionDigits: 1 });

function cell(text: string): HTMLTableCellElement {
  const td = document.createElement("td");
  td.textContent = text;
  return td;
}

function fillRows(body: HTMLTableSectionElement, rows: string[][], cols: number, empty: string): void {
  const frag = document.createDocumentFragment();
  if (rows.length === 0) {
    const tr = document.createElement("tr");
    const td = cell(empty);
    td.colSpan = cols;
    td.className = "empty";
    tr.append(td);
    frag.append(tr);
  }
  for (const r of rows) {
    const tr = document.createElement("tr");
    tr.append(...r.map(cell));
    frag.append(tr);
  }
  body.replaceChildren(frag);
}

function renderStats(s: StatsResponse): void {
  const off = s.utc_offset_s;
  setText(el.sSub, `${formatLocalTime(s.from, off, false)} – ${formatLocalTime(s.to, off, false)} (TSİ)`);
  const days = Math.max(1, (s.to - s.from) / 86400);
  setText(el.kFlights, fmtInt(s.totals.flights));
  setText(el.kAircraft, fmtInt(s.totals.aircraft));
  setText(el.kPerDay, nf1.format(s.totals.flights / days));
  setText(el.kPeak, s.peak_hour === null ? "—" : `${String(s.peak_hour).padStart(2, "0")}:00`);

  const daily = dailyBars(s);
  const busiest = s.daily.findIndex((d) => d.date === s.busiest_day);
  el.sDaily.innerHTML = barChartSvg(daily, {
    ariaLabel: "Günlük uçuş sayısı",
    labelEvery: Math.max(1, Math.ceil(daily.length / 7)),
    highlight: busiest >= 0 ? busiest : null,
  });
  el.sHours.innerHTML = barChartSvg(hourBars(s), {
    ariaLabel: "Saatlere göre havadaki uçuş sayısı",
    labelEvery: 3,
    highlight: s.peak_hour,
    formatValue: (v) => nf1.format(v),
  });
  const byIcaoCode = new Map(s.airports.map((a) => [a.icao, a]));
  const name = (icao: string) => {
    const a = byIcaoCode.get(icao);
    return a ? airportLabel(a) : icao;
  };
  fillRows(
    el.sAirports,
    s.airports.slice(0, 12).map((a) => [airportLabel(a), fmtInt(a.departures), fmtInt(a.arrivals)]),
    3,
    "Bu aralıkta kalkış/iniş görülmedi.",
  );
  fillRows(
    el.sRoutes,
    s.routes.map((r) => [`${name(r.from)} → ${name(r.to)}`, fmtInt(r.flights)]),
    2,
    "Kalkışı ve inişi birlikte görülen uçuş yok.",
  );
  setText(el.corrMax, `${fmtInt(s.corridors.max)} uçuş`);
  overlays?.setCorridors(corridorsToGeoJSON(s));
  overlays?.setCorridorsVisible(mode === "stats" && corridorsOn);
}

async function loadStats(): Promise<void> {
  statsAbort?.abort();
  const ctl = new AbortController();
  statsAbort = ctl;
  el.sBody.setAttribute("aria-busy", "true");
  const st = await refreshStatus();
  const notes: string[] = [];
  if (st?.mode === "replay") notes.push(`Kayıttan oynatma${st.label ? ` (${st.label})` : ""}: sayılar bu kayda aittir.`);
  try {
    // Replays end at the recording's end; live data at "now".
    const end = st?.mode === "replay" && st.history ? st.history.to + 1 : (st?.now ?? Date.now() / 1000);
    const s = await fetchStats({ from: end - statsRange, to: end }, ctl.signal);
    if (ctl.signal.aborted) return;
    stats = s;
    if (s.totals.flights === 0) notes.push("Bu aralıkta tamamlanmış uçuş yok.");
    renderStats(s);
    el.sNote.classList.remove("err");
  } catch (e) {
    if (ctl.signal.aborted) return;
    notes.push(`İstatistik alınamadı: ${e instanceof Error ? e.message : String(e)}`);
    el.sNote.classList.add("err");
  } finally {
    el.sBody.removeAttribute("aria-busy");
  }
  el.sNote.hidden = notes.length === 0;
  setText(el.sNote, notes.join(" "));
}

for (const b of Array.from(el.stats.querySelectorAll<HTMLButtonElement>(".seg .btn"))) {
  b.addEventListener("click", () => {
    statsRange = Number(b.dataset.range) || 604800;
    for (const o of Array.from(el.stats.querySelectorAll<HTMLButtonElement>(".seg .btn"))) {
      o.setAttribute("aria-pressed", String(o === b));
    }
    void loadStats();
  });
}

el.btnCorridors.addEventListener("click", () => {
  corridorsOn = !corridorsOn;
  el.btnCorridors.setAttribute("aria-pressed", String(corridorsOn));
  overlays?.setCorridorsVisible(mode === "stats" && corridorsOn);
});

el.btnAbout.addEventListener("click", () => void refreshStatus());

renderStatus();
void initMap();
feed.start();
startTicking();

// Exposed for debugging in the console only.
(window as unknown as { sema?: unknown }).sema = {
  get hello() { return hello; },
  get map() { return mapRef; },
  get mode() { return mode; },
  get stats() { return stats; },
  serverNow,
  viewNow,
};
