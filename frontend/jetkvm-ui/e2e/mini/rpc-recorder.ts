import type { Page } from "@playwright/test";

// Records the JSON-RPC traffic of the page's "rpc" data channel and unhandled
// rejections, from the first script of every document. Entries carry a
// sequence number so a caller can read only what happened after a mark.

export interface RpcEntry {
  seq: number;
  direction: "in" | "out";
  id?: unknown;
  method?: string;
  error?: { code?: number; message?: string } | string;
  // An answer that carries a result member; a boolean result is kept.
  hasResult?: boolean;
  flag?: boolean;
  parseError?: string;
}

interface RecorderState {
  seq: number;
  rpc: RpcEntry[];
  rejections: { seq: number; reason: string }[];
}

const RECORDER = () => {
  const state = { seq: 0, rpc: [] as unknown[], rejections: [] as unknown[] };
  Object.defineProperty(window, "__rpcRecorder", { value: state });

  window.addEventListener("unhandledrejection", event => {
    state.rejections.push({ seq: state.seq++, reason: String(event.reason).slice(0, 500) });
  });

  const record = (direction: "in" | "out", channel: RTCDataChannel, data: unknown) => {
    if (channel.label !== "rpc" || typeof data !== "string") return;
    const entry: Record<string, unknown> = { seq: state.seq++, direction };
    try {
      const payload = JSON.parse(data);
      if (typeof payload?.method === "string") entry.method = payload.method;
      if (payload?.id !== undefined && payload?.id !== null) entry.id = payload.id;
      if (payload?.error) entry.error = payload.error;
      if (payload !== null && typeof payload === "object" && "result" in payload) {
        entry.hasResult = true;
        if (typeof payload.result === "boolean") entry.flag = payload.result;
      }
    } catch (error) {
      entry.parseError = String(error);
    }
    state.rpc.push(entry);
  };

  const send = RTCDataChannel.prototype.send;
  RTCDataChannel.prototype.send = function (this: RTCDataChannel, data: never) {
    record("out", this, data);
    return send.call(this, data);
  };
  const createDataChannel = RTCPeerConnection.prototype.createDataChannel;
  RTCPeerConnection.prototype.createDataChannel = function (
    this: RTCPeerConnection,
    ...args: Parameters<RTCPeerConnection["createDataChannel"]>
  ) {
    const channel = createDataChannel.apply(this, args);
    channel.addEventListener("message", event => record("in", channel, event.data));
    return channel;
  };
};

export async function recordRpc(page: Page): Promise<void> {
  await page.addInitScript(RECORDER);
}

export class RpcLog {
  private readonly state: RecorderState;

  constructor(state: RecorderState) {
    this.state = state;
  }

  /** Sequence number of the next entry; pass it as `since` to read what follows. */
  get mark(): number {
    return this.state.seq;
  }

  private methodOf(id: unknown): string | undefined {
    const key = JSON.stringify(id);
    return this.state.rpc.find(e => e.direction === "out" && JSON.stringify(e.id) === key)?.method;
  }

  private answersTo(method: string, since: number): RpcEntry[] {
    return this.state.rpc.filter(
      e => e.seq >= since && e.direction === "in" && this.methodOf(e.id) === method,
    );
  }

  /** Answers to `method` since `since` that carry a result. */
  successes(method: string, since = 0): number {
    return this.answersTo(method, since).filter(e => !e.error && e.hasResult).length;
  }

  /** Whether an answer to `method` since `since` had the result true. */
  answeredTrue(method: string, since = 0): boolean {
    return this.answersTo(method, since).some(e => e.flag === true);
  }

  /** Error answers and unparsable messages since `since`. */
  errors(since = 0): { method: string; error: unknown }[] {
    return this.state.rpc
      .filter(e => e.seq >= since && (e.parseError || (e.direction === "in" && e.error)))
      .map(e => ({ method: this.methodOf(e.id) ?? "<unknown>", error: e.parseError ?? e.error }));
  }

  /** Requests sent since `since` that have no answer yet: neither a result nor an error. */
  unanswered(since = 0): { id?: unknown; method?: string }[] {
    const answered = new Set(
      this.state.rpc
        .filter(e => e.direction === "in" && (e.hasResult || e.error))
        .map(e => JSON.stringify(e.id)),
    );
    return this.state.rpc
      .filter(e => e.seq >= since && e.direction === "out" && e.id !== undefined)
      .filter(e => !answered.has(JSON.stringify(e.id)))
      .map(({ id, method }) => ({ id, method }));
  }

  /** Events named `method` (device messages without an id) since `since`. */
  events(method: string, since = 0): number {
    return this.state.rpc.filter(
      e => e.seq >= since && e.direction === "in" && e.method === method && e.id === undefined,
    ).length;
  }

  /** Methods the page requested since `since`, in order. */
  requested(since = 0): string[] {
    return this.state.rpc
      .filter(e => e.seq >= since && e.direction === "out" && e.method !== undefined)
      .map(e => e.method!);
  }

  /** Unhandled promise rejections since `since`. */
  rejections(since = 0): string[] {
    return this.state.rejections.filter(r => r.seq >= since).map(r => r.reason);
  }
}

export async function rpcLog(page: Page): Promise<RpcLog> {
  const state = await page.evaluate(
    () => (window as unknown as { __rpcRecorder?: RecorderState }).__rpcRecorder,
  );
  if (!state) throw new Error("the RPC recorder is not installed; call recordRpc(page) first");
  return new RpcLog(state);
}
