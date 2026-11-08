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
import {
  PROTOCOL_VERSION,
  type Aircraft,
  type AnomalyEvent,
  type CoverageMessage,
  type EventsMessage,
  type HelloMessage,
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
let eventsMsg: EventsMessage | null = null;
let eventsOpen = false;
/** Key of the event shown in detail, and the last copy of it (kept if it drops off the list). */
let selectedEvent: AnomalyEvent | null = null;

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
    if (selectedEvent) overlays.setEvent(eventGeometry(selectedEvent));
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
