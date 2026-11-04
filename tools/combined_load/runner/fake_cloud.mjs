// A stand-in for JetKVM Cloud: the sign-in and name pages an adoption passes,
// the login's key set, the token exchange, the Mini's WebSocket and device
// removal. It does not relay sessions.
//
// The Mini must run a bench build that takes its keys from this server
// (JETKVM_MOCK_CLOUD_JWKS_URL) and trusts the e2e test CA. The server's TLS
// certificate is signed with that CA at start, which needs openssl 3.
import { execFileSync } from 'node:child_process';
import { createHash, generateKeyPairSync, randomBytes, sign } from 'node:crypto';
import fs from 'node:fs';
import http from 'node:http';
import https from 'node:https';
import net from 'node:net';
import os from 'node:os';
import path from 'node:path';
import { fileURLToPath } from 'node:url';

const CLIENT_ID = 'combined-load.apps.googleusercontent.com';
const WEBSOCKET_GUID = '258EAFA5-E914-47DA-95CA-C5AB0DC85B11';
const base64url = value => Buffer.from(value).toString('base64url');
// The name page of a device: where the Mini's /adopt page sends the browser
// after the registration, under the cloud's app address, with the version
// of the cloud's UI before it when the Mini's answer names one.
const NAME_PAGE = /^(\/v\/[^/]+)?\/devices\/[^/]+\/setup$/;

// The e2e test CA, mock-cloud-ca.pem and mock-cloud-ca.key: the only
// authority besides the public ones that a bench build trusts.
const TEST_CA = fileURLToPath(new URL('../../../frontend/jetkvm-ui/e2e/mini/mock-cloud-ca', import.meta.url));

/**
 * A new key and a certificate chain for a server at `host`, valid for 2 days:
 * { key, cert } as PEM, the server's certificate first. `host` is a name or
 * an address as a URL has it, so an IPv6 address can be in brackets.
 *
 * A Mini that checks the chain does the same work as for api.jetkvm.com: one
 * ECDSA P-256 and one ECDSA P-384 signature on the certificates, and a P-256
 * signature of the server's key on the handshake. api.jetkvm.com has a P-256
 * key in a certificate signed by a P-256 intermediate, which a P-384 root
 * signed. The test CA's key is P-256, so here the P-384 key is the
 * intermediate's: the test CA signs the intermediate, and the intermediate
 * signs the server's P-256 key.
 */
export function serverCertificate(urlHost) {
  const host = urlHost.replace(/^\[(.*)\]$/, '$1');
  const directory = fs.mkdtempSync(path.join(os.tmpdir(), 'combined-load-'));
  const file = name => path.join(directory, name);
  const certificate = (name, issuer, curve, subject, options) => execFileSync('openssl', ['req', '-new', '-x509', '-noenc',
    '-days', '2', '-newkey', 'ec', '-pkeyopt', `ec_paramgen_curve:${curve}`, '-keyout', file(`${name}.key`),
    '-out', file(`${name}.pem`), '-subj', `/CN=${subject}`, '-CA', `${issuer}.pem`, '-CAkey', `${issuer}.key`,
    ...options.flatMap(option => (option.startsWith('-') ? [option] : ['-addext', option]))], { stdio: ['ignore', 'ignore', 'pipe'] });
  try {
    certificate('intermediate', TEST_CA, 'P-384', 'JetKVM combined load intermediate',
      ['basicConstraints=critical,CA:TRUE,pathlen:0', 'keyUsage=critical,keyCertSign']);
    certificate('server', file('intermediate'), 'P-256', host, ['-sha384',
      `subjectAltName=${net.isIP(host) ? 'IP' : 'DNS'}:${host}`, 'basicConstraints=critical,CA:FALSE',
      'keyUsage=critical,digitalSignature', 'extendedKeyUsage=serverAuth']);
    return {
      key: fs.readFileSync(file('server.key'), 'utf8'),
      cert: fs.readFileSync(file('server.pem'), 'utf8') + fs.readFileSync(file('intermediate.pem'), 'utf8'),
    };
  } catch (error) {
    throw new Error(`the cloud's certificate needs the openssl command, 3.0 or later: ${error.message}`);
  } finally {
    fs.rmSync(directory, { recursive: true, force: true });
  }
}

