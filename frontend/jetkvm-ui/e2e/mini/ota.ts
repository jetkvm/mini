import * as http from "http";
import * as https from "https";
import * as fs from "fs";
import * as crypto from "crypto";
import * as net from "net";
import * as os from "os";
import * as path from "path";
import { execSync } from "child_process";
import { expect, test } from "@playwright/test";
import type { Browser, Page } from "@playwright/test";
import {
  callJsonRpc,
  ensureRpcReady,
  getDeviceHost,
  rawJsonRpc,
  rebootAndReconnect,
  waitForDeviceDown,
  waitForDeviceReady,
  waitForWebRTCReady,
  withRpcPage,
} from "../helpers/device";
import { ensureLocalAuthMode } from "../helpers/auth";
import { serverKeyAndCertificate } from "./mock-cloud";
import {
  signDetached,
  testReleaseKey,
} from "../../../../tools/combined_load/runner/release_signature.mjs";

/**
 * Which release component the mock serves.
 *
 * "app": the JetKVM's userspace binary (jetkvm_app), the upstream default.
 * "system": a whole firmware image. The Mini installs its firmware as the
 * system component and sends `sku=` on every request; images are picked
 * per SKU from a directory laid out like an R2 release
 * (`skus/<sku>/jetkvm-mini.bin`, what `tools/release_r2.sh --dry-run` stages).
 * A stable version is offered with a signature by the test release key
 * (`systemSigUrl`), as a release has one by the JetKVM release key, and the
 * mock serves that key at /keyserver/<fingerprint>, where a mock cloud build
 * asks for it. A prerelease is offered without a signature.
 */
export type OtaComponent = "app" | "system";

export interface MockRelease {
  version: string;
  /** System component: release directory with skus/<sku>/jetkvm-mini.bin. */
  stageDir?: string;
  /** App component, or a system image served for every SKU. */
  binaryPath?: string;
}

export interface MockUpdateServerConfig extends MockRelease {
  signaturePath?: string;
  port?: number;
  component?: OtaComponent;
  /**
   * System component only: what a device that has not opted into prereleases
   * receives while `version` is a prerelease. Without it such a request gets
   * 404, like the cloud does for a channel with no release.
   */
  stable?: MockRelease;
  /** Serve `version` regardless of the device's channel. */
  ignoreChannel?: boolean;
  /** Answer every /releases request with this status and a JSON error body. */
  failReleasesWith?: number;
  /**
   * Serve over HTTPS, with a certificate for this machine's address signed by
   * the mock cloud's test CA (mock-cloud.ts; needs the openssl command). Mock
   * cloud builds trust that CA. Over HTTPS the device takes its "ota-download"
   * TLS ticket for the check and for the download, as it does against the
   * real update server; over HTTP it takes none.
   */
  secure?: boolean;
}

export interface MockRequest {
  path: string;
  query: Record<string, string>;
  headers: Record<string, string | undefined>;
  status: number;
  servedVersion?: string;
  servedSku?: string;
  at: number;
}

export interface MockUpdateServer {
  url: string;
  port: number;
  close: () => Promise<void>;
  enableSignature: (sigPath: string) => void;
  disableSignature: () => void;
  /** Every request the device made, oldest first. */
  requests: MockRequest[];
  releasesRequests: () => MockRequest[];
}

export const PRODUCTION_API_URL = "https://api.jetkvm.com";

const MINI_IMAGE_NAME = "jetkvm-mini.bin";

