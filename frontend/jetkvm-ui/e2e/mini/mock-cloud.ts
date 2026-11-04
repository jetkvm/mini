/**
 * A local stand-in for JetKVM Cloud (jetkvm/cloud-api) and its Google login,
 * for the Mini's cloud specs. No dependencies.
 *
 * It signs its own Google-style ID tokens, so the Mini must run a bench build
 * that fetches the signing keys from this server instead of Google:
 *
 *   idf.py -DJETKVM_MOCK_CLOUD_JWKS_URL=http://<this host>:<MOCK_CLOUD_PORT>/oidc/jwks ...
 *
 * Such a build also accepts an http:// cloud URL, trusts the test CA in
 * mock-cloud-ca.pem (sdkconfig.mockcloud.defaults) and reports "mockcloud" in
 * its version. The server listens on MOCK_CLOUD_PORT on every interface, takes
 * HTTP and HTTPS on that port (`url` and `secureUrl`; the HTTPS certificate is
 * signed by the test CA at start, which needs the openssl command) and serves:
 * - the key set at /oidc/jwks;
 * - /signup, which stands in for the cloud app and Google: it sends the
 *   browser back to the Mini's /adopt with a temporary token and an ID token;
 * - the cloud API the Mini calls: POST /devices/token and DELETE /devices/:id;
 * - the Mini's WebSocket at /, and a browser's signaling WebSocket at
 *   /webrtc/signaling/client, relayed as cloud-api's webrtc-signaling.ts does;
 * - every other request, proxied to the Mini, so that a page loaded from this
 *   server runs the Mini's own UI with its signaling through the relay.
 */
import { execFileSync } from "node:child_process";
import { createHash, generateKeyPairSync, randomBytes, sign } from "node:crypto";
import http from "node:http";
import https from "node:https";
import net from "node:net";
import type { TLSSocket } from "node:tls";
import type { Duplex } from "node:stream";
import { fileURLToPath } from "node:url";

// Per bench (JETKVM_MOCK_CLOUD_PORT in tools/benches/), so that two benches
// can run the cloud specs at the same time from one machine.
export const MOCK_CLOUD_PORT = Number(process.env.JETKVM_MOCK_CLOUD_PORT ?? 8790);
export const MOCK_CLIENT_ID = "mock-cloud-client.apps.googleusercontent.com";
const ISSUER = "https://accounts.google.com";
const SUBJECT = "mock-cloud-user";

/** A version from a -DJETKVM_MOCK_CLOUD_JWKS_URL build (CMakeLists.txt). */
export function isMockCloudBuild(version: string): boolean {
  const prerelease = version.split("+")[0].split("-").slice(1).join("-");
  return prerelease.split(".").includes("mockcloud");
}

// --- WebSocket (RFC 6455), enough for text messages, ping and close --------

class WebSocketPeer {
  private buffer = Buffer.alloc(0);
  private fragments: Buffer[] = [];
  onMessage: (text: string) => void = () => {};
  onClose: () => void = () => {};
  closed = false;

  readonly socket: Duplex;

  constructor(socket: Duplex, head: Buffer) {
    this.socket = socket;
    // Bytes that came with the upgrade request, after the caller has set
    // onMessage and before the next socket event.
    if (head.length) queueMicrotask(() => this.receive(head));
    socket.on("data", chunk => this.receive(chunk as Buffer));
    socket.on("close", () => this.finish());
    socket.on("error", () => this.finish());
  }

  send(text: string): void {
    this.frame(0x1, Buffer.from(text));
  }

  /** Drop the connection without a close frame, as a network failure does. */
  destroy(): void {
    this.socket.destroy();
    this.finish();
  }

  private finish(): void {
    if (this.closed) return;
    this.closed = true;
    this.onClose();
  }

