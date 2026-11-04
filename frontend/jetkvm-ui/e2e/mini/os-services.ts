import { spawn, type ChildProcess } from "node:child_process";
import { readFileSync } from "node:fs";
import * as path from "node:path";
import { fileURLToPath } from "node:url";
import { expect, test, type Page } from "@playwright/test";
import {
  SSH_OPTS,
  callJsonRpc,
  ensureRpcReady,
  getVideoStreamDimensions,
  withRpcPage,
} from "../helpers";

// IP-KVM OS Services (protocol version 5) between the Mini
// and the sender on the remote host: the Mini's browser RPCs and events, and
// the remote host's side of it: the sender's systemd user unit, its approved
// KVM list and log, the desktop clipboard and the USB interface in sysfs.
//
// The remote host (JETKVM_REMOTE_HOST) runs the sender as the user unit
// ipkvm-os-services.service of the SSH login's user, who is signed in to an
// unlocked GNOME Wayland session. The tests use passwordless sudo there only
// to read the user unit's journal, to run the transport probe and lsusb -v.
//
// A macOS remote host runs the sender as the launchd agent the app installs,
// for the SSH login's user, who is logged in at the desktop. The tests need
// no sudo there, and Homebrew's libusb for the USB descriptors and the probe.

export const SENDER_UNIT = "ipkvm-os-services.service";

export const FINGERPRINT = /^(?:[0-9A-F]{2}:){7}[0-9A-F]{2}$/;

export interface OsServicesSettings {
  terminal_enabled: boolean;
  files_enabled: boolean;
  capture_display: string;
  video_enabled: boolean;
  resolution: string;
  display_mode: string;
  // The Mini leaves an empty adaptation out.
  adaptation?: string;
  clipboard_enabled: boolean;
}

export interface OsServices {
  sender?: { version: string; displays: string[] };
  connected: boolean;
  video: boolean;
  videoRegistered: boolean;
  videoError?: string;
  clipboard: boolean;
  approvalPending: boolean;
  fingerprint: string;
}

export interface GuestClipboard {
  read: boolean;
  write: boolean;
  os: string;
}

/** The update of the sender to the newest OS Services release the Mini installs. */
export interface GuestUpdate {
  version: string;
  available: boolean;
  installable: boolean;
  automatic: boolean;
  phase?: "downloading" | "sending" | "installing" | "done" | "failed";
  sent?: number;
  size?: number;
  error?: string;
}

export interface OsServicesState {
  source: string;
  osServices: OsServices;
  guestClipboard?: GuestClipboard;
  guestUpdate?: GuestUpdate;
}

export interface RpcAnswer {
  result?: unknown;
  error?: { code?: number; message: string; data?: string };
}

export interface ClipboardText {
  status: string;
  text: string;
  truncated: boolean;
}

// --- Browser side ----------------------------------------------------------

/** One JSON-RPC exchange with the whole answer, so a test can check error codes. */
export async function rpcAnswer(
  page: Page,
  method: string,
  params: Record<string, unknown> = {},
  timeoutMs = 10_000,
): Promise<RpcAnswer> {
  const send = () =>
    page.evaluate(
      ({ method, params, timeoutMs }) =>
        new Promise<RpcAnswer>(resolve => {
          const hooks = window.__kvmTestHooks;
          if (!hooks) {
            resolve({ error: { message: "Test hooks not available" } });
            return;
          }
          hooks.sendJsonRpc(method, params, answer => resolve(answer as RpcAnswer), timeoutMs);
        }),
      { method, params, timeoutMs },
    );
  const answer = await send();
  if (answer.error?.message !== "RPC data channel not available") return answer;
  await ensureRpcReady(page, { timeoutMs: 20_000 });
  return send();
}

export async function osServicesState(page: Page): Promise<OsServicesState> {
  return (await callJsonRpc(page, "getVideoState")) as OsServicesState;
}

export async function bridgeSettings(page: Page): Promise<OsServicesSettings> {
  return (await callJsonRpc(page, "getOsServicesSettings")) as OsServicesSettings;
}

export async function setBridgeSettings(
  page: Page,
  settings: Partial<Record<keyof OsServicesSettings, unknown>>,
): Promise<OsServicesSettings> {
  return (await callJsonRpc(page, "setOsServicesSettings", { settings })) as OsServicesSettings;
}

/** The request that puts `settings` back, an empty adaptation included. */
export function settingsRequest(settings: OsServicesSettings): Record<string, unknown> {
  return { ...settings, adaptation: settings.adaptation ?? "" };
}

// --- Service video ---------------------------------------------------------

// 45 s for the switch (the Mini's timer) and 15 s for a disable to land.
export const SWITCH_TIMEOUT = 60_000;

export interface VideoState extends OsServicesState {
  ready: boolean;
  width: number;
  height: number;
}

/** The fault the device reports for service video, if any. */
export function videoError(state: OsServicesState): string | undefined {
  return (state.osServices as { videoError?: string }).videoError;
}

export function showing(width: number, height: number) {
  return (state: OsServicesState) => {
    const video = state as VideoState;
    return (
      video.source === "guest" &&
      video.osServices.video &&
      video.ready &&
      video.width === width &&
      video.height === height
    );
  };
}

export async function waitForGuestVideo(page: Page, width: number, height: number): Promise<void> {
  const state = await waitForState(
    page,
    `guest video at ${width}x${height}`,
    state => showing(width, height)(state) || videoError(state) !== undefined,
    SWITCH_TIMEOUT,
  );
  expect(videoError(state), "the device's service video error").toBeUndefined();
  await expect
    .poll(() => getVideoStreamDimensions(page), {
      message: `the browser decodes ${width}x${height}`,
      timeout: 30_000,
    })
    .toEqual({ width, height });
}

/** The frames of the video the browser has decoded so far. */
export async function framesDecoded(page: Page): Promise<number> {
  return page.evaluate(
    async () => (await window.__kvmTestHooks?.getInboundVideoStats())?.framesDecoded ?? 0,
  );
}