export async function createMockUpdateServer(
  config: MockUpdateServerConfig,
): Promise<MockUpdateServer> {
  const component: OtaComponent = config.component ?? "app";
  const { version } = config;
  const port = config.port ?? 0;
  const localIP = getLocalNetworkIP();
  const timestamp = Date.now();
  const requests: MockRequest[] = [];
  const hashes = new Map<string, string>();
  let signaturePath: string | undefined = config.signaturePath;
  let baseUrl = "";

  const hashOf = async (file: string): Promise<string> => {
    let hash = hashes.get(file);
    if (!hash) {
      hash = await computeFileHash(file);
      hashes.set(file, hash);
    }
    return hash;
  };

  if (component === "app" && !config.binaryPath) {
    throw new Error("app component needs binaryPath");
  }
  if (config.binaryPath && !fs.existsSync(config.binaryPath)) {
    throw new Error(`Binary not found: ${config.binaryPath}`);
  }
  if (config.stageDir && !fs.existsSync(config.stageDir)) {
    throw new Error(`Stage directory not found: ${config.stageDir}`);
  }
  if (component === "system" && !config.binaryPath && !config.stageDir) {
    throw new Error("system component needs stageDir or binaryPath");
  }

  const binaryHash = config.binaryPath ? await hashOf(config.binaryPath) : "";

  const record = (
    req: http.IncomingMessage,
    url: URL,
    status: number,
    served?: { version: string; sku?: string },
  ) => {
    requests.push({
      path: url.pathname,
      query: Object.fromEntries(url.searchParams),
      headers: {
        "x-device-sku": req.headers["x-device-sku"] as string | undefined,
        "x-app-version": req.headers["x-app-version"] as string | undefined,
        "user-agent": req.headers["user-agent"],
      },
      status,
      servedVersion: served?.version,
      servedSku: served?.sku,
      at: Date.now(),
    });
  };

  const sendJson = (res: http.ServerResponse, status: number, payload: unknown) => {
    const body = JSON.stringify(payload);
    res.writeHead(status, {
      "Content-Type": "application/json",
      "Content-Length": Buffer.byteLength(body),
    });
    res.end(body);
  };

  const streamFile = (filePath: string, res: http.ServerResponse) => {
    const stat = fs.statSync(filePath);
    res.writeHead(200, {
      "Content-Length": stat.size,
      "Content-Type": "application/octet-stream",
    });
    fs.createReadStream(filePath).pipe(res);
  };

  // ---- app component (upstream behaviour) ----

  const handleAppReleases = (req: http.IncomingMessage, url: URL, res: http.ServerResponse) => {
    const query = Object.fromEntries(url.searchParams);
    const isCustomVersion = "appVersion" in query || "systemVersion" in query;
    const appVersion = isCustomVersion ? (query.appVersion ?? version) : version;

    const response: Record<string, unknown> = {
      appVersion,
      appUrl: `${baseUrl}/app/${version}/jetkvm_app`,
      appHash: binaryHash,
      appCachedAt: timestamp,
      appMaxSatisfying: "*",
      systemVersion: "0.0.1",
      systemUrl: "",
      systemHash: "",
      systemCachedAt: timestamp,
      systemMaxSatisfying: "*",
    };
    if (signaturePath) {
      response.appSigUrl = `${baseUrl}/app/${version}/jetkvm_app.sig`;
    }
    record(req, url, 200, { version: appVersion });
    sendJson(res, 200, response);
  };

  // ---- system component (Mini) ----

  const imageFor = (release: MockRelease, sku: string): string | null => {
    if (release.stageDir) {
      const file = path.join(release.stageDir, "skus", sku, MINI_IMAGE_NAME);
      return fs.existsSync(file) ? file : null;
    }
    return release.binaryPath ?? null;
  };

  const handleSystemReleases = async (
    req: http.IncomingMessage,
    url: URL,
    res: http.ServerResponse,
  ) => {
    const query = Object.fromEntries(url.searchParams);
    const sku = query.sku;
    if (!sku) {
      record(req, url, 400);
      sendJson(res, 400, { name: "BadRequestError", message: "SKU is required" });
      return;
    }
    const wantsPrerelease = query.prerelease === "true";
    const isPrerelease = version.includes("-");
    let release: MockRelease = config;
    if (query.systemVersion) {
      // An explicit version is served whatever the channel, as the cloud
      // and upstream's mock do for custom-version updates.
      const known = [config, config.stable].find(r => r?.version === query.systemVersion);
      if (!known) {
        record(req, url, 404);
        sendJson(res, 404, { name: "NotFoundError", message: "Release not found" });
        return;
      }
      release = known;
    } else if (isPrerelease && !wantsPrerelease && !config.ignoreChannel) {
      if (!config.stable) {
        record(req, url, 404);
        sendJson(res, 404, { name: "NotFoundError", message: "No mini release is rolled out yet" });
        return;
      }
      release = config.stable;
    }
    const file = imageFor(release, sku);
    if (!file) {
      record(req, url, 404);
      sendJson(res, 404, { name: "NotFoundError", message: `No image for ${sku}` });
      return;
    }
    const hash = await hashOf(file);
    const systemUrl = `${baseUrl}/mini/${release.version}/skus/${sku}/${MINI_IMAGE_NAME}`;
    record(req, url, 200, { version: release.version, sku });
    sendJson(res, 200, {
      systemVersion: release.version,
      systemUrl,
      ...(release.version.includes("-") ? {} : { systemSigUrl: `${systemUrl}.sig` }),
      systemHash: hash,
    });
  };

  const releaseKey = testReleaseKey();
  const handleSystemImage = (req: http.IncomingMessage, url: URL, res: http.ServerResponse) => {
    if (url.pathname.startsWith("/keyserver/")) {
      const known = url.pathname === `/keyserver/${releaseKey.rootFingerprint}`;
      record(req, url, known ? 200 : 404);
      res.writeHead(known ? 200 : 404, { "Content-Type": "application/pgp-keys" });
      res.end(known ? releaseKey.publicKey : undefined);
      return true;
    }
    const match = url.pathname.match(/^\/mini\/([^/]+)\/skus\/([^/]+)\/jetkvm-mini\.bin(\.sig)?$/);
    if (!match) return false;
    const [, wanted, sku, signature] = match;
    const releases = [config, config.stable].filter((r): r is MockRelease => !!r);
    const release = releases.find(r => r.version === wanted);
    const file = release ? imageFor(release, sku) : null;
    if (!file) {
      record(req, url, 404);
      sendJson(res, 404, { message: "Not found" });
      return true;
    }
    record(req, url, 200, { version: wanted, sku });
    if (signature) {
      const body = signDetached(fs.readFileSync(file), releaseKey);
      res.writeHead(200, {
        "Content-Length": body.length,
        "Content-Type": "application/pgp-signature",
      });
      res.end(body);
      return true;
    }
    streamFile(file, res);
    return true;
  };

  const handler = (req: http.IncomingMessage, res: http.ServerResponse) => {
    const url = new URL(req.url!, "http://localhost");
    if (url.pathname === "/releases") {
      if (config.failReleasesWith) {
        record(req, url, config.failReleasesWith);
        sendJson(res, config.failReleasesWith, { message: "mock failure" });
        return;
      }
      if (component === "system") {
        handleSystemReleases(req, url, res).catch(err => {
          record(req, url, 500);
          sendJson(res, 500, { message: String(err) });
        });
      } else {
        handleAppReleases(req, url, res);
      }
      return;
    }
    if (component === "app") {
      if (url.pathname === `/app/${version}/jetkvm_app` && config.binaryPath) {
        record(req, url, 200, { version });
        streamFile(config.binaryPath, res);
        return;
      }
      if (url.pathname === `/app/${version}/jetkvm_app.sig` && signaturePath) {
        record(req, url, 200, { version });
        streamFile(signaturePath, res);
        return;
      }
    } else if (handleSystemImage(req, url, res)) {
      return;
    }
    record(req, url, 404);
    res.writeHead(404);
    res.end("Not found");
  };
  let server: net.Server;
  if (config.secure) {
    const pem = serverKeyAndCertificate(localIP);
    server = https.createServer({ key: pem, cert: pem }, handler);
  } else {
    server = http.createServer(handler);
  }

  await new Promise<void>((resolve, reject) => {
    server.on("error", reject);
    server.listen(port, "0.0.0.0", () => resolve());
  });

  const actualPort = (server.address() as { port: number }).port;
  baseUrl = `${config.secure ? "https" : "http"}://${localIP}:${actualPort}`;

  return {
    url: baseUrl,
    port: actualPort,
    requests,
    releasesRequests: () => requests.filter(r => r.path === "/releases"),
    close: () =>
      new Promise<void>((resolve, reject) => {
        server.close(err => (err ? reject(err) : resolve()));
      }),
    enableSignature: (sigPath: string) => {
      signaturePath = sigPath;
    },
    disableSignature: () => {
      signaturePath = undefined;
    },
  };
}