  private frame(opcode: number, payload: Buffer): void {
    if (this.closed) return;
    let header: Buffer;
    if (payload.length < 126) {
      header = Buffer.from([0x80 | opcode, payload.length]);
    } else if (payload.length < 65536) {
      header = Buffer.from([0x80 | opcode, 126, 0, 0]);
      header.writeUInt16BE(payload.length, 2);
    } else {
      header = Buffer.alloc(10);
      header[0] = 0x80 | opcode;
      header[1] = 127;
      header.writeBigUInt64BE(BigInt(payload.length), 2);
    }
    this.socket.write(Buffer.concat([header, payload]));
  }

  private receive(chunk: Buffer): void {
    this.buffer = Buffer.concat([this.buffer, chunk]);
    for (;;) {
      if (this.buffer.length < 2) return;
      const fin = (this.buffer[0] & 0x80) !== 0;
      const opcode = this.buffer[0] & 0x0f;
      const masked = (this.buffer[1] & 0x80) !== 0;
      let length = this.buffer[1] & 0x7f;
      let offset = 2;
      if (length === 126) {
        if (this.buffer.length < 4) return;
        length = this.buffer.readUInt16BE(2);
        offset = 4;
      } else if (length === 127) {
        if (this.buffer.length < 10) return;
        length = Number(this.buffer.readBigUInt64BE(2));
        offset = 10;
      }
      const mask = masked ? this.buffer.subarray(offset, offset + 4) : null;
      if (masked) offset += 4;
      if (this.buffer.length < offset + length) return;
      const payload = Buffer.from(this.buffer.subarray(offset, offset + length));
      this.buffer = this.buffer.subarray(offset + length);
      if (mask) for (let i = 0; i < payload.length; i++) payload[i] ^= mask[i % 4];

      if (opcode === 0x8) {
        this.frame(0x8, payload.subarray(0, 2));
        this.socket.end();
        return;
      }
      if (opcode === 0x9) {
        this.frame(0xa, payload);
        continue;
      }
      if (opcode === 0xa) continue;
      // Text, binary or a continuation; the Mini splits long messages.
      this.fragments.push(payload);
      if (fin) {
        const text = Buffer.concat(this.fragments).toString("utf8");
        this.fragments = [];
        try {
          this.onMessage(text);
        } catch (error) {
          console.warn(`mock cloud: message not handled: ${String(error)}: ${text.slice(0, 200)}`);
        }
      }
    }
  }
}

function acceptWebSocket(
  req: http.IncomingMessage,
  socket: Duplex,
  head: Buffer,
): WebSocketPeer | null {
  const key = req.headers["sec-websocket-key"];
  if (typeof key !== "string" || req.headers.upgrade?.toLowerCase() !== "websocket") return null;
  const accept = createHash("sha1")
    .update(key + "258EAFA5-E914-47DA-95CA-C5AB0DC85B11")
    .digest("base64");
  socket.write(
    "HTTP/1.1 101 Switching Protocols\r\nUpgrade: websocket\r\nConnection: Upgrade\r\n" +
      `Sec-WebSocket-Accept: ${accept}\r\n\r\n`,
  );
  return new WebSocketPeer(socket, head);
}

// --- HTTPS with the test CA -----------------------------------------------

const CA_CERT = fileURLToPath(new URL("./mock-cloud-ca.pem", import.meta.url));
const CA_KEY = fileURLToPath(new URL("./mock-cloud-ca.key", import.meta.url));

/**
 * A new P-256 key and a certificate for `host` signed by the test CA, valid
 * for 2 days, as one PEM text (Node's TLS takes each from it). The mock
 * update server (ota.ts) serves HTTPS with one too.
 */
export function serverKeyAndCertificate(host: string): string {
  const name = net.isIP(host) ? `IP:${host}` : `DNS:${host}`;
  const args = ["req", "-new", "-x509", "-noenc", "-subj", `/CN=${host}`, "-days", "2"];
  args.push("-newkey", "ec", "-pkeyopt", "ec_paramgen_curve:P-256", "-keyout", "-");
  args.push("-addext", `subjectAltName=${name}`);
  args.push("-addext", "basicConstraints=critical,CA:FALSE");
  args.push("-CA", CA_CERT, "-CAkey", CA_KEY);
  return execFileSync("openssl", args, { encoding: "utf8", stdio: ["ignore", "pipe", "ignore"] });
}