/**
 * Calls `probe` every `interval` ms until it returns a value other than
 * undefined; returns that value. A throw counts as not yet: a device that is
 * rebooting or a sender that is restarting fails a call or two on the way.
 * On timeout it fails with `message()` and the last error.
 */
export async function pollUntil<T>(
  probe: () => Promise<T | undefined>,
  {
    timeout,
    interval = 250,
    message,
  }: { timeout: number; interval?: number; message: () => string },
): Promise<T> {
  const deadline = Date.now() + timeout;
  let lastError: unknown;
  for (;;) {
    try {
      const value = await probe();
      if (value !== undefined) return value;
    } catch (error) {
      lastError = error;
    }
    if (Date.now() >= deadline) break;
    await new Promise(resolve => setTimeout(resolve, interval));
  }
  const cause = lastError instanceof Error ? `; last error: ${lastError.message}` : "";
  throw new Error(`timed out after ${timeout} ms: ${message()}${cause}`);
}

/** Polls getVideoState until `predicate` holds; returns the state that matched. */
export async function waitForState(
  page: Page,
  description: string,
  predicate: (state: OsServicesState) => boolean,
  timeout = 30_000,
): Promise<OsServicesState> {
  let last: OsServicesState | undefined;
  return pollUntil(
    async () => {
      last = await osServicesState(page);
      return predicate(last) ? last : undefined;
    },
    { timeout, message: () => `${description}; last state: ${JSON.stringify(last)}` },
  );
}

/**
 * The session is open, the clipboard is offered and the sender's details are
 * published. The sender reports its details in a message of its own, so they
 * can follow the clipboard offer by one state change.
 */
export function connectedWithClipboard(state: OsServicesState): boolean {
  return (
    state.osServices.connected &&
    !state.osServices.approvalPending &&
    state.guestClipboard !== undefined &&
    !!state.osServices.sender?.version
  );
}

export function connectedState(state: OsServicesState): boolean {
  return state.osServices.connected && !state.osServices.approvalPending;
}

export interface RecordedEvent {
  method: string;
  params: Record<string, unknown>;
  at: number;
}

// Keeps every videoInputState, osServicesSettings and otaState event the rpc
// channel delivers, with the browser's Date.now() at arrival, from the first
// script of every document.
const EVENT_RECORDER = () => {
  const events: unknown[] = [];
  const wanted = new Set(["videoInputState", "osServicesSettings", "otaState"]);
  Object.defineProperty(window, "__osServicesEvents", { value: events });
  const createDataChannel = RTCPeerConnection.prototype.createDataChannel;
  RTCPeerConnection.prototype.createDataChannel = function (
    this: RTCPeerConnection,
    ...args: Parameters<RTCPeerConnection["createDataChannel"]>
  ) {
    const channel = createDataChannel.apply(this, args);
    if (channel.label === "rpc") {
      channel.addEventListener("message", event => {
        if (typeof event.data !== "string") return;
        try {
          const message = JSON.parse(event.data) as { method?: string; params?: unknown };
          if (message.method && wanted.has(message.method))
            events.push({ method: message.method, params: message.params ?? {}, at: Date.now() });
        } catch {
          // Not JSON; not an event.
        }
      });
    }
    return channel;
  };
};

export async function recordOsServicesEvents(page: Page): Promise<void> {
  await page.addInitScript(EVENT_RECORDER);
}

export async function recordedEvents(
  page: Page,
  method: string,
  since: number,
): Promise<RecordedEvent[]> {
  const events = await page.evaluate(
    () => (window as unknown as { __osServicesEvents?: RecordedEvent[] }).__osServicesEvents ?? [],
  );
  return events.filter(event => event.method === method && event.at >= since);
}

// --- Remote host side -------------------------------------------------------

export interface CommandResult {
  status: number;
  stdout: Buffer;
  stderr: string;
}

// The systemd user manager and the desktop session of the SSH login's user.
export const USER_SESSION =
  'export XDG_RUNTIME_DIR="${XDG_RUNTIME_DIR:-/run/user/$(id -u)}"; ' +
  'export DBUS_SESSION_BUS_ADDRESS="${DBUS_SESSION_BUS_ADDRESS:-unix:path=$XDG_RUNTIME_DIR/bus}"; ';

// The Wayland display the sender's unit runs with: GNOME imports it into the
// user manager's environment.
const WAYLAND_SESSION =
  USER_SESSION +
  "eval \"$(systemctl --user show-environment | grep -E '^(WAYLAND_DISPLAY|DISPLAY|XAUTHORITY)=' | sed 's/^/export /')\"; ";

export type ClipboardTool = "wl-clipboard" | "gtk-x11" | "pbcopy";

// The remote clock in ms. uutils date ignores %3N's width, so %N and shell
// arithmetic instead.
const NOW_MS = "$(($(date +%s%N) / 1000000))";

// --- The macOS remote host ---
//
// The sender's launchd agent, as the app installs it, and
// the log launchd writes the sender's output to, without time stamps. Each
// clock reading also notes the log's size then, so senderLog(since) can read from where the log stood at `since`.
// The notes are kept per log file (its inode): a log made again after the app
// was removed starts with no notes, as the sizes noted for the old log point
// past the new one's end.
const MAC_AGENT =
  't=gui/$(id -u)/com.jetkvm.os-services; p="$HOME/Library/LaunchAgents/com.jetkvm.os-services.plist"; ' +
  'log="$HOME/Library/Logs/IP-KVM OS Services/service.log"; ' +
  'marks="${TMPDIR:-/tmp}/osvc-e2e-log-marks-$(stat -f %i "$log" 2>/dev/null || echo none)"; ';

// pbcopy and pbpaste read the text's encoding from the locale.
const MAC_UTF8 = "LANG=en_US.UTF-8 ";

