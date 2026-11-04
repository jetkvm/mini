import { createHash } from "node:crypto";
import { expect, type Page } from "@playwright/test";
import { type OsServicesState } from "./os-services";

// The OS Services terminal and file drop channels, opened by the page as the
// UI opens them, without the UI: GUEST_HARNESS goes into the page with
// addInitScript before the page loads, and the functions below drive it.

/** What the page keeps for one guest channel. */
export interface ChannelView {
  label: string;
  /** The SCTP stream ID, as in a packet trace. */
  id: number | null;
  readyState: RTCDataChannelState;
  /** Binary messages, decoded as UTF-8. */
  output: string;
  binaryMessages: number;
  binaryBytes: number;
  /** Text messages, in order. */
  texts: string[];
  /** "binary", "text", "open", "close" and "error", in order. */
  events: string[];
}

export interface DropTransferView {
  transfer: number;
  size: number;
  credit: number;
  creditMessages: number;
  sent: number;
  status?: string;
  path?: string;
}

export interface DropView {
  readyState: RTCDataChannelState;
  transfers: DropTransferView[];
  /** Text messages that were not credit or done. */
  other: string[];
  events: string[];
}

export interface DropFileSpec {
  transfer: number;
  size: number;
  seed: number;
  /** Cancel the transfer once this many bytes went out. */
  cancelAfter?: number;
}

export interface GuestHarness {
  open(label: string): number;
  sendText(handle: number, text: string): void;
  sendBytes(handle: number, text: string): void;
  close(handle: number): void;
  view(handle: number): ChannelView;
  drop(label: string, files: DropFileSpec[]): number;
  dropView(handle: number): DropView;
  /** The next "guestfile_" channel opens ms after its first message. */
  holdNextDropOpen(ms: number): void;
  /** The text messages of the channel that holdNextDropOpen held. */
  heldDropMessages(): string[];
  fileBytes(seed: number, size: number): Uint8Array<ArrayBuffer>;
  /** Closes the newest "guest-terminal" channel, also one the UI opened. */
  closeNewestTerminal(): void;
}

declare global {
  interface Window {
    __e2eGuest?: GuestHarness;
  }
}