// --- Google-style ID tokens ------------------------------------------------

const base64url = (data: Buffer | string) => Buffer.from(data).toString("base64url");

// --- The mock cloud --------------------------------------------------------

export interface DeviceConnection {
  at: number;
  appVersion: string;
  /** Over TLS (wss), as the real cloud. */
  secure: boolean;
}

export interface MockCloud {
  /** The cloud URL the Mini is given (API and app are the same server). */
  url: string;
  /** The same server over HTTPS, so the Mini's cloud socket uses wss. */
  secureUrl: string;
  /** Requests for the key set. */
  jwksFetches: number;
  /** Successful POST /devices/token calls. */
  tokenExchanges: number;
  /** Accepted Mini WebSockets, oldest first. */
  deviceConnections: DeviceConnection[];
  /** Offers relayed to the Mini and answers relayed back. */
  offers: number;
  answers: number;
  /** DELETE /devices/:id calls with the Mini's token. */
  deregistrations: number;
  /** What the mock received and relayed, with times, for failure reports. */
  events: string[];
  /** Whether the Mini's WebSocket is open. */
  deviceConnected(): boolean;
  /** Drop the Mini's WebSocket without a close frame. */
  dropDevice(): void;
  close(): Promise<void>;
}

/**
 * Start the mock cloud for the Mini at `deviceUrl`. `host` is this machine's
 * address as the Mini sees it (brokerHost in mqtt-broker.ts).
 */
