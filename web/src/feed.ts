// WebSocket client for the server's /ws feed, with exponential-backoff reconnect.

import { parseMessage, type ServerMessage } from "./protocol";

export type ConnectionState = "connecting" | "open" | "retrying";

export interface FeedHandlers {
  onMessage(msg: ServerMessage, receivedAtMs: number): void;
  onState(state: ConnectionState, retryInMs?: number): void;
  /** A frame was rejected (bad JSON, unknown shape, wrong protocol version). */
  onProtocolError(kind: "json" | "shape" | "protocol", detail: string): void;
}

export const BACKOFF_MIN_MS = 1000;
export const BACKOFF_MAX_MS = 30000;

/**
 * Delay before reconnect attempt number `attempt` (0-based): 1 s doubling to
 * 30 s, with ±25 % jitter. `rand` is injectable for tests.
 */
export function backoffDelay(attempt: number, rand: () => number = Math.random): number {
  const base = Math.min(BACKOFF_MAX_MS, BACKOFF_MIN_MS * 2 ** Math.max(0, attempt));
  const jitter = 0.75 + rand() * 0.5;
  return Math.round(Math.min(BACKOFF_MAX_MS, Math.max(BACKOFF_MIN_MS * 0.75, base * jitter)));
}

/** Same-origin ws(s)://host/ws URL. */
export function feedUrl(loc: Pick<Location, "protocol" | "host">, path = "/ws"): string {
  const scheme = loc.protocol === "https:" ? "wss:" : "ws:";
  return `${scheme}//${loc.host}${path}`;
}

export class Feed {
  private ws: WebSocket | null = null;
  private attempt = 0;
  private timer: ReturnType<typeof setTimeout> | null = null;
  private stopped = false;

  constructor(
    private readonly url: string,
    private readonly h: FeedHandlers,
  ) {}

  start(): void {
    this.stopped = false;
    this.connect();
    document.addEventListener("visibilitychange", this.onVisible);
    window.addEventListener("online", this.onOnline);
  }

  stop(): void {
    this.stopped = true;
    if (this.timer) clearTimeout(this.timer);
    this.timer = null;
    document.removeEventListener("visibilitychange", this.onVisible);
    window.removeEventListener("online", this.onOnline);
    this.ws?.close();
    this.ws = null;
  }

  /** Reconnect immediately if we are waiting on a backoff timer. */
  private readonly onVisible = (): void => {
    if (document.visibilityState === "visible") this.retryNow();
  };
  private readonly onOnline = (): void => this.retryNow();

  private retryNow(): void {
    if (this.stopped || this.ws || !this.timer) return;
    clearTimeout(this.timer);
    this.timer = null;
    this.connect();
  }

  private connect(): void {
    this.h.onState("connecting");
    let ws: WebSocket;
    try {
      ws = new WebSocket(this.url);
    } catch {
      this.scheduleRetry();
      return;
    }
    this.ws = ws;
    ws.onopen = () => {
      this.attempt = 0;
      this.h.onState("open");
    };
    ws.onmessage = (ev: MessageEvent) => {
      const recv = performance.now();
      if (typeof ev.data !== "string") return;
      const r = parseMessage(ev.data);
      if (r.ok) this.h.onMessage(r.message, recv);
      else this.h.onProtocolError(r.error, r.detail);
    };
    ws.onclose = () => {
      if (this.ws !== ws) return;
      this.ws = null;
      if (!this.stopped) this.scheduleRetry();
    };
    ws.onerror = () => {
      // onclose follows and handles the retry.
    };
  }

  private scheduleRetry(): void {
    const delay = backoffDelay(this.attempt++);
    this.h.onState("retrying", delay);
    this.timer = setTimeout(() => {
      this.timer = null;
      if (!this.stopped) this.connect();
    }, delay);
  }
}