// macOS date has no %N.
const MAC_NOW_MS = `$(perl -MTime::HiRes=time -e 'printf "%d", time * 1000')`;

const MAC_MARK = `now=${MAC_NOW_MS}; echo "$now $(stat -f %z "$log" 2>/dev/null || echo 0)" >> "$marks"; echo $now; `;

// pid of the running agent, empty when it does not run.
const MAC_PID = `launchctl print $t 2>/dev/null | sed -n 's/^\tpid = //p' | head -1`;

// The agent's state, as systemctl show prints a unit's.
const MAC_UNIT_PROPERTIES =
  MAC_AGENT +
  `pid=$(${MAC_PID}); ` +
  'if launchctl print $t >/dev/null 2>&1 || [ -f "$p" ]; then echo LoadState=loaded; else echo LoadState=not-found; fi; ' +
  'if [ -n "$pid" ]; then echo ActiveState=active; else echo ActiveState=inactive; fi; ' +
  'echo "MainPID=${pid:-0}"; echo "FragmentPath=$p"';

// stop unloads the agent and waits until launchd has removed it; start loads
// it, or starts it again when it is loaded and has exited.
const MAC_SENDER_ACTIONS =
  "stop() { launchctl bootout $t 2>/dev/null; i=0; " +
  "while launchctl print $t >/dev/null 2>&1 && [ $i -lt 300 ]; do sleep 0.1; i=$((i + 1)); done; }; " +
  'start() { if launchctl print $t >/dev/null 2>&1; then launchctl kickstart $t; else launchctl bootstrap gui/$(id -u) "$p"; fi; }; ';

// Lists a USB device as the sysfs loop below does (tools/e2e/os_services_usb_macos.py).
function macUsbLister(): string {
  return readFileSync(path.resolve(toolsDir(), "os_services_usb_macos.py"), "utf8");
}

export function toolsDir(): string {
  const thisDir = path.dirname(fileURLToPath(import.meta.url));
  return path.resolve(thisDir, "..", "..", "..", "..", "tools", "e2e");
}

function probeScript(): Buffer {
  return readFileSync(path.resolve(toolsDir(), "os_services_usb_probe.py"));
}

// The X11 clipboard through Gtk 3 (python3-gi), for hosts without
// wl-clipboard. "check" succeeds when it can reach the display; "paste"
// prints the text (exit 3 when there is none); "copy" and "clear" take the
// text from the file in argv[2], own the clipboard, write "ok" to argv[3],
// and then "copy" serves the text until another application takes the
// clipboard or 120 s pass.
const GTK_CLIPBOARD_PY = `
import os, sys, time
import gi
gi.require_version("Gdk", "3.0")
gi.require_version("Gtk", "3.0")
from gi.repository import Gdk, GLib, Gtk
if Gdk.Display.get_default() is None:
    sys.exit(1)
mode = sys.argv[1]
board = Gtk.Clipboard.get(Gdk.SELECTION_CLIPBOARD)
if mode == "check":
    sys.exit(0)
if mode == "paste":
    text = board.wait_for_text()
    if text is None:
        sys.exit(3)
    sys.stdout.buffer.write(text.encode("utf-8"))
    sys.exit(0)
with open(sys.argv[2], "rb") as source:
    text = source.read().decode("utf-8")
os.unlink(sys.argv[2])
board.set_text(text, -1)
owned_at = time.monotonic()
if mode == "clear":
    board.clear()
def ready():
    with open(sys.argv[3], "w") as answer:
        answer.write("ok")
    if mode == "clear":
        Gtk.main_quit()
    return False
def owner_changed(*_):
    if time.monotonic() - owned_at > 0.5:
        Gtk.main_quit()
board.connect("owner-change", owner_changed)
GLib.idle_add(ready)
GLib.timeout_add_seconds(120, Gtk.main_quit)
Gtk.main()
`;

export function quote(value: string): string {
  return `'${value.replace(/'/g, "'\\''")}'`;
}

function inSh(command: string): string {
  return `sh -c ${quote(command)}`;
}

// The GNOME settings of the wallpaper: its image for the light and the dark
// style, and how it is fitted to the screen.
const GNOME_WALLPAPER = ["picture-uri", "picture-uri-dark", "picture-options"].map(
  key => `org.gnome.desktop.background ${key}`,
);

function gnomeWallpaper(settings: string[]): string {
  return (
    USER_SESSION +
    GNOME_WALLPAPER.map((key, i) => `gsettings set ${key} ${quote(settings[i])}`).join(" && ")
  );
}

// An AppleScript run in System Events, one statement per line.
function systemEvents(lines: string[]): string {
  return [
    "osascript",
    ...['tell application "System Events"', ...lines, "end tell"].map(line => `-e ${quote(line)}`),
  ].join(" ");
}

function appleString(text: string): string {
  return `"${text.replace(/[\\"]/g, "\\$&")}"`;
}

// env: setsid and timeout run their first word as the program.
const GTK_CLIPBOARD = `env GDK_BACKEND=x11 python3 -c ${quote(GTK_CLIPBOARD_PY)}`;

// One SSH connection carries every short command, so the timings the latency
// spec takes are not dominated by SSH handshakes.
const SSH_SHARED = [
  "-o",
  "ControlMaster=auto",
  "-o",
  "ControlPath=/tmp/osvc-e2e-%C",
  "-o",
  "ControlPersist=120",
];

export interface UsbEndpoint {
  address: string;
  type: string;
  direction: string;
  maxPacketSize: number;
}

export interface UsbInterface {
  name: string;
  number: number;
  triple: string;
  endpointCount: number;
  driver: string | null;
  endpoints: UsbEndpoint[];
}

export interface UsbDevice {
  /** The sysfs name, the port path, such as 3-2. */
  name: string;
  id: string;
  busnum: number;
  devnum: number;
  speed: string;
  /** bcdUSB, as 2.10. */
  version: string;
  bcdDevice: string;
  manufacturer: string;
  product: string;
  serial: string;
  interfaces: UsbInterface[];
}