/**
 * jwksUrl: where the Mini's build fetches the keys, on this machine; the
 * cloud listens on its port. With `tls` the cloud is https and wss only, and
 * a request without TLS is refused. Without it the cloud is http and ws, for
 * a build that fetches the keys over http, and still takes TLS.
 *
 * Returns the cloud: its `url`, `signInPage()` for the browser that adopts
 * the Mini, `adoption()` for the Mini's /cloud/register, `connected()`,
 * `socketAt()`, `handshakes()`, `asked()`, `drop()`, `problem()` and
 * `close()`.
 */
export function startFakeCloud(jwksUrl, { tls = true } = {}) {
  const { hostname, port } = new URL(jwksUrl);
  const { privateKey, publicKey } = generateKeyPairSync('rsa', { modulusLength: 2048 });
  const kid = randomBytes(8).toString('hex');
  const { n, e } = publicKey.export({ format: 'jwk' });
  const jwks = JSON.stringify({ keys: [{ kty: 'RSA', alg: 'RS256', use: 'sig', kid, n, e }] });

  let tempToken = null;     // given to the Mini with the login, exchanged once
  let secretToken = null;   // the Mini's own token after the exchange
  let socket = null;        // the Mini's WebSocket
  let socketAt = null;      // when the cloud accepted it
  let heardAt = 0;          // when the Mini last sent something on it
  let handshakes = 0;       // TLS handshakes that the Mini completed
  let handshakeMs = null;   // the last one, from the TCP connection to its end
  let problem = null;       // why the Mini's last request did not get to the cloud
  let keyFetches = 0;       // answers with the key set
  let tokenExchanges = 0;   // adoption tokens that were exchanged
  let sockets = 0;          // WebSockets of the Mini that were accepted
  let appVersion = null;    // the version that the Mini's newest WebSocket named

  // A token to exchange and a login that this cloud signed.
  const adoption = () => {
    const now = Math.floor(Date.now() / 1000);
    const head = `${base64url(JSON.stringify({ alg: 'RS256', kid, typ: 'JWT' }))}.` +
      base64url(JSON.stringify({ iss: 'https://accounts.google.com', aud: CLIENT_ID, sub: 'combined-load', iat: now, exp: now + 3600 }));
    tempToken = randomBytes(16).toString('hex');
    problem = null;
    return { token: tempToken, oidcGoogle: `${head}.${base64url(sign('sha256', Buffer.from(head), privateKey))}`, clientId: CLIENT_ID };
  };

  // The TLS handshake of each connection, until its first request says whose
  // it is: the Mini's handshakes are counted, the browser's are not.
  const uncounted = new WeakMap();
  const fromTheMini = request => {
    const ms = uncounted.get(request.socket);
    if (ms === undefined) return;
    uncounted.delete(request.socket);
    handshakes++;
    handshakeMs = ms;
  };

  const json = (response, status, body) => {
    response.writeHead(status, { 'Content-Type': 'application/json' });
    response.end(JSON.stringify(body));
  };
  // A request without TLS to the https cloud: the Mini's build has an http:// URL for the keys.
  const withoutTls = request => {
    if (!tls || request.socket.encrypted) return false;
    problem = `the Mini asked for ${request.method} ${request.url} without TLS: its build fetches the keys over http`;
    return true;
  };
  const answer = (request, response) => {
    const { pathname, searchParams } = new URL(request.url, 'http://localhost');
    // The two pages are for the browser; every other request is the Mini's.
    const page = request.method === 'GET' && (pathname === '/signup' || NAME_PAGE.test(pathname));
    if (!page) fromTheMini(request);
    else uncounted.delete(request.socket);
    if (withoutTls(request)) {
      json(response, 400, { message: 'This cloud takes https only' });
    } else if (page && pathname === '/signup') {
      // The sign-in page: to the address in returnTo, the Mini's /adopt
      // page, with what that page reads from its address.
      const returnTo = searchParams.get('returnTo');
      if (!URL.canParse(returnTo ?? '')) return json(response, 400, { message: 'No returnTo' });
      const { token, oidcGoogle, clientId } = adoption();
      const adopt = new URL(returnTo);
      adopt.search = new URLSearchParams({ tempToken: token, deviceId: searchParams.get('deviceId') ?? '', oidcGoogle, clientId });
      response.writeHead(302, { Location: adopt.toString(), 'Cache-Control': 'no-store' }).end();
    } else if (page) {
      response.writeHead(200, { 'Content-Type': 'text/html; charset=utf-8', 'Cache-Control': 'no-store' });
      response.end('<!doctype html><title>Name your device</title>');
    } else if (request.method === 'GET' && pathname === '/oidc/jwks') {
      keyFetches++;
      response.writeHead(200, { 'Content-Type': 'application/json' });
      response.end(jwks);
    } else if (request.method === 'POST' && pathname === '/devices/token') {
      let body = '';
      request.on('data', chunk => { body += chunk; });
      request.on('end', () => {
        if (!tempToken || JSON.parse(body || '{}').tempToken !== tempToken) return json(response, 404, { message: 'Device not found' });
        tempToken = null;
        tokenExchanges++;
        secretToken = randomBytes(20).toString('hex');
        json(response, 200, { secretToken });
      });
    } else if (request.method === 'DELETE' && /^\/devices\/[^/]+$/.test(pathname)) {
      if (!secretToken || request.headers.authorization !== `Bearer ${secretToken}`) return json(response, 404, { message: 'Device not found' });
      secretToken = null;
      response.writeHead(204).end();
    } else {
      json(response, 404, { message: 'Not found' });
    }
  };

  // The Mini's WebSocket: accepted with its token, and its pings answered.
  const upgrade = (request, connection) => {
    connection.on('error', () => {});
    const key = request.headers['sec-websocket-key'];
    fromTheMini(request);
    if (withoutTls(request) || !key || !secretToken || request.headers.authorization !== `Bearer ${secretToken}`) return connection.destroy();
    const accept = createHash('sha1').update(key + WEBSOCKET_GUID).digest('base64');
    connection.write('HTTP/1.1 101 Switching Protocols\r\nUpgrade: websocket\r\nConnection: Upgrade\r\n' +
      `Sec-WebSocket-Accept: ${accept}\r\n\r\n`);
    socket?.destroy();
    socket = connection;
    socketAt = heardAt = Date.now();
    sockets++;
    appVersion = request.headers['x-app-version'] ?? null;
    let buffer = Buffer.alloc(0);
    connection.on('data', chunk => {
      heardAt = Date.now();
      buffer = Buffer.concat([buffer, chunk]);
      // A frame from a client: 2 bytes, an extended length, a 4-byte mask, the payload.
      for (;;) {
        if (buffer.length < 2) return;
        const short = buffer[1] & 0x7f;
        const lengthBytes = short === 126 ? 2 : short === 127 ? 8 : 0;
        if (buffer.length < 2 + lengthBytes + 4) return;
        const length = short === 126 ? buffer.readUInt16BE(2) : short === 127 ? Number(buffer.readBigUInt64BE(2)) : short;
        const start = 2 + lengthBytes + 4;
        if (buffer.length < start + length) return;
        const mask = buffer.subarray(start - 4, start);
        const payload = Buffer.from(buffer.subarray(start, start + length).map((byte, i) => byte ^ mask[i % 4]));
        const opcode = buffer[0] & 0x0f;
        buffer = buffer.subarray(start + length);
        if (opcode === 0x9 && payload.length < 126) connection.write(Buffer.concat([Buffer.from([0x8a, payload.length]), payload]));
        if (opcode === 0x8) return connection.destroy();
      }
    });
    connection.on('close', () => { if (socket === connection) socket = null; });
  };

  // One port for both: a connection whose first byte starts a TLS handshake
  // record (0x16) goes to the HTTPS server, any other to the HTTP server.
  const plainServer = http.createServer(answer);
  const tlsServer = https.createServer(serverCertificate(hostname), answer);
  for (const server of [plainServer, tlsServer]) server.on('upgrade', upgrade);
  const connections = new Map();   // the open TCP connections, by the client's address and port, and when each came
  const peer = connection => `${connection.remoteAddress} ${connection.remotePort}`;
  tlsServer.on('secureConnection', connection => {
    uncounted.set(connection, Date.now() - connections.get(peer(connection))?.at);
  });
  // The Mini ends a handshake when it does not accept the certificate.
  tlsServer.on('tlsClientError', error => { problem = `a TLS handshake with the Mini failed: ${error.message}`; });
  const listener = net.createServer(connection => {
    const client = peer(connection);
    connections.set(client, { connection, at: Date.now() });
    connection.on('close', () => connections.delete(client));
    connection.on('error', () => {});
    connection.once('data', first => {
      connection.pause();
      connection.unshift(first);
      (first[0] === 0x16 ? tlsServer : plainServer).emit('connection', connection);
      process.nextTick(() => connection.resume());
    });
  });

  // A Mini that restarts does not close its socket. The cloud pings it every
  // second; a socket whose Mini has not answered for 5 s is not connected.
  const pinger = setInterval(() => { if (socket?.writable) socket.write(Buffer.from([0x89, 0x00])); }, 1000);
  const SILENT_MS = 5000;

  listener.listen(Number(port));
  const url = `${tls ? 'https' : 'http'}://${hostname}:${port}`;
  return {
    url,
    /**
     * The address of the sign-in page as the "Adopt" button of the Mini's
     * settings page makes it, for the Mini with this ID at this address:
     * the page sends the browser to the Mini's /adopt page.
     */
    signInPage(deviceId, miniUrl) {
      const returnTo = new URL(miniUrl);
      returnTo.pathname = '/adopt';
      returnTo.search = '';
      returnTo.hash = '';
      return `${url}/signup?deviceId=${deviceId}&returnTo=${returnTo.toString()}`;
    },
    /** Whether a path of this cloud is the name page of a device, with or without the version of the cloud's UI before it. */
    isNamePage: pathname => NAME_PAGE.test(pathname),
    /** What the Mini's POST /cloud/register takes: a token to exchange and a login this cloud signed. */
    adoption,
    /** Whether the Mini's WebSocket is open and the Mini has answered on it in the last 5 s. */
    connected: () => socket !== null && Date.now() - heardAt < SILENT_MS,
    /** When the cloud accepted the Mini's newest WebSocket, or null before the first. */
    socketAt: () => socketAt,
    /**
     * The TLS handshakes that the Mini completed: how many, and how long the
     * last one took, from the Mini's TCP connection to the end of the
     * handshake. That time is the Mini's: this machine's part is short. A
     * handshake counts when its connection makes its first request, and
     * the handshakes of the browser that asks for the two pages do not.
     */
    handshakes: () => ({ handshakes, handshakeMs }),
    /**
     * What the Mini has asked of this cloud: how often it fetched the key
     * set (keyFetches) and exchanged an adoption token (tokenExchanges), how
     * many of its WebSockets the cloud accepted (sockets), and the version
     * that the newest of them named in X-App-Version (appVersion), which is
     * null before the first socket and for a socket without that header.
     */
    asked: () => ({ keyFetches, tokenExchanges, sockets, appVersion }),
    /** Closes the Mini's WebSocket without a close frame, as a cloud that loses the connection does. */
    drop: () => { socket?.destroy(); },
    /** Why a request since the last adoption() did not get to the cloud, or null. */
    problem: () => problem,
    close: () => { clearInterval(pinger); for (const { connection } of connections.values()) connection.destroy(); listener.close(); },
  };
}