// From the first script of every document: keeps the page's peer connection
// (the one that opens "rpc") and opens guest channels on it, as the UI does.
export const GUEST_HARNESS = () => {
  // Every peer connection that opened "rpc"; the last is the page's.
  const peers: RTCPeerConnection[] = [];
  let holdOpenMs: number | undefined;
  let heldMessages: string[] = [];
  const terminals: RTCDataChannel[] = [];
  const create = RTCPeerConnection.prototype.createDataChannel;
  RTCPeerConnection.prototype.createDataChannel = function (
    this: RTCPeerConnection,
    ...args: Parameters<RTCPeerConnection["createDataChannel"]>
  ) {
    const channel = create.apply(this, args);
    if (channel.label === "rpc") peers.push(this);
    if (channel.label === "guest-terminal") terminals.push(channel);
    if (holdOpenMs !== undefined && channel.label.startsWith("guestfile_")) {
      holdOpen(channel, holdOpenMs);
      holdOpenMs = undefined;
    }
    return channel;
  };

  // Chrome can dispatch a drop channel's first message (the device's credit)
  // before its open event. A held channel always does: it
  // reads "connecting" and its open event comes ms after its first message.
  const holdOpen = (channel: RTCDataChannel, ms: number) => {
    let held = true;
    let releasing = false;
    heldMessages = [];
    Object.defineProperty(channel, "readyState", {
      get: () =>
        held ? "connecting" : Reflect.get(RTCDataChannel.prototype, "readyState", channel),
    });
    channel.addEventListener("open", event => {
      if (held) event.stopImmediatePropagation();
    });
    channel.addEventListener("message", (event: MessageEvent) => {
      if (typeof event.data === "string") heldMessages.push(event.data);
      if (releasing) return;
      releasing = true;
      window.setTimeout(() => {
        held = false;
        channel.dispatchEvent(new Event("open"));
      }, ms);
    });
  };

  // The bytes of a file: the same function as fileBytes() below.
  const fileBytes = (seed: number, size: number) => {
    const bytes = new Uint8Array(size);
    for (let i = 0; i < size; i++) {
      let x = (Math.imul(i, 0x9e3779b1) + seed) >>> 0;
      x ^= x >>> 15;
      x = Math.imul(x, 0x85ebca77) >>> 0;
      x ^= x >>> 13;
      bytes[i] = x & 0xff;
    }
    return bytes;
  };

  const channels: RTCDataChannel[] = [];
  const views: ChannelView[] = [];
  const drops: DropView[] = [];

  const openChannel = (label: string) => {
    const peer = peers.at(-1);
    if (!peer || peer.connectionState === "closed") throw new Error("no peer connection yet");
    const channel = peer.createDataChannel(label);
    channel.binaryType = "arraybuffer";
    return channel;
  };

  const harness: GuestHarness = {
    holdNextDropOpen(ms) {
      holdOpenMs = ms;
    },
    heldDropMessages() {
      return heldMessages;
    },
    fileBytes,
    closeNewestTerminal() {
      terminals.at(-1)!.close();
    },
    open(label) {
      const channel = openChannel(label);
      const view: ChannelView = {
        label,
        id: channel.id,
        readyState: channel.readyState,
        output: "",
        binaryMessages: 0,
        binaryBytes: 0,
        texts: [],
        events: [],
      };
      const decoder = new TextDecoder();
      const note = (event: string) => {
        view.events.push(event);
        view.readyState = channel.readyState;
      };
      channel.addEventListener("open", () => note("open"));
      channel.addEventListener("close", () => note("close"));
      channel.addEventListener("error", () => note("error"));
      channel.addEventListener("message", (event: MessageEvent) => {
        if (typeof event.data === "string") {
          view.texts.push(event.data);
          note("text");
          return;
        }
        const bytes = new Uint8Array(event.data as ArrayBuffer);
        view.output += decoder.decode(bytes, { stream: true });
        view.binaryMessages++;
        view.binaryBytes += bytes.byteLength;
        note("binary");
      });
      channels.push(channel);
      views.push(view);
      return channels.length - 1;
    },
    sendText(handle, text) {
      channels[handle].send(text);
    },
    sendBytes(handle, text) {
      channels[handle].send(new TextEncoder().encode(text));
    },
    close(handle) {
      channels[handle].close();
    },
    view(handle) {
      return { ...views[handle], readyState: channels[handle].readyState, id: channels[handle].id };
    },
    drop(label, files) {
      const channel = openChannel(label);
      // The UI's numbers: 32 KiB a message, sending paused above 96 KiB.
      const chunk = 32 * 1024;
      const highWater = 96 * 1024;
      channel.bufferedAmountLowThreshold = highWater;
      const view: DropView = {
        readyState: channel.readyState,
        transfers: files.map(file => ({
          transfer: file.transfer,
          size: file.size,
          credit: 0,
          creditMessages: 0,
          sent: 0,
        })),
        other: [],
        events: [],
      };
      const state = files.map(file => ({
        spec: file,
        bytes: fileBytes(file.seed, file.size),
        available: 0,
        pumping: false,
        done: false,
      }));
      const drained = () =>
        new Promise<void>(resolve => {
          if (channel.bufferedAmount <= highWater || channel.readyState !== "open")
            return resolve();
          const poll = window.setInterval(() => {
            if (channel.bufferedAmount <= highWater || channel.readyState !== "open") {
              window.clearInterval(poll);
              resolve();
            }
          }, 20);
        });
      const pump = async (index: number) => {
        const entry = state[index];
        const record = view.transfers[index];
        if (entry.pumping) return;
        entry.pumping = true;
        try {
          while (!entry.done && entry.available > 0 && record.sent < entry.spec.size) {
            if (entry.spec.cancelAfter !== undefined && record.sent >= entry.spec.cancelAfter) {
              entry.done = true;
              channel.send(JSON.stringify({ type: "cancel", transfer: entry.spec.transfer }));
              return;
            }
            const n = Math.min(chunk, entry.available, entry.spec.size - record.sent);
            await drained();
            if (entry.done || channel.readyState !== "open") return;
            const message = new Uint8Array(4 + n);
            new DataView(message.buffer).setUint32(0, entry.spec.transfer, true);
            message.set(entry.bytes.subarray(record.sent, record.sent + n), 4);
            channel.send(message);
            record.sent += n;
            entry.available -= n;
          }
        } finally {
          entry.pumping = false;
        }
      };
      const note = (event: string) => {
        view.events.push(event);
        view.readyState = channel.readyState;
      };
      // Credit that came before the open event is sent once it opens.
      channel.addEventListener("open", () => {
        note("open");
        state.forEach((_, index) => void pump(index));
      });
      channel.addEventListener("close", () => note("close"));
      channel.addEventListener("error", () => note("error"));
      channel.addEventListener("message", (event: MessageEvent) => {
        if (typeof event.data !== "string") {
          view.other.push("<binary>");
          return;
        }
        let message: {
          type?: string;
          transfer?: number;
          bytes?: number;
          status?: string;
          path?: string;
        };
        try {
          message = JSON.parse(event.data);
        } catch {
          view.other.push(event.data);
          return;
        }
        const index = files.findIndex(file => file.transfer === message.transfer);
        if (index < 0 || (message.type !== "credit" && message.type !== "done")) {
          view.other.push(event.data);
          return;
        }
        const record = view.transfers[index];
        if (message.type === "credit") {
          record.credit += message.bytes ?? 0;
          record.creditMessages++;
          state[index].available += message.bytes ?? 0;
          void pump(index);
        } else {
          record.status = message.status;
          record.path = message.path;
          state[index].done = true;
          // As the UI does: the browser closes the channel when all are done.
          if (state.every(entry => entry.done) && view.transfers.every(t => t.status))
            channel.close();
        }
        note(message.type);
      });
      drops.push(view);
      return drops.length - 1;
    },
    dropView(handle) {
      return JSON.parse(JSON.stringify(drops[handle])) as DropView;
    },
  };
  Object.defineProperty(window, "__e2eGuest", { value: harness });
};