export interface ApprovalList {
  present: boolean;
  content: string;
}

export interface SenderLogEntry {
  /** The remote host's clock, ms since the epoch. */
  at: number;
  message: string;
}

export function normalizeFingerprint(text: string): string {
  return text.replace(/[-: ]/g, "").toUpperCase();
}

/** The remote host (JETKVM_REMOTE_HOST): the target PC the Mini's USB is plugged into. */
export class TargetHost {
  readonly login: string;

  constructor(login: string) {
    this.login = login;
  }

  static fromEnv(): TargetHost {
    const login = process.env.JETKVM_REMOTE_HOST;
    if (!login) throw new Error("JETKVM_REMOTE_HOST not set");
    return new TargetHost(login);
  }

  /**
   * Runs `command` with `sh` on the remote host, with `input` on its standard
   * input. The login shell only starts `sh`: in zsh, for one, a glob that
   * matches nothing is an error.
   */
  run(
    command: string,
    { input, timeoutMs = 30_000 }: { input?: string | Buffer; timeoutMs?: number } = {},
  ): Promise<CommandResult> {
    return new Promise((resolve, reject) => {
      const child = spawn(
        "ssh",
        [...SSH_OPTS.split(" "), ...SSH_SHARED, this.login, inSh(command)],
        {
          stdio: ["pipe", "pipe", "pipe"],
        },
      );
      const stdout: Buffer[] = [];
      const stderr: Buffer[] = [];
      const timer = setTimeout(() => {
        child.kill("SIGKILL");
        reject(new Error(`timed out after ${timeoutMs} ms on ${this.login}: ${command}`));
      }, timeoutMs);
      child.stdout.on("data", (chunk: Buffer) => stdout.push(chunk));
      child.stderr.on("data", (chunk: Buffer) => stderr.push(chunk));
      child.on("error", error => {
        clearTimeout(timer);
        reject(error);
      });
      child.on("close", code => {
        clearTimeout(timer);
        resolve({
          status: code ?? -1,
          stdout: Buffer.concat(stdout),
          stderr: Buffer.concat(stderr).toString("utf8"),
        });
      });
      child.stdin.on("error", () => undefined);
      child.stdin.end(input ?? "");
    });
  }

  /** Runs `command` and returns its standard output; fails on a non-zero exit. */
  async text(
    command: string,
    options: { input?: string | Buffer; timeoutMs?: number } = {},
  ): Promise<string> {
    const result = await this.run(command, options);
    if (result.status !== 0)
      throw new Error(
        `${this.login}: exit ${result.status}: ${command}\n${result.stderr.slice(0, 2000)}`,
      );
    return result.stdout.toString("utf8");
  }

  async succeeds(command: string): Promise<boolean> {
    return (await this.run(command)).status === 0;
  }

  /**
   * Starts a long-running command on its own SSH connection and hands each
   * line of its output to `onLine`. Killing the returned process closes the
   * connection; the command must write now and then so that it notices.
   */
  stream(command: string, onLine: (line: string) => void): ChildProcess {
    const child = spawn("ssh", [...SSH_OPTS.split(" "), this.login, inSh(command)], {
      stdio: ["ignore", "pipe", "ignore"],
    });
    let pending = "";
    child.stdout?.on("data", (chunk: Buffer) => {
      pending += chunk.toString("utf8");
      const lines = pending.split("\n");
      pending = lines.pop() ?? "";
      for (const line of lines) onLine(line);
    });
    return child;
  }

  private macosCache?: Promise<boolean>;

  /** Whether the remote host runs macOS rather than Linux. */
  isMacos(): Promise<boolean> {
    this.macosCache ??= this.text("uname -s").then(name => name.trim() === "Darwin");
    return this.macosCache;
  }

  /** The SHA-256 of a file on the remote host, as lowercase hex. */
  async sha256(file: string): Promise<string> {
    const tool = (await this.isMacos()) ? "shasum -a 256" : "sha256sum";
    return (await this.text(`${tool} -- ${quote(file)}`)).split(/\s+/)[0];
  }

  /** The remote host's clock, ms since the epoch. */
  async now(): Promise<number> {
    const command = (await this.isMacos()) ? MAC_AGENT + MAC_MARK : `echo ${NOW_MS}`;
    return Number((await this.text(command)).trim());
  }

  // --- The sender's unit ---

  async unitProperties(): Promise<Record<string, string>> {
    const text = await this.text(
      (await this.isMacos())
        ? MAC_UNIT_PROPERTIES
        : USER_SESSION +
            `systemctl --user show -p LoadState,ActiveState,FragmentPath,ControlGroup,MainPID ${SENDER_UNIT}`,
    );
    return Object.fromEntries(
      text
        .trim()
        .split("\n")
        .map(line => [line.slice(0, line.indexOf("=")), line.slice(line.indexOf("=") + 1)]),
    );
  }

  async senderActive(): Promise<boolean> {
    return (await this.unitProperties()).ActiveState === "active";
  }

  /**
   * Starts, stops or restarts the sender; returns the remote host's clock (ms)
   * just before, for reading the sender's log from then on.
   */
  async sender(action: "start" | "stop" | "restart"): Promise<number> {
    const macActions = { start: "start", stop: "stop", restart: "stop; start" };
    const text = await this.text(
      (await this.isMacos())
        ? MAC_AGENT + MAC_MARK + MAC_SENDER_ACTIONS + macActions[action]
        : // reset-failed clears the start rate limit the specs' restarts reach.
          USER_SESSION +
            `echo ${NOW_MS}; systemctl --user reset-failed ${SENDER_UNIT} 2>/dev/null; ` +
            `systemctl --user ${action} ${SENDER_UNIT}`,
      { timeoutMs: 45_000 },
    );
    return Number(text.trim().split("\n")[0]);
  }