// ---------------------------------------------------------------------------
// Device control over JSON-RPC.
//
// The Mini has no shell. Everything upstream's OTA specs do with ssh + sed
// has an RPC: setUpdateApiUrl, setDevChannelState, reboot, getLocalVersion.
// RPC runs over a WebRTC data channel, so it needs a browser.
// ---------------------------------------------------------------------------

export interface DeviceControl {
  browser?: Browser;
}

function requireBrowser(ctl: DeviceControl, what: string): Browser {
  if (!ctl.browser) {
    throw new Error(`${what}: the device has no shell; pass { browser } so it can use RPC`);
  }
  return ctl.browser;
}

export async function configureDeviceUpdateUrl(
  url: string,
  ctl: DeviceControl = {},
): Promise<void> {
  const browser = requireBrowser(ctl, "configureDeviceUpdateUrl");
  await withRpcPage(browser, page => callJsonRpc(page, "setUpdateApiUrl", { url }));
}

export async function restoreDeviceUpdateUrl(ctl: DeviceControl = {}): Promise<void> {
  try {
    await configureDeviceUpdateUrl(PRODUCTION_API_URL, ctl);
  } catch {
    // Best-effort cleanup
  }
}

export async function setIncludePreRelease(value: boolean, ctl: DeviceControl = {}): Promise<void> {
  const browser = requireBrowser(ctl, "setIncludePreRelease");
  await withRpcPage(browser, page => callJsonRpc(page, "setDevChannelState", { enabled: value }));
}