export async function startMockCloud(host: string, deviceUrl: string): Promise<MockCloud> {
  const { privateKey, publicKey } = generateKeyPairSync("rsa", { modulusLength: 2048 });
  const kid = randomBytes(8).toString("hex");
  const jwk = publicKey.export({ format: "jwk" });
  const jwks = JSON.stringify({
    keys: [{ kty: "RSA", alg: "RS256", use: "sig", kid, n: jwk.n, e: jwk.e }],
  });
  const idToken = () => {
    const now = Math.floor(Date.now() / 1000);
    const header = base64url(JSON.stringify({ alg: "RS256", kid, typ: "JWT" }));
    const claims = base64url(
      JSON.stringify({ iss: ISSUER, aud: MOCK_CLIENT_ID, sub: SUBJECT, iat: now, exp: now + 3600 }),
    );
    const signature = sign("sha256", Buffer.from(`${header}.${claims}`), privateKey);
    return `${header}.${claims}.${base64url(signature)}`;
  };

  const device = new URL(deviceUrl);
  const authority = `${host.includes(":") ? `[${host}]` : host}:${MOCK_CLOUD_PORT}`;
  const url = `http://${authority}`;
  let tempToken: string | null = null;
  let deviceId: string | null = null;
  let secretToken: string | null = null;
  let deviceSocket: WebSocketPeer | null = null;
  let deviceIp = "";
  let deviceAppVersion = "";
  // One browser at a time, as cloud-api's inFlight set.
  let browser: WebSocketPeer | null = null;
  const sockets = new Set<Duplex>();

  const cloud: MockCloud = {
    url,
    secureUrl: `https://${authority}`,
    jwksFetches: 0,
    tokenExchanges: 0,
    deviceConnections: [],
    offers: 0,
    answers: 0,
    deregistrations: 0,
    events: [],
    deviceConnected: () => !!deviceSocket && !deviceSocket.closed,
    dropDevice: () => deviceSocket?.destroy(),
    close: () =>
      new Promise(resolve => {
        for (const socket of sockets) socket.destroy();
        listener.close(() => resolve());
      }),
  };

  const note = (event: string) => cloud.events.push(`${new Date().toISOString()} ${event}`);

  const readBody = (req: http.IncomingMessage) =>
    new Promise<string>((resolve, reject) => {
      const chunks: Buffer[] = [];
      req.on("data", chunk => chunks.push(chunk as Buffer));
      req.on("end", () => resolve(Buffer.concat(chunks).toString("utf8")));
      req.on("error", reject);
    });

  const json = (res: http.ServerResponse, status: number, body: unknown) => {
    res.writeHead(status, { "Content-Type": "application/json" });
    res.end(JSON.stringify(body));
  };

  // Everything this server does not answer itself goes to the Mini, with the
  // Mini's own origin so that its same-origin checks see a local page.
  const proxy = (req: http.IncomingMessage, res: http.ServerResponse) => {
    const headers = { ...req.headers, host: device.host };
    if (headers.origin) headers.origin = device.origin;
    if (headers.referer) headers.referer = device.origin + "/";
    const upstream = http.request(
      {
        host: device.hostname,
        port: device.port || 80,
        method: req.method,
        path: req.url,
        headers,
      },
      response => {
        res.writeHead(response.statusCode ?? 502, response.headers);
        response.pipe(res);
      },
    );
    upstream.on("error", () => {
      if (!res.headersSent) res.writeHead(502);
      res.end();
    });
    req.pipe(upstream);
  };

  const handleRequest = (req: http.IncomingMessage, res: http.ServerResponse) => {
    const path = new URL(req.url ?? "/", url);
    void (async () => {
      if (req.method === "GET" && path.pathname === "/oidc/jwks") {
        cloud.jwksFetches++;
        note("key set fetched");
        res.writeHead(200, {
          "Content-Type": "application/json",
          "Cache-Control": "public, max-age=3600",
        });
        return res.end(jwks);
      }
      if (req.method === "GET" && path.pathname === "/signup") {
        // The cloud app signs the user in with Google and returns to the
        // Mini's /adopt (upstream ui/src/routes/adopt.tsx).
        const returnTo = new URL(path.searchParams.get("returnTo") ?? "");
        deviceId = path.searchParams.get("deviceId");
        tempToken = randomBytes(16).toString("hex");
        returnTo.search = new URLSearchParams({
          tempToken,
          deviceId: deviceId ?? "",
          oidcGoogle: idToken(),
          clientId: MOCK_CLIENT_ID,
        }).toString();
        res.writeHead(302, { Location: returnTo.toString() });
        return res.end();
      }
      if (req.method === "GET" && /^\/v\/[^/]+\/devices\/[^/]+\/setup$/.test(path.pathname)) {
        res.writeHead(200, { "Content-Type": "text/html" });
        return res.end("<!doctype html><title>Mock cloud</title><p>Adopted</p>");
      }
      if (req.method === "POST" && path.pathname === "/devices/token") {
        const body = JSON.parse(await readBody(req)) as { tempToken?: string };
        if (!tempToken || body.tempToken !== tempToken)
          return json(res, 404, { message: "Device not found" });
        tempToken = null;
        secretToken = randomBytes(20).toString("hex");
        cloud.tokenExchanges++;
        note("temporary token exchanged");
        return json(res, 200, { secretToken });
      }
      if (req.method === "DELETE" && /^\/devices\/[^/]+$/.test(path.pathname)) {
        if (!secretToken || req.headers.authorization !== `Bearer ${secretToken}`) {
          return json(res, 404, { message: "Device not found" });
        }
        // cloud-api deletes the device and leaves its socket open.
        secretToken = null;
        cloud.deregistrations++;
        note("device deleted");
        res.writeHead(204);
        return res.end();
      }
      proxy(req, res);
    })().catch(() => {
      if (!res.headersSent) res.writeHead(500);
      res.end();
    });
  };

  const handleUpgrade = (req: http.IncomingMessage, socket: Duplex, head: Buffer) => {
    socket.on("error", () => {});
    const path = new URL(req.url ?? "/", url).pathname;
    if (path === "/") {
      // cloud-api authenticates the Mini by its token and device ID.
      if (
        !secretToken ||
        req.headers.authorization !== `Bearer ${secretToken}` ||
        req.headers["x-device-id"] !== deviceId
      ) {
        note("device socket refused");
        return socket.destroy();
      }
      deviceSocket?.destroy();
      const peer = acceptWebSocket(req, socket, head);
      if (!peer) return socket.destroy();
      deviceSocket = peer;
      deviceIp = (socket as net.Socket).remoteAddress ?? "";
      deviceAppVersion = String(req.headers["x-app-version"] ?? "");
      const secure = (socket as TLSSocket).encrypted === true;
      cloud.deviceConnections.push({ at: Date.now(), appVersion: deviceAppVersion, secure });
      note(`device socket open from ${deviceIp}${secure ? " over TLS" : ""}`);
      peer.onMessage = text => {
        const message = JSON.parse(text) as { type: string; data: unknown };
        note(`device -> ${message.type} ${JSON.stringify(message.data).slice(0, 120)}`);
        if (message.type === "answer") cloud.answers++;
        if (message.type === "answer" || message.type === "new-ice-candidate") {
          browser?.send(JSON.stringify({ type: message.type, data: message.data }));
        }
      };
      peer.onClose = () => {
        if (deviceSocket === peer) deviceSocket = null;
        note("device socket closed");
        browser?.destroy();
      };
      return;
    }
    if (path === "/webrtc/signaling/client") {
      if (!cloud.deviceConnected() || browser) {
        socket.write("HTTP/1.1 404 Not Found\r\n\r\n");
        return socket.destroy();
      }
      const peer = acceptWebSocket(req, socket, head);
      if (!peer) return socket.destroy();
      browser = peer;
      note("browser socket open");
      peer.onClose = () => {
        if (browser === peer) browser = null;
        note("browser socket closed");
      };
      peer.send(
        JSON.stringify({
          type: "device-metadata",
          data: { deviceVersion: deviceAppVersion, deviceSku: null },
        }),
      );
      peer.onMessage = text => {
        if (text === "ping") return peer.send("pong");
        const message = JSON.parse(text) as { type: string; data: { sd?: string } };
        note(`browser -> ${message.type} ${JSON.stringify(message.data).slice(0, 120)}`);
        if (message.type === "offer") {
          cloud.offers++;
          // No STUN servers: the Mini and the browser share a LAN.
          deviceSocket?.send(
            JSON.stringify({
              type: "offer",
              data: { sd: message.data.sd, ip: deviceIp, iceServers: [], OidcGoogle: idToken() },
            }),
          );
        } else if (message.type === "new-ice-candidate") {
          deviceSocket?.send(JSON.stringify({ type: "new-ice-candidate", data: message.data }));
        }
      };
      return;
    }
    socket.destroy();
  };

  // One port for both: a connection whose first byte starts a TLS handshake
  // record (0x16) goes to the HTTPS server, any other to the HTTP server.
  const pem = serverKeyAndCertificate(host);
  const server = http.createServer(handleRequest);
  const secureServer = https.createServer({ key: pem, cert: pem }, handleRequest);
  for (const s of [server, secureServer]) s.on("upgrade", handleUpgrade);
  const listener = net.createServer(socket => {
    sockets.add(socket);
    socket.on("close", () => sockets.delete(socket));
    socket.on("error", () => {});
    socket.once("data", first => {
      socket.pause();
      socket.unshift(first);
      (first[0] === 0x16 ? secureServer : server).emit("connection", socket);
      process.nextTick(() => socket.resume());
    });
  });

  await new Promise<void>((resolve, reject) => {
    listener.once("error", reject);
    listener.listen(MOCK_CLOUD_PORT, () => resolve());
  });
  return cloud;
}