  /** What the sender wrote to its log since `since` (remote clock, ms). */
  async senderLog(since: number): Promise<SenderLogEntry[]> {
    if (await this.isMacos()) return this.macSenderLog(since);
    const text = await this.text(
      `uid=$(id -u); sudo -n journalctl --no-pager -o json --output-fields=MESSAGE ` +
        `--since @${Math.floor(since / 1000) - 1} _SYSTEMD_USER_UNIT=${SENDER_UNIT} _UID=$uid`,
    );
    return text
      .split("\n")
      .filter(line => line.trim() !== "")
      .map(line => {
        const entry = JSON.parse(line) as { __REALTIME_TIMESTAMP: string; MESSAGE?: unknown };
        // journald keeps a message that is not UTF-8 as an array of bytes.
        const bytes = Array.isArray(entry.MESSAGE) ? (entry.MESSAGE as number[]) : [];
        const message =
          typeof entry.MESSAGE === "string" ? entry.MESSAGE : Buffer.from(bytes).toString("utf8");
        return { at: Number(entry.__REALTIME_TIMESTAMP) / 1000, message };
      })
      .filter(entry => entry.at >= since);
  }

  /**
   * The macOS log from where it stood at `since`. The log has no time
   * stamps: a line carries the time of the last clock reading before it
   * was written, and at least `since`.
   */
  private async macSenderLog(since: number): Promise<SenderLogEntry[]> {
    const result = await this.run(
      MAC_AGENT +
        `o=$(awk -v s=${since} '$1 <= s && $2 > o { o = $2 } END { print o + 0 }' "$marks" 2>/dev/null); ` +
        `o=\${o:-0}; echo $o; awk -v o=$o '$2 >= o' "$marks" 2>/dev/null; ` +
        'echo ---; tail -c +$((o + 1)) "$log"',
    );
    if (result.status !== 0)
      throw new Error(`${this.login}: reading the sender's log failed: ${result.stderr}`);
    const split = result.stdout.indexOf("\n---\n");
    const [start, ...markLines] = result.stdout.subarray(0, split).toString("utf8").split("\n");
    const marks = markLines.map(line => line.split(" ").map(Number));
    const entries: SenderLogEntry[] = [];
    let offset = Number(start);
    for (const line of result.stdout
      .subarray(split + 5)
      .toString("utf8")
      .split("\n")) {
      const at = Math.max(
        since,
        ...marks.filter(([, size]) => size <= offset).map(([time]) => time),
      );
      if (line !== "") entries.push({ at, message: line });
      offset += Buffer.byteLength(line, "utf8") + 1;
    }
    return entries;
  }

  // --- The sender's approved KVM list ---

  // The sender keeps the list in ipkvm-os-services/approved-kvms under the
  // user's local configuration directory, one fingerprint per line. The
  // unit's environment decides XDG_CONFIG_HOME.
  private async listPath(): Promise<string> {
    if (await this.isMacos())
      return 'f="$HOME/Library/Application Support/ipkvm-os-services/approved-kvms"; ';
    return (
      USER_SESSION +
      "cfg=$(systemctl --user show-environment | sed -n 's/^XDG_CONFIG_HOME=//p'); " +
      'f="${cfg:-$HOME/.config}/ipkvm-os-services/approved-kvms"; '
    );
  }

  async readApprovalList(): Promise<ApprovalList> {
    const text = await this.text(
      (await this.listPath()) +
        'if [ -e "$f" ]; then echo present; base64 < "$f"; else echo absent; fi',
    );
    const [state, ...encoded] = text.trim().split("\n");
    return {
      present: state === "present",
      content: Buffer.from(encoded.join(""), "base64").toString("utf8"),
    };
  }

  /** Replaces the list as the sender saves it: a new file renamed over the old one. */
  async writeApprovalList(list: ApprovalList): Promise<void> {
    if (!list.present) {
      await this.text((await this.listPath()) + 'rm -f "$f"');
      return;
    }
    await this.text(
      (await this.listPath()) +
        'mkdir -p "$(dirname "$f")" && cat > "$f.e2e$$" && mv -f "$f.e2e$$" "$f"',
      { input: list.content },
    );
  }

  async approve(fingerprint: string): Promise<void> {
    const list = await this.readApprovalList();
    const lines = list.content.split("\n").filter(line => line.trim() !== "");
    if (lines.some(line => normalizeFingerprint(line) === normalizeFingerprint(fingerprint)))
      return;
    await this.writeApprovalList({
      present: true,
      content: [...lines, fingerprint].map(line => `${line}\n`).join(""),
    });
  }

  // --- The desktop clipboard ---
  //
  // wl-clipboard in the user's Wayland session, or, where it is not
  // installed, Gtk 3 on the session's X11 display. GNOME keeps the two
  // clipboards in step, and the sender reads whichever its clipboard library
  // reaches. On macOS, pbcopy and pbpaste with the general pasteboard.

  async waylandDisplay(): Promise<string> {
    return (await this.text(WAYLAND_SESSION + 'printf %s "${WAYLAND_DISPLAY:-}"')).trim();
  }

  private clipboardToolCache?: Promise<ClipboardTool | null>;

  /** How this host's desktop clipboard is read and written, if it can be. */
  clipboardTool(): Promise<ClipboardTool | null> {
    this.clipboardToolCache ??= (async () => {
      if (await this.isMacos()) return "pbcopy";
      if (
        (await this.succeeds("command -v wl-copy >/dev/null && command -v wl-paste >/dev/null")) &&
        (await this.waylandDisplay()) !== ""
      )
        return "wl-clipboard";
      if (await this.succeeds(WAYLAND_SESSION + `${GTK_CLIPBOARD} check`)) return "gtk-x11";
      return null;
    })();
    return this.clipboardToolCache;
  }

  async hasDesktopClipboard(): Promise<boolean> {
    return (await this.clipboardTool()) !== null;
  }

