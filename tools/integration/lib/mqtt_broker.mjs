// Minimal MQTT 3.1.1 broker for tests. QoS 0 and 1 only; retained messages
// are recorded but not delivered to later subscribers.
import { execFileSync } from 'node:child_process';
import fs from 'node:fs';
import net from 'node:net';
import os from 'node:os';
import path from 'node:path';
import tls from 'node:tls';

const CONNECT = 1;
const PUBLISH = 3;
const SUBSCRIBE = 8;
const UNSUBSCRIBE = 10;
const PINGREQ = 12;
const DISCONNECT = 14;
const REFUSED_CREDENTIALS = 4;

function packet(first, body = Buffer.alloc(0)) {
  const length = [];
  let rest = body.length;
  do {
    length.push((rest % 128) | (rest >= 128 ? 0x80 : 0));
    rest = Math.floor(rest / 128);
  } while (rest > 0);
  return Buffer.concat([Buffer.from([first, ...length]), body]);
}

// Splits complete packets off `buffer`: { packets: [{ type, flags, body }], rest }.
function takePackets(buffer) {
  const packets = [];
  let offset = 0;
  for (;;) {
    let length = 0;
    let at = offset + 1;
    let whole = false;
    for (let shift = 0; at < buffer.length && shift <= 21 && !whole; shift += 7) {
      length += (buffer[at] & 0x7f) << shift;
      whole = !(buffer[at++] & 0x80);
    }
    if (!whole || at + length > buffer.length) break;
    packets.push({ type: buffer[offset] >> 4, flags: buffer[offset] & 0x0f, body: buffer.subarray(at, at + length) });
    offset = at + length;
  }
  return { packets, rest: buffer.subarray(offset) };
}

function reader(body) {
  let at = 0;
  const take = count => {
    if (at + count > body.length) throw new Error('the packet is shorter than it says');
    at += count;
    return body.subarray(at - count, at);
  };
  return {
    byte: () => take(1)[0],
    number: () => take(2).readUInt16BE(0),
    field: () => take(take(2).readUInt16BE(0)),
    rest: () => take(body.length - at),
    left: () => body.length - at,
  };
}

function takes(filter, topic) {
  const want = filter.split('/');
  const have = topic.split('/');
  for (let level = 0; level < want.length; level += 1) {
    if (want[level] === '#') return true;
    if (level >= have.length || (want[level] !== '+' && want[level] !== have[level])) return false;
  }
  return want.length === have.length;
}

function untrustedCertificate() {
  const directory = fs.mkdtempSync(path.join(os.tmpdir(), 'integration-mqtt-'));
  try {
    const key = path.join(directory, 'key.pem');
    const cert = path.join(directory, 'cert.pem');
    execFileSync('openssl', ['req', '-x509', '-newkey', 'rsa:2048', '-nodes', '-keyout', key, '-out', cert, '-days', '1',
      '-subj', '/CN=integration-mqtt.invalid'], { stdio: 'pipe', timeout: 20_000 });
    return { key: fs.readFileSync(key), cert: fs.readFileSync(cert) };
  } finally {
    fs.rmSync(directory, { recursive: true, force: true });
  }
}

/**
 * Starts the broker on a free port of `host`. `credentials` ({ username,
 * password }) refuses others; `secure` uses TLS with a self-signed cert.
 * messages() entries have origin 'client', 'will' or 'broker' (publish()).
 * While stop()ped, the port refuses connections and counts them (turnedAway()).
 */