/** The bytes the page sends for a file: see fileBytes() in the harness. */
export function fileBytes(seed: number, size: number): Buffer {
  const bytes = Buffer.alloc(size);
  for (let i = 0; i < size; i++) {
    let x = (Math.imul(i, 0x9e3779b1) + seed) >>> 0;
    x ^= x >>> 15;
    x = Math.imul(x, 0x85ebca77) >>> 0;
    x ^= x >>> 13;
    bytes[i] = x & 0xff;
  }
  return bytes;
}

export function sha256(data: string | Buffer): string {
  return createHash("sha256").update(data).digest("hex");
}

export function shellQuote(value: string): string {
  return `'${value.replace(/'/g, "'\\''")}'`;
}

export type GuestState = OsServicesState & {
  guestTerminal?: boolean;
  guestFile?: { receive?: boolean };
};

export function offersTerminalAndFiles(state: OsServicesState): boolean {
  const guest = state as GuestState;
  return (
    state.osServices.connected && guest.guestTerminal === true && guest.guestFile?.receive === true
  );
}

/** The closing line the UI parses: "\r\n[reason]\r\n". */
export const CLOSING_LINE = /^\r\n\[(.*)\]\r\n$/;

export async function openChannel(page: Page, label: string): Promise<number> {
  return page.evaluate(label => window.__e2eGuest!.open(label), label);
}

export async function channelView(page: Page, handle: number): Promise<ChannelView> {
  return page.evaluate(handle => window.__e2eGuest!.view(handle), handle);
}

export async function waitForChannel(
  page: Page,
  handle: number,
  description: string,
  predicate: (view: ChannelView) => boolean,
  timeout = 20_000,
): Promise<ChannelView> {
  let last: ChannelView | undefined;
  try {
    await expect
      .poll(
        async () => {
          last = await channelView(page, handle);
          return predicate(last);
        },
        { message: description, timeout },
      )
      .toBe(true);
  } catch (error) {
    // The message must be built after the poll: before it, last is unset.
    const view = last && { ...last, output: last.output.slice(-400) };
    throw new Error(`${description}; last: ${JSON.stringify(view)}`, { cause: error });
  }
  return last!;
}

export async function sendKeys(page: Page, handle: number, text: string): Promise<void> {
  await page.evaluate(({ handle, text }) => window.__e2eGuest!.sendBytes(handle, text), {
    handle,
    text,
  });
}

/** Runs `command` and waits for `expected` in the output after it. */
export async function run(
  page: Page,
  handle: number,
  command: string,
  expected: RegExp,
): Promise<string> {
  const before = (await channelView(page, handle)).output.length;
  await sendKeys(page, handle, `${command}\r`);
  const view = await waitForChannel(page, handle, `"${command}" prints ${expected}`, view =>
    expected.test(view.output.slice(before)),
  );
  return view.output.slice(before);
}

/** The channel ended with one closing line and no output after it. */
export function closingReason(view: ChannelView): string | undefined {
  const last = view.texts.at(-1);
  const match = last === undefined ? null : CLOSING_LINE.exec(last);
  return match?.[1];
}