  private async requireClipboardTool(): Promise<ClipboardTool> {
    const tool = await this.clipboardTool();
    if (!tool) throw new Error(`${this.login} cannot read or write its desktop clipboard`);
    return tool;
  }

  /** Sets the desktop clipboard to `text`, as an application copying text does. */
  async copy(text: string): Promise<void> {
    await this.ownClipboard("copy", text);
  }

  private async ownClipboard(mode: "copy" | "clear", text: string): Promise<void> {
    const tool = await this.requireClipboardTool();
    if (tool === "pbcopy") {
      await this.text(`${MAC_UTF8}pbcopy`, { input: Buffer.from(text, "utf8") });
      return;
    }
    if (tool === "wl-clipboard") {
      // wl-copy forks a process that serves the clipboard until something
      // else takes it; it reads a file, not the SSH pipe, so the command
      // returns.
      await this.text(
        WAYLAND_SESSION +
          't=$(mktemp) && cat > "$t" && ' +
          'wl-copy --type "text/plain;charset=utf-8" < "$t" >/dev/null 2>&1; s=$?; rm -f "$t"; exit $s',
        { input: Buffer.from(text, "utf8") },
      );
      return;
    }
    // The helper owns the X11 clipboard from its own session and says "ok"
    // through the second file once it does.
    await this.text(
      WAYLAND_SESSION +
        't=$(mktemp) && r=$(mktemp) && cat > "$t" && ' +
        `setsid -f ${GTK_CLIPBOARD} ${mode} "$t" "$r" </dev/null >/dev/null 2>&1; ` +
        'i=0; while [ ! -s "$r" ] && [ $i -lt 100 ]; do sleep 0.1; i=$((i + 1)); done; ' +
        'a=$(cat "$r"); rm -f "$r"; [ "$a" = ok ] || { echo "clipboard helper: ${a:-no answer}" >&2; exit 1; }',
      { input: Buffer.from(text, "utf8") },
    );
  }

  /** The desktop clipboard's text, as an application pasting reads it. */
  async paste(): Promise<string> {
    const tool = await this.requireClipboardTool();
    const commands: Record<ClipboardTool, string> = {
      "wl-clipboard": WAYLAND_SESSION + "timeout 10 wl-paste --no-newline",
      "gtk-x11": WAYLAND_SESSION + `timeout 10 ${GTK_CLIPBOARD} paste`,
      pbcopy: `${MAC_UTF8}pbpaste`,
    };
    const result = await this.run(commands[tool]);
    if (result.status !== 0)
      throw new Error(`reading the clipboard failed (${result.status}): ${result.stderr}`);
    return result.stdout.toString("utf8");
  }

  // --- The desktop wallpaper ---
  //
  // GNOME's background settings, or on macOS the picture of every desktop
  // through System Events. A new wallpaper goes to a file in the remote
  // temporary directory.

  /** Whether the desktop shows its lock screen, which hides the wallpaper:
   * macOS's session dictionary, or GNOME's screen shield. */
  async screenLocked(): Promise<boolean> {
    const text = await this.text(
      (await this.isMacos())
        ? `osascript -l JavaScript -e ${quote(
            'ObjC.import("CoreGraphics"); ObjC.deepUnwrap(ObjC.castRefToObject(' +
              "$.CGSessionCopyCurrentDictionary())).CGSSessionScreenIsLocked === true",
          )}`
        : USER_SESSION +
            "gdbus call --session --dest org.gnome.ScreenSaver " +
            "--object-path /org/gnome/ScreenSaver --method org.gnome.ScreenSaver.GetActive",
    );
    return text.includes("true");
  }

  /** The wallpaper settings, as restoreWallpaper takes them back. */
  async wallpaper(): Promise<string[]> {
    const text = await this.text(
      (await this.isMacos())
        ? systemEvents([
            "set AppleScript's text item delimiters to linefeed",
            "(picture of every desktop) as text",
          ])
        : USER_SESSION + GNOME_WALLPAPER.map(key => `gsettings get ${key}`).join("; "),
    );
    return text.trim().split("\n");
  }

  /** Shows the PNG `image` as the wallpaper, filling the screen. */
  async showWallpaper(image: Buffer): Promise<void> {
    const file = await this.text(
      'f="${TMPDIR:-/tmp}/jetkvm-e2e-wallpaper.png"; cat > "$f"; printf %s "$f"',
      { input: image },
    );
    const uri = `'file://${file}'`;
    await this.text(
      (await this.isMacos())
        ? systemEvents([`tell every desktop to set picture to ${appleString(file)}`])
        : gnomeWallpaper([uri, uri, "'zoom'"]),
    );
  }

  async restoreWallpaper(settings: string[]): Promise<void> {
    await this.text(
      (await this.isMacos())
        ? systemEvents(
            settings.map((file, i) => `set picture of desktop ${i + 1} to ${appleString(file)}`),
          )
        : gnomeWallpaper(settings),
    );
  }

  // --- USB, as the remote host's kernel sees it ---