/** Whether the device installs updates on its own. */
export async function getAutoUpdateState(page: Page): Promise<boolean> {
  return (await callJsonRpc(page, "getAutoUpdateState", {})) === true;
}

export async function setAutoUpdateState(enabled: boolean, ctl: DeviceControl = {}): Promise<void> {
  const browser = requireBrowser(ctl, "setAutoUpdateState");
  await withRpcPage(browser, page => callJsonRpc(page, "setAutoUpdateState", { enabled }));
}

/** Reboot and wait until the device answers again. */
export async function rebootDevice(ctl: DeviceControl = {}): Promise<void> {
  const browser = requireBrowser(ctl, "rebootDevice");
  await withRpcPage(browser, page => rebootAndReconnect(page));
}

/**
 * Call tryUpdate and return once the device has gone down for the reboot
 * that completes the install. The reply to tryUpdate is often lost with the
 * connection, so it is not awaited. With `components` the pinned
 * tryUpdateComponents variant is used, which installs any different version,
 * downgrades included — the way the UI's custom-version path does.
 */
export async function tryUpdateAndWaitForReboot(
  page: Page,
  opts: { installTimeoutMs?: number; components?: { app?: string; system?: string } } = {},
): Promise<void> {
  const installTimeoutMs = opts.installTimeoutMs ?? 300_000;
  const call = opts.components
    ? rawJsonRpc(
        page,
        "tryUpdateComponents",
        { params: { components: opts.components }, includePreRelease: false, resetConfig: false },
        10_000,
      )
    : rawJsonRpc(page, "tryUpdate", {}, 10_000);
  await Promise.all([
    call.catch(() => undefined),
    waitForDeviceDown(page, "device must go down to reboot into the new image", installTimeoutMs),
  ]);
}

export interface UpdateStatus {
  local?: { appVersion?: string; systemVersion?: string };
  remote?: { appVersion?: string; systemVersion?: string; systemUrl?: string };
  systemUpdateAvailable?: boolean;
  appUpdateAvailable?: boolean;
}

/** Run the device's update check (a real request to its update server). */
export async function getUpdateStatus(page: Page): Promise<UpdateStatus> {
  return (await callJsonRpc(page, "getUpdateStatus", {}, 30_000)) as UpdateStatus;
}

/**
 * The Mini's live transport statistics document. `query` picks a variant:
 * "bitrate=1" is the short one that carries the uptime (nowMs), "details=1"
 * measures the largest free blocks now instead of serving them from a cache.
 */
export async function fetchWebrtcStats(query = ""): Promise<Record<string, unknown>> {
  const res = await fetch(`${process.env.JETKVM_URL}/webrtc/stats${query ? `?${query}` : ""}`, {
    signal: AbortSignal.timeout(10_000),
  });
  if (!res.ok) throw new Error(`/webrtc/stats returned ${res.status}`);
  return (await res.json()) as Record<string, unknown>;
}

/** One owner's TLS admissions since boot, from the Mini's memory policy. */
export interface TlsAdmission {
  owner: string;
  admitted: number;
  denied: number;
  lastInternalHeadroom: number;
  minInternalHeadroom: number;
  lastDmaHeadroom: number;
  minDmaHeadroom: number;
}

export async function tlsAdmission(owner: string): Promise<TlsAdmission> {
  const budget = (await fetchWebrtcStats()).memoryBudget as
    | { tlsAdmissions?: TlsAdmission[] }
    | undefined;
  if (!budget?.tlsAdmissions) throw new Error("/webrtc/stats has no memoryBudget.tlsAdmissions");
  return (
    budget.tlsAdmissions.find(a => a.owner === owner) ?? {
      owner,
      admitted: 0,
      denied: 0,
      lastInternalHeadroom: 0,
      minInternalHeadroom: 0,
      lastDmaHeadroom: 0,
      minDmaHeadroom: 0,
    }
  );
}