export async function startMqttBroker({ host, credentials = null, secure = false }) {
  const log = [];
  const kept = new Map();       // topic -> retained message
  const sessions = new Set();   // accepted connections
  const sockets = new Set();
  let packetId = 0;

  const route = message => {
    log.push({ type: 'message', ...message, at: Date.now() });
    if (message.retain && message.payload === '') kept.delete(message.topic);
    else if (message.retain) kept.set(message.topic, log.at(-1));
    let delivered = 0;
    for (const session of sessions) {
      const granted = [...session.subscriptions].filter(([filter]) => takes(filter, message.topic)).map(([, qos]) => qos);
      if (granted.length === 0) continue;
      const qos = Math.min(message.qos, Math.max(...granted));
      const topic = Buffer.from(message.topic);
      packetId = (packetId % 0xffff) + 1;
      session.socket.write(packet(0x30 | (qos << 1), Buffer.concat([
        Buffer.from([topic.length >> 8, topic.length & 0xff]), topic,
        qos ? Buffer.from([packetId >> 8, packetId & 0xff]) : Buffer.alloc(0), Buffer.from(message.payload),
      ])));
      delivered += 1;
    }
    return delivered;
  };

  const handle = socket => {
    let pending = Buffer.alloc(0);
    let session = null;
    socket.on('error', () => undefined);
    socket.on('close', () => {
      if (!session) return;
      sessions.delete(session);
      if (session.will && !session.graceful) route({ ...session.will, clientId: session.clientId, origin: 'will' });
    });
    const connect = body => {
      const read = reader(body);
      if (read.field().toString() !== 'MQTT' || read.byte() !== 4) throw new Error('not an MQTT 3.1.1 CONNECT');
      const flags = read.byte();
      read.number();   // keep-alive, not enforced
      const clientId = read.field().toString();
      const will = flags & 0x04
        ? { topic: read.field().toString(), payload: read.field().toString(), qos: (flags >> 3) & 3, retain: Boolean(flags & 0x20) }
        : null;
      const username = flags & 0x80 ? read.field().toString() : '';
      const password = flags & 0x40 ? read.field().toString() : '';
      const accepted = !credentials || (username === credentials.username && password === credentials.password);
      log.push({ type: 'connect', clientId, accepted, at: Date.now() });
      if (!accepted) {
        socket.end(packet(0x20, Buffer.from([0, REFUSED_CREDENTIALS])));
        return;
      }
      for (const other of sessions) if (other.clientId === clientId) other.socket.destroy();
      session = { socket, clientId, will, graceful: false, subscriptions: new Map() };
      sessions.add(session);
      socket.write(packet(0x20, Buffer.from([0, 0])));
    };
    const answer = ({ type, flags, body }) => {
      if (type === CONNECT && !session) return connect(body);
      if (!session) throw new Error('a packet before CONNECT');
      const read = reader(body);
      if (type === PUBLISH) {
        const qos = (flags >> 1) & 3;
        if (qos > 1) throw new Error('QoS 2 is not supported');
        const topic = read.field().toString();
        const id = qos ? read.number() : 0;
        route({ topic, payload: read.rest().toString(), qos, retain: Boolean(flags & 1), clientId: session.clientId, origin: 'client' });
        if (qos) socket.write(packet(0x40, Buffer.from([id >> 8, id & 0xff])));
      } else if (type === SUBSCRIBE) {
        const id = read.number();
        const granted = [];
        do {
          const filter = read.field().toString();
          const qos = Math.min(read.byte(), 1);
          session.subscriptions.set(filter, qos);
          granted.push(qos);
        } while (read.left() > 0);
        socket.write(packet(0x90, Buffer.from([id >> 8, id & 0xff, ...granted])));
      } else if (type === UNSUBSCRIBE) {
        const id = read.number();
        while (read.left() > 0) session.subscriptions.delete(read.field().toString());
        socket.write(packet(0xb0, Buffer.from([id >> 8, id & 0xff])));
      } else if (type === PINGREQ) {
        socket.write(packet(0xd0));
      } else if (type === DISCONNECT) {
        session.graceful = true;
        socket.end();
      }
    };
    socket.on('data', chunk => {
      const { packets, rest } = takePackets(Buffer.concat([pending, chunk]));
      pending = rest;
      try {
        packets.forEach(answer);
      } catch {
        socket.destroy();
      }
    });
  };

  const server = secure ? tls.createServer(untrustedCertificate(), handle) : net.createServer(handle);
  server.on('tlsClientError', error => log.push({ type: 'tlsRejected', error: error.message, at: Date.now() }));
  server.on('connection', socket => {
    sockets.add(socket);
    socket.on('close', () => sockets.delete(socket));
  });
  const listen = port => new Promise((resolve, reject) => {
    server.once('error', reject);
    server.listen(port, host, () => {
      server.removeListener('error', reject);
      resolve();
    });
  });
  await listen(0);
  const { port } = server.address();
  const closed = one => new Promise(resolve => (one?.listening ? one.close(() => resolve()) : resolve()));
  const end = async () => {
    // No last wills when the broker itself goes away.
    for (const session of sessions) session.graceful = true;
    for (const socket of sockets) socket.destroy();
    await closed(server);
  };
  let away = null;
  let turnedAway = 0;
  const stop = async () => {
    await end();
    away = net.createServer(socket => {
      turnedAway += 1;
      socket.on('error', () => undefined);
      socket.destroy();
    });
    await new Promise((resolve, reject) => away.once('error', reject).listen(port, host, resolve));
  };
  const start = async () => {
    await closed(away);
    await listen(port);
  };
  return {
    host,
    port,
    log,
    connects: clientId => log.filter(entry => entry.type === 'connect' && entry.accepted && entry.clientId === clientId).length,
    messages: topic => log.filter(entry => entry.type === 'message' && entry.topic === topic),
    retained: () => Object.fromEntries(kept),
    subscriptions: clientId => {
      const session = [...sessions].find(one => one.clientId === clientId);
      return session && Object.fromEntries(session.subscriptions);
    },
    publish: (topic, payload) => route({ topic, payload, qos: 1, retain: false, clientId: null, origin: 'broker' }),
    drop: clientId => {
      const session = [...sessions].find(one => one.clientId === clientId);
      session?.socket.destroy();
      return Boolean(session);
    },
    stop,
    turnedAway: () => turnedAway,
    start,
    close: () => Promise.all([end(), closed(away)]),
  };
}