  /** One line per device, interface and endpoint with this vendor:product ID. */
  private async usbListing(id: string): Promise<string> {
    if (await this.isMacos())
      return this.text(
        't=$(mktemp -d) && cat > "$t/os_services_usb_probe.py" && ' +
          `python3 -c ${quote(macUsbLister())} "$t" ${quote(id.toLowerCase())}; s=$?; rm -rf "$t"; exit $s`,
        { input: probeScript() },
      );
    return this.text(
      'r() { cat "$1" 2>/dev/null | tr -d "\\t\\n"; }; ' +
        "for d in /sys/bus/usb/devices/*; do " +
        '[ -f "$d/idVendor" ] || continue; ' +
        `[ "$(r "$d/idVendor"):$(r "$d/idProduct")" = ${quote(id.toLowerCase())} ] || continue; ` +
        "n=${d##*/}; " +
        'printf "device\\t%s\\t%s\\t%s\\t%s\\t%s\\t%s\\t%s\\t%s\\t%s\\n" "$n" "$(r "$d/busnum")" "$(r "$d/devnum")" ' +
        '"$(r "$d/speed")" "$(r "$d/version" | tr -d " ")" "$(r "$d/bcdDevice")" ' +
        '"$(r "$d/manufacturer")" "$(r "$d/product")" "$(r "$d/serial")"; ' +
        'for i in "$d/$n":*; do ' +
        '[ -f "$i/bInterfaceClass" ] || continue; ' +
        'drv=$(readlink "$i/driver" 2>/dev/null); drv=${drv##*/}; ' +
        'printf "interface\\t%s\\t%s\\t%s\\t%s\\t%s\\n" "${i##*/}" "$(r "$i/bInterfaceNumber")" ' +
        '"$(r "$i/bInterfaceClass")$(r "$i/bInterfaceSubClass")$(r "$i/bInterfaceProtocol")" ' +
        '"$(r "$i/bNumEndpoints")" "${drv:--}"; ' +
        'for e in "$i"/ep_*; do [ -d "$e" ] || continue; ' +
        'printf "endpoint\\t%s\\t%s\\t%s\\t%s\\n" "$(r "$e/bEndpointAddress")" "$(r "$e/type")" ' +
        '"$(r "$e/direction")" "$(r "$e/wMaxPacketSize")"; ' +
        "done; done; done",
    );
  }

  /** Every USB device with this vendor:product ID, its interfaces and endpoints. */
  async usbDevices(id: string): Promise<UsbDevice[]> {
    const text = await this.usbListing(id);
    const devices: UsbDevice[] = [];
    for (const line of text.split("\n").filter(Boolean)) {
      const [kind, ...fields] = line.split("\t");
      if (kind === "device") {
        devices.push({
          name: fields[0],
          id: id.toLowerCase(),
          busnum: Number(fields[1]),
          devnum: Number(fields[2]),
          speed: fields[3],
          version: fields[4],
          bcdDevice: fields[5],
          manufacturer: fields[6],
          product: fields[7],
          serial: fields[8],
          interfaces: [],
        });
      } else if (kind === "interface") {
        devices.at(-1)!.interfaces.push({
          name: fields[0],
          number: Number.parseInt(fields[1], 16),
          triple: fields[2].toLowerCase(),
          endpointCount: Number.parseInt(fields[3], 16),
          driver: fields[4] === "-" ? null : fields[4],
          endpoints: [],
        });
      } else if (kind === "endpoint") {
        devices
          .at(-1)!
          .interfaces.at(-1)!
          .endpoints.push({
            address: fields[0],
            type: fields[1],
            direction: fields[2],
            maxPacketSize: Number.parseInt(fields[3], 16),
          });
      }
    }
    return devices;
  }

  // --- The transport probe ---
}

/** What identifies the Mini's USB device to the host; bus and device numbers left out. */
export interface UsbIdentity {
  id: string;
  version: string;
  bcdDevice: string;
  manufacturer: string;
  product: string;
  serial: string;
  interfaces: { number: number; triple: string; endpoints: string[] }[];
}

export function usbIdentity(device: UsbDevice): UsbIdentity {
  return {
    id: device.id,
    version: device.version,
    bcdDevice: device.bcdDevice,
    manufacturer: device.manufacturer,
    product: device.product,
    serial: device.serial,
    interfaces: device.interfaces
      .map(i => ({
        number: i.number,
        triple: i.triple,
        endpoints: i.endpoints.map(e => `${e.address} ${e.type} ${e.direction} ${e.maxPacketSize}`),
      }))
      .sort((a, b) => a.number - b.number),
  };
}

// --- Preconditions and restoring the bench ----------------------------------

/** Skips unless the remote host runs the sender as a user unit and has passwordless sudo. */
export async function requireSenderHost(target: TargetHost): Promise<Record<string, string>> {
  const unit = await target.unitProperties();
  test.skip(
    unit.LoadState !== "loaded",
    `${target.login} has no ${SENDER_UNIT} user unit or launchd agent (LoadState ${unit.LoadState}): ` +
      "install the OS Services AppImage or app there and open it once",
  );
  test.skip(
    !(await target.isMacos()) && !(await target.succeeds("sudo -n true")),
    `${target.login} needs passwordless sudo (for journalctl, lsusb -v and the USB probe)`,
  );
  return unit;
}

/** What the tests change, as it was before they ran. */
export interface OsServicesSnapshot {
  settings: OsServicesSettings;
  approvals: ApprovalList;
  senderActive: boolean;
  fingerprint: string;
}

/**
 * Skips unless the remote host runs the sender as a systemd user unit with
 * passwordless sudo, then records what the tests change.
 */
export async function prepareOsServices(
  page: Page,
  target: TargetHost,
): Promise<OsServicesSnapshot> {
  const answer = await rpcAnswer(page, "getOsServicesSettings");
  expect(answer.error, "getOsServicesSettings").toBeUndefined();
  const unit = await requireSenderHost(target);
  const state = await osServicesState(page);
  expect(state.osServices, "getVideoState carries osServices").toBeDefined();
  expect(state.osServices.fingerprint, "the Mini's fingerprint").toMatch(FINGERPRINT);
  return {
    settings: answer.result as OsServicesSettings,
    approvals: await target.readApprovalList(),
    senderActive: unit.ActiveState === "active",
    fingerprint: state.osServices.fingerprint,
  };
}

/**
 * The Mini is approved, the sender runs and is connected, and clipboard
 * sharing is on, so the sender offers the clipboard.
 */