/**
 * The reason for videoStops() in a test that makes update checks: a check
 * stops the picture for the time of its TLS handshake (a known limitation:
 * the handshake task can take the CPU core of the capture task), which
 * tools/combined_load judges.
 */
export const UPDATE_CHECK_STOPS_VIDEO =
  "an update check stops the picture for the time of its TLS handshake";

/**
 * Run the update check from page against the production server and require
 * that it connected: the Mini admitted its HTTPS connection under the
 * "ota-download" TLS ticket (the firmware download takes the same ticket) and
 * production answered with a release.
 */
export async function expectUpdateCheckRuns(page: Page): Promise<TlsAdmission> {
  const configured = (await callJsonRpc(page, "getUpdateApiUrl")) as string;
  if (configured !== PRODUCTION_API_URL) {
    await callJsonRpc(page, "setUpdateApiUrl", { url: PRODUCTION_API_URL });
  }
  try {
    const before = await tlsAdmission("ota-download");
    const outcome = await getUpdateStatus(page).catch((error: Error) => error);
    const after = await tlsAdmission("ota-download");
    expect(after.denied, `the check was refused: ${JSON.stringify(after)}`).toBe(before.denied);
    expect(after.admitted, "the check takes the ota-download TLS ticket").toBeGreaterThan(
      before.admitted,
    );
    expect(outcome instanceof Error ? outcome.message : "", "the check itself").toBe("");
    expect(
      (outcome as UpdateStatus).remote?.systemVersion,
      "production offers a release",
    ).toBeTruthy();
    return after;
  } finally {
    if (configured !== PRODUCTION_API_URL) {
      await callJsonRpc(page, "setUpdateApiUrl", { url: configured });
    }
  }
}

/**
 * Firmware version as the Mini reports it over RPC: its firmware is one image,
 * so the system version (it leaves appVersion out).
 */
export async function getLocalVersion(page: Page): Promise<string | null> {
  await waitForWebRTCReady(page);
  // Explicit timeout: right after an install reboot the device can leave a
  // request unanswered, and the default would wait on it indefinitely.
  const local = (await callJsonRpc(page, "getLocalVersion", {}, 10_000)) as {
    appVersion?: string;
    systemVersion?: string;
  };
  const app = local.appVersion?.trim();
  // Some Mini releases that the OTA specs install report "n/a".
  if (app && app !== "n/a") return app;
  // Bench metadata is not part of the OTA release identifier.
  return local.systemVersion?.trim().replace(/\+codelog$/, "") || null;
}

/** getLocalVersion through a fresh page, retried once: right after a reboot the first RPC can time out. */
async function readLocalVersion(browser: Browser): Promise<string | null> {
  try {
    return await withRpcPage(browser, page => getLocalVersion(page));
  } catch {
    await new Promise(resolve => setTimeout(resolve, 5000));
    return withRpcPage(browser, page => getLocalVersion(page));
  }
}

export interface InstallViaOtaOptions {
  installTimeoutMs?: number;
  rebootTimeoutMs?: number;
}

// The firmware's OTA_CONFIRM_DELAY_MS (10 s, main.c) and a margin, counted
// from when the device answers again, which is after userspace started.
export const OTA_CONFIRM_WAIT_MS = 15_000;

/**
 * Install `release` through the device's own OTA path from a temporary mock
 * server: set the update URL, call tryUpdate, wait for the reboot, verify the
 * version and that the firmware confirmed the image, restore the URL. Skips
 * when the device already runs `release`.
 */
export async function installViaOta(
  browser: Browser,
  release: MockRelease,
  opts: InstallViaOtaOptions = {},
): Promise<void> {
  const installTimeoutMs = opts.installTimeoutMs ?? 300_000;
  const rebootTimeoutMs = opts.rebootTimeoutMs ?? 120_000;
  const host = getDeviceHost();

  await waitForDeviceReady(host, 120_000);
  const running = await readLocalVersion(browser);
  if (running === release.version) return;

  const mock = await createMockUpdateServer({
    component: "system",
    version: release.version,
    stageDir: release.stageDir,
    binaryPath: release.binaryPath,
    ignoreChannel: true,
  });
  try {
    await configureDeviceUpdateUrl(mock.url, { browser });
    // A pinned install is a custom-version install to the firmware, which
    // persists auto-update off afterwards; put the setting back.
    const autoUpdate = await withRpcPage(browser, async page => {
      const enabled = await getAutoUpdateState(page);
      // Pin the version: without a constraint the device only offers a
      // semver-greater release, and a baseline install is a downgrade.
      const components = { system: release.version };
      const status = (await callJsonRpc(page, "checkUpdateComponents", {
        params: { components },
      })) as {
        systemUpdateAvailable?: boolean;
        remote?: { systemVersion?: string };
      };
      if (!status.systemUpdateAvailable) {
        throw new Error(
          `installViaOta: device does not consider ${release.version} an update over ${running} ` +
            `(offered ${status.remote?.systemVersion ?? "nothing"})`,
        );
      }
      await tryUpdateAndWaitForReboot(page, { installTimeoutMs, components });
      return enabled;
    });
    await new Promise(resolve => setTimeout(resolve, 2000));
    await waitForDeviceReady(host, rebootTimeoutMs);
    const now = await readLocalVersion(browser);
    if (now !== release.version) {
      throw new Error(`installViaOta: device reports ${now} after installing ${release.version}`);
    }
    // Until the firmware confirms the new image, the bootloader would roll it
    // back and the firmware refuses the next OTA.
    await new Promise(resolve => setTimeout(resolve, OTA_CONFIRM_WAIT_MS));
    if (autoUpdate) await setAutoUpdateState(true, { browser });
  } finally {
    await restoreDeviceUpdateUrl({ browser });
    await mock.close();
  }
}

/** Query parameter the update page uses for a custom version of the device's firmware component. */
export async function customVersionQuery(version: string): Promise<string> {
  return `custom_system_version=${version}`;
}

export async function computeFileHash(filePath: string): Promise<string> {
  return new Promise((resolve, reject) => {
    const hash = crypto.createHash("sha256");
    const stream = fs.createReadStream(filePath);
    stream.on("data", data => hash.update(data));
    stream.on("end", () => resolve(hash.digest("hex")));
    stream.on("error", reject);
  });
}

export function getLocalNetworkIP(): string {
  try {
    const routeOutput = execSync("ip route get 1", {
      encoding: "utf8",
      stdio: ["ignore", "pipe", "ignore"],
    });
    const routeMatch = routeOutput.match(/\bsrc\s+(\d+\.\d+\.\d+\.\d+)\b/);
    if (routeMatch?.[1]) {
      return routeMatch[1];
    }
  } catch {
    // Fall through to interface scan
  }

  const interfaces = os.networkInterfaces();
  for (const name of Object.keys(interfaces)) {
    for (const iface of interfaces[name]!) {
      if (iface.family === "IPv4" && !iface.internal) {
        return iface.address;
      }
    }
  }
  throw new Error("Could not detect local network IP address");
}

export interface StableReleaseInfo {
  appVersion: string;
  appUrl: string;
  appHash: string;
  appSigUrl?: string;
}

export async function fetchLatestStableRelease(): Promise<StableReleaseInfo> {
  const url = "https://api.jetkvm.com/releases?deviceId=e2e-test";
  const body = await new Promise<string>((resolve, reject) => {
    https
      .get(url, res => {
        if (res.statusCode !== 200) {
          reject(new Error(`Release API returned ${res.statusCode}`));
          res.resume();
          return;
        }
        let data = "";
        res.on("data", chunk => (data += chunk));
        res.on("end", () => resolve(data));
        res.on("error", reject);
      })
      .on("error", reject);
  });

  const json = JSON.parse(body);
  if (!json.appVersion || !json.appUrl || !json.appHash) {
    throw new Error(`Unexpected release API response: ${body}`);
  }
  return {
    appVersion: json.appVersion,
    appUrl: json.appUrl,
    appHash: json.appHash,
    appSigUrl: json.appSigUrl,
  };
}

export async function downloadFile(url: string, destPath: string): Promise<void> {
  const proto = url.startsWith("https") ? https : http;
  const file = fs.createWriteStream(destPath);

  await new Promise<void>((resolve, reject) => {
    const request = (requestUrl: string) => {
      proto
        .get(requestUrl, res => {
          if (
            res.statusCode &&
            res.statusCode >= 300 &&
            res.statusCode < 400 &&
            res.headers.location
          ) {
            request(res.headers.location);
            return;
          }
          if (res.statusCode !== 200) {
            reject(new Error(`Download failed: ${res.statusCode} for ${requestUrl}`));
            res.resume();
            return;
          }
          res.pipe(file);
          file.on("finish", () => file.close(() => resolve()));
          res.on("error", reject);
        })
        .on("error", reject);
    };
    request(url);
  });
}