export async function connectSender(
  page: Page,
  target: TargetHost,
  fingerprint: string,
): Promise<OsServicesState> {
  await target.approve(fingerprint);
  if (!(await target.senderActive())) await target.sender("start");
  if (!(await bridgeSettings(page)).clipboard_enabled)
    await setBridgeSettings(page, { clipboard_enabled: true });
  // A sender waiting for approval reads the list every second; a sender in
  // a session already stays in it.
  return waitForState(
    page,
    "the sender connects and offers the clipboard",
    connectedWithClipboard,
    45_000,
  );
}

/**
 * The state once the sender has registered service video or the device
 * reports a video fault. A sender registers video seconds after its session
 * starts, so the state connectSender returns can show none yet. After `timeout` it is the last state: a sender without video.
 */
export async function videoRegistrationState(
  page: Page,
  timeout = 15_000,
): Promise<OsServicesState> {
  return waitForState(
    page,
    "the sender registers service video",
    state => state.osServices.videoRegistered || state.osServices.videoError !== undefined,
    timeout,
  ).catch(() => osServicesState(page));
}

/**
 * Turns service video off and on again when it is on. Every switch to
 * service video counts against the retry budget, also one after a sender
 * restart, and its block stays across sessions until video is turned off and
 * on (the protocol's Switch to service video section). The specs before
 * restart the sender with video on, so a spec that needs video clears the
 * budget first.
 */
export async function clearVideoRetryBudget(page: Page): Promise<void> {
  if (!(await bridgeSettings(page)).video_enabled) return;
  await setBridgeSettings(page, { video_enabled: false });
  await setBridgeSettings(page, { video_enabled: true });
}

/** Puts back the settings, the approved list and the sender's state. */
export async function restoreOsServices(
  page: Page,
  target: TargetHost,
  snapshot: OsServicesSnapshot,
): Promise<void> {
  const errors: unknown[] = [];
  try {
    await ensureRpcReady(page, { timeoutMs: 60_000 });
    await setBridgeSettings(page, settingsRequest(snapshot.settings));
    expect(await bridgeSettings(page), "OS Services settings restored").toEqual(snapshot.settings);
  } catch (error) {
    errors.push(error);
  }
  try {
    await target.writeApprovalList(snapshot.approvals);
    expect(await target.readApprovalList(), "approved KVM list restored").toEqual(
      snapshot.approvals,
    );
    // The sender reads the list when a session starts: restart it so that it
    // acts on the restored list, or leave it stopped as it was.
    await target.sender(snapshot.senderActive ? "restart" : "stop");
    const approved = snapshot.approvals.content
      .split("\n")
      .some(line => normalizeFingerprint(line) === normalizeFingerprint(snapshot.fingerprint));
    if (snapshot.senderActive && approved)
      await waitForState(page, "the sender is back", connectedState, 45_000);
  } catch (error) {
    errors.push(error);
  }
  if (errors.length) throw new AggregateError(errors, "OS Services restoration failed");
}

/** The OS Services settings of a bench with OS Services on: the firmware's defaults. */
export const BENCH_BRIDGE_SETTINGS: OsServicesSettings = {
  terminal_enabled: true,
  files_enabled: true,
  capture_display: "",
  video_enabled: true,
  resolution: "2560x1440",
  display_mode: "extend",
  clipboard_enabled: true,
};

/**
 * `list` with the Mini's fingerprint `from` replaced by `to`. A factory reset
 * gives the Mini a new key; the Mini stays as approved as it was.
 */
export function carryApproval(list: ApprovalList, from: string, to: string): ApprovalList {
  const lines = list.content.split("\n").filter(line => line.trim() !== "");
  const carried = lines.map(line =>
    normalizeFingerprint(line) === normalizeFingerprint(from) ? to : line,
  );
  return { present: list.present, content: carried.map(line => `${line}\n`).join("") };
}

/** Waits until HDMI capture has one valid mode (one videoEpoch) for `holdMs`. */
async function waitForHeldHdmiMode(holdMs: number): Promise<void> {
  let held: { epoch?: number; since: number } | undefined;
  await pollUntil(
    async () => {
      const response = await fetch(`${process.env.JETKVM_URL}/webrtc/stats`, {
        signal: AbortSignal.timeout(10_000),
      });
      const stats = (await response.json()) as { modeValid?: boolean; videoEpoch?: number };
      const now = Date.now();
      if (!stats.modeValid) held = undefined;
      else if (held?.epoch !== stats.videoEpoch) held = { epoch: stats.videoEpoch, since: now };
      return (held && now - held.since >= holdMs) || undefined;
    },
    {
      timeout: 45_000,
      interval: 500,
      message: () => `live HDMI video that keeps its mode for ${holdMs} ms`,
    },
  );
}

/**
 * For a spec that asserts HDMI capture: guest video is off for the spec and
 * back on after it. A sender that is reconnecting (after a USB re-enumeration)
 * would otherwise take over the display during the spec.
 */
export function useHdmiCapture(): void {
  let turnedOff = false;
  test.beforeAll(async ({ browser }) => {
    test.setTimeout(120_000);
    await withRpcPage(browser, async page => {
      const answer = await rpcAnswer(page, "getOsServicesSettings");
      if (answer.error?.code !== -32601) {
        expect(answer.error, "getOsServicesSettings").toBeUndefined();
        if ((answer.result as OsServicesSettings).video_enabled) {
          await setBridgeSettings(page, { video_enabled: false });
          turnedOff = true;
          await waitForState(page, "HDMI capture", state => state.source === "physical", 45_000);
        }
      }
      // The receiver gets the physical EDID back and the host sets its mode
      // again before capture runs. The host can still be setting a mode for
      // the guest display (turned on by the spec before) and then set
      // another, so after a handover the mode must hold for a few seconds.
      // The open session keeps "Hide display when idle" from lowering HPD.
      await waitForHeldHdmiMode(turnedOff ? 5_000 : 0);
    });
  });
  test.afterAll(async ({ browser }) => {
    if (!turnedOff) return;
    await withRpcPage(browser, page => setBridgeSettings(page, { video_enabled: true }));
  });
}