export interface OTAEnvVars {
  /** Shell devices: the app binary to scp as the baseline. */
  baselinePath?: string;
  /** Shell devices: the app binary the mock offers. Mini: a single system image. */
  releasePath?: string;
  releaseVersion: string;
  signaturePath?: string;
  /** Mini: release directories with skus/<sku>/jetkvm-mini.bin. */
  releaseStageDir?: string;
  baselineStageDir?: string;
  baselineVersion?: string;
}

/**
 * Shell devices need BASELINE_BINARY_PATH, RELEASE_BINARY_PATH and
 * TEST_UPDATE_VERSION. A device without a shell needs TEST_UPDATE_VERSION
 * with RELEASE_STAGE_DIR (or RELEASE_BINARY_PATH), and BASELINE_VERSION with
 * BASELINE_STAGE_DIR (or BASELINE_BINARY_PATH).
 */
export function getOTAEnvVars(opts?: { requireSignature?: boolean }): OTAEnvVars {
  const baselinePath = process.env.BASELINE_BINARY_PATH;
  const releasePath = process.env.RELEASE_BINARY_PATH;
  const releaseVersion = process.env.TEST_UPDATE_VERSION;
  const signaturePath = process.env.RELEASE_SIGNATURE_PATH;
  const releaseStageDir = process.env.RELEASE_STAGE_DIR;
  const baselineStageDir = process.env.BASELINE_STAGE_DIR;
  const baselineVersion = process.env.BASELINE_VERSION;

  // The images to install are a bench precondition: without them the OTA
  // specs skip and name what is missing.
  const missing = [
    !releaseVersion && "TEST_UPDATE_VERSION",
    !releasePath && !releaseStageDir && "RELEASE_BINARY_PATH or RELEASE_STAGE_DIR",
    !baselinePath && !baselineStageDir && "BASELINE_BINARY_PATH or BASELINE_STAGE_DIR",
    !baselinePath && !baselineVersion && "BASELINE_VERSION",
    opts?.requireSignature && !signaturePath && "RELEASE_SIGNATURE_PATH",
  ].filter(Boolean);
  test.skip(missing.length > 0, `needs release images: set ${missing.join(", ")}`);

  return {
    baselinePath,
    releasePath,
    // test.skip above throws when it is missing.
    releaseVersion: releaseVersion!,
    signaturePath,
    releaseStageDir,
    baselineStageDir,
    baselineVersion,
  };
}

/** The mock release for the candidate build under test. */
export function releaseFromEnv(env: OTAEnvVars, version = env.releaseVersion): MockRelease {
  return { version, stageDir: env.releaseStageDir, binaryPath: env.releasePath };
}

/** The mock release for the baseline the device starts from. */
export function baselineFromEnv(env: OTAEnvVars): MockRelease | undefined {
  if (!env.baselineVersion) return undefined;
  return {
    version: env.baselineVersion,
    stageDir: env.baselineStageDir,
    binaryPath: env.baselinePath,
  };
}

/**
 * The latest production release of `sku`, or its release `version`, as a
 * stage directory with that SKU alone. It is downloaded from the production
 * update server once and kept in ~/.cache/jetkvm-e2e/mini-release-<sku>
 * (mini-release-<sku>-<version> for a `version` that directory does not hold);
 * delete that directory to take a newer release.
 */
export async function productionRelease(sku: string, version?: string): Promise<MockRelease> {
  const cache = process.env.XDG_CACHE_HOME ?? path.join(os.homedir(), ".cache");
  let stageDir = path.join(cache, "jetkvm-e2e", `mini-release-${sku}`);
  const cached = (dir: string) => fs.readFileSync(path.join(dir, "version"), "utf8").trim();
  if (version && !(fs.existsSync(path.join(stageDir, "version")) && cached(stageDir) === version)) {
    stageDir += `-${version}`;
  }
  const versionFile = path.join(stageDir, "version");
  if (!fs.existsSync(versionFile)) {
    const query = new URLSearchParams({ deviceId: "jetkvm-e2e", sku });
    if (version) query.set("systemVersion", version);
    const response = await fetch(`${PRODUCTION_API_URL}/releases?${query.toString()}`);
    if (!response.ok)
      throw new Error(`${PRODUCTION_API_URL} has no ${sku} release: ${response.status}`);
    const release = (await response.json()) as {
      systemVersion: string;
      systemUrl: string;
      systemHash: string;
    };
    const download = await fetch(release.systemUrl);
    if (!download.ok) throw new Error(`${release.systemUrl}: ${download.status}`);
    const image = Buffer.from(await download.arrayBuffer());
    const hash = crypto.createHash("sha256").update(image).digest("hex");
    if (hash !== release.systemHash.toLowerCase()) {
      throw new Error(`${release.systemUrl} has SHA-256 ${hash}, not ${release.systemHash}`);
    }
    // Written aside and renamed, so that an interrupted download leaves nothing.
    const partial = `${stageDir}.${process.pid}`;
    fs.rmSync(partial, { recursive: true, force: true });
    fs.mkdirSync(path.join(partial, "skus", sku), { recursive: true });
    fs.writeFileSync(path.join(partial, "skus", sku, MINI_IMAGE_NAME), image);
    fs.writeFileSync(path.join(partial, "version"), release.systemVersion);
    fs.rmSync(stageDir, { recursive: true, force: true });
    fs.renameSync(partial, stageDir);
  }
  return { version: cached(stageDir), stageDir };
}

export function toPreReleaseVersion(version: string): string {
  return version.includes("-") ? version : `${version}-dev.1`;
}

/**
 * Open the update dialog with a working session. The device allows one
 * session; a leftover one (another tab, an earlier helper page) puts an
 * "Another Active Session" takeover dialog in front of everything, so claim
 * the session first and dismiss the dialog if it shows up after navigating.
 */
export async function openUpdatePage(page: Page, query = ""): Promise<void> {
  await ensureRpcReady(page, { timeoutMs: 60_000, navigateFirst: true });
  await page.goto(`/settings/general/update${query ? `?${query}` : ""}`, {
    waitUntil: "networkidle",
    timeout: 30_000,
  });
  // The full page load opened a new session. The dialog's first state renders
  // at once, but the takeover prompt arrives only when signaling notices the
  // previous session, so the dialog being visible is not enough: this page
  // must also be the connected one. Click through until both hold.
  const useHere = page.getByRole("button", { name: "Use Here" });
  const dialog = page.getByText(
    /Checking for updates|System is up to date|Update Error|Update Now|Rebooting to complete/,
  );
  const connected = () =>
    page
      .evaluate(() => {
        const hooks = window.__kvmTestHooks;
        return !!hooks && hooks.isWebRTCConnected() && hooks.isHidRpcReady();
      })
      .catch(() => false);
  const deadline = Date.now() + 45_000;
  while (Date.now() < deadline) {
    if (await useHere.isVisible({ timeout: 250 }).catch(() => false)) {
      await useHere.click();
      await page.waitForTimeout(1000);
      continue;
    }
    const dialogVisible = await dialog
      .first()
      .isVisible({ timeout: 250 })
      .catch(() => false);
    if (dialogVisible && (await connected())) {
      // Give a late takeover prompt one more chance to show before trusting the page.
      await page.waitForTimeout(1000);
      if (!(await useHere.isVisible({ timeout: 250 }).catch(() => false))) return;
      continue;
    }
    await page.waitForTimeout(500);
  }
  throw new Error("update dialog did not appear on a connected page");
}

/**
 * Navigate to the update page, dismiss a cached error (Retry button) if present,
 * and click "Update Now".
 */
export async function triggerUpdate(page: Page): Promise<void> {
  await openUpdatePage(page);

  const retryButton = page.getByRole("button", { name: "Retry" });
  if (await retryButton.isVisible({ timeout: 5000 }).catch(() => false)) {
    await retryButton.click();
  }

  const updateButton = page.getByRole("button", { name: "Update Now" });
  await expect(updateButton).toBeVisible({ timeout: 30000 });
  await updateButton.click();
}

/** Put the device in no-password mode from a page of its own, then close it. */
export async function ensureNoPasswordMode(browser: Browser): Promise<void> {
  const context = await browser.newContext({ baseURL: process.env.JETKVM_URL });
  const page = await context.newPage();
  try {
    await ensureLocalAuthMode(page, { mode: "noPassword" });
  } finally {
    await page.close();
    await context.close();
  }
}
