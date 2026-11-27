// A stand-in TURN server (RFC 8656 over UDP, IPv4) on this machine's address
// towards the Mini: enough for a browser to get a relay address and to send
// and receive through it. It also answers STUN Binding requests, so the Mini
// can learn its server-reflexive address from it.
import { createHash, createHmac, randomBytes } from 'node:crypto';
import dgram from 'node:dgram';
import { addressTowards } from '../../combined_load/runner/switches.mjs';

const COOKIE = 0x2112a442;
export const METHOD = { binding: 0x001, allocate: 0x003, refresh: 0x004, send: 0x006, data: 0x007, createPermission: 0x008, channelBind: 0x009 };
export const CLASS = { request: 0x000, indication: 0x010, success: 0x100, error: 0x110 };
export const ATTRIBUTE = {
  mappedAddress: 0x0001, username: 0x0006, messageIntegrity: 0x0008, errorCode: 0x0009, channelNumber: 0x000c,
  lifetime: 0x000d, xorPeerAddress: 0x0012, data: 0x0013, realm: 0x0014, nonce: 0x0015, xorRelayedAddress: 0x0016,
  xorMappedAddress: 0x0020,
};
const REALM = 'integration.test';
const LIFETIME_S = 600;

const key = ({ address, port }) => `${address}:${port}`;

// An IPv4 address and port as STUN writes them, XORed with the magic cookie or not.
export function addressValue({ address, port }, xor) {
  const value = Buffer.alloc(8);
  value[1] = 0x01;
  value.writeUInt16BE(xor ? port ^ (COOKIE >>> 16) : port, 2);
  const number = address.split('.').reduce((sum, part) => sum * 256 + Number(part), 0);
  value.writeUInt32BE(xor ? (number ^ COOKIE) >>> 0 : number, 4);
  return value;
}

export function xorAddress(value) {
  if (value.length < 8 || value[1] !== 0x01) return null;
  const number = (value.readUInt32BE(4) ^ COOKIE) >>> 0;
  return { address: [24, 16, 8, 0].map(shift => (number >>> shift) & 0xff).join('.'), port: value.readUInt16BE(2) ^ (COOKIE >>> 16) };
}

// A STUN message's attributes by type, and where MESSAGE-INTEGRITY starts.
export function parse(message) {
  const attributes = new Map();
  let integrityAt = null;
  for (let at = 20; at + 4 <= message.length;) {
    const type = message.readUInt16BE(at);
    const length = message.readUInt16BE(at + 2);
    if (at + 4 + length > message.length) break;
    if (type === ATTRIBUTE.messageIntegrity) integrityAt = at;
    if (!attributes.has(type)) attributes.set(type, message.subarray(at + 4, at + 4 + length));
    at += 4 + length + ((4 - (length % 4)) % 4);
  }
  return { attributes, integrityAt };
}

// The HMAC-SHA1 of a message up to its MESSAGE-INTEGRITY attribute at `end`,
// with the length field that counts that attribute.
export function integrity(message, end, secret) {
  const covered = Buffer.from(message.subarray(0, end));
  covered.writeUInt16BE(end - 20 + 24, 2);
  return createHmac('sha1', secret).update(covered).digest();
}

export function build(type, transaction, attributes, secret) {
  const parts = [];
  for (const [attribute, value] of attributes) {
    const head = Buffer.alloc(4);
    head.writeUInt16BE(attribute, 0);
    head.writeUInt16BE(value.length, 2);
    parts.push(head, value, Buffer.alloc((4 - (value.length % 4)) % 4));
  }
  const header = Buffer.alloc(20);
  header.writeUInt16BE(type, 0);
  header.writeUInt32BE(COOKIE, 4);
  transaction.copy(header, 8);
  let message = Buffer.concat([header, ...parts]);
  message.writeUInt16BE(message.length - 20, 2);
  if (secret) {
    const head = Buffer.from([0, ATTRIBUTE.messageIntegrity, 0, 20]);
    message = Buffer.concat([message, head, integrity(message, message.length, secret)]);
    message.writeUInt16BE(message.length - 20, 2);
  }
  return message;
}

const errorCode = (code, reason) => Buffer.concat([Buffer.from([0, 0, Math.floor(code / 100), code % 100]), Buffer.from(reason)]);
const uint32 = value => {
  const buffer = Buffer.alloc(4);
  buffer.writeUInt32BE(value);
  return buffer;
};

/**
 * Starts the relay for the bench's Mini. With `peerFirst`, the relay sends a
 * client's data to a peer address only after that address has sent to the
 * relay address, and drops it before. That is what a client sees when a
 * firewall or NAT in front of the peer lets a packet in only from an address
 * that the peer has sent to.
 *
 * Returns { iceServer, stunUrl, counts(), close }: `iceServer` for a
 * browser's RTCPeerConnection (with the login), `stunUrl` for the Mini, and
 * counts() with the packets toPeers and fromPeers that the relay passed, the
 * packets that it held back from a peer that had not sent yet, and the peer
 * addresses that sent to a relay address.
 */
export async function turnRelay(mini, { peerFirst = false } = {}) {
  const address = await addressTowards(new URL(mini.url).hostname);
  const username = 'integration';
  const credential = randomBytes(12).toString('hex');
  const secret = createHash('md5').update(`${username}:${REALM}:${credential}`).digest();
  const nonce = randomBytes(8).toString('hex');
  const allocations = new Map();   // by the client's address and port
  const counts = { toPeers: 0, fromPeers: 0, held: 0, peers: new Set() };
  const server = dgram.createSocket('udp4');
  await new Promise((resolve, reject) => {
    server.once('error', reject);
    server.bind(0, address, resolve);
  });

  const toPeer = (allocation, peer, data) => {
    if (peerFirst && !allocation.heard.has(key(peer))) {
      counts.held++;
      return;
    }
    counts.toPeers++;
    allocation.relay.send(data, peer.port, peer.address);
  };

  // A client's allocation, made by the Allocate request of `transaction`:
  // its relay socket, and `ready` once that socket has its port.
  const allocate = (client, transaction) => {
    const relay = dgram.createSocket('udp4');
    const allocation = { relay, transaction: transaction.toString('hex'), heard: new Set(), channels: new Map(), numbers: new Map() };
    allocation.ready = new Promise((resolve, reject) => {
      relay.once('error', reject);
      relay.bind(0, address, resolve);
    });
    relay.on('error', () => {});
    relay.on('message', (data, peer) => {
      allocation.heard.add(key(peer));
      counts.peers.add(key(peer));
      counts.fromPeers++;
      const channel = allocation.numbers.get(key(peer));
      if (channel !== undefined) {
        const head = Buffer.alloc(4);
        head.writeUInt16BE(channel, 0);
        head.writeUInt16BE(data.length, 2);
        server.send(Buffer.concat([head, data]), client.port, client.address);
      } else {
        server.send(build(METHOD.data | CLASS.indication, randomBytes(12),
          [[ATTRIBUTE.xorPeerAddress, addressValue(peer, true)], [ATTRIBUTE.data, data]]), client.port, client.address);
      }
    });
    return allocation;
  };

  server.on('message', async (message, client) => {
    if (message.length < 4) return;
    const reply = bytes => server.send(bytes, client.port, client.address);
    const allocation = allocations.get(key(client));
    // ChannelData: a channel number from 0x4000, a length and the data.
    if ((message[0] & 0xc0) === 0x40) {
      const peer = allocation?.channels.get(message.readUInt16BE(0));
      if (peer) toPeer(allocation, peer, message.subarray(4, 4 + message.readUInt16BE(2)));
      return;
    }
    if (message.length < 20 || message.readUInt32BE(4) !== COOKIE) return;
    const type = message.readUInt16BE(0);
    const method = type & 0x3eef;
    const kind = type & 0x0110;
    const transaction = message.subarray(8, 20);
    const { attributes, integrityAt } = parse(message);
    const peer = attributes.has(ATTRIBUTE.xorPeerAddress) ? xorAddress(attributes.get(ATTRIBUTE.xorPeerAddress)) : null;

    if (method === METHOD.binding && kind === CLASS.request) {
      return reply(build(METHOD.binding | CLASS.success, transaction,
        [[ATTRIBUTE.xorMappedAddress, addressValue(client, true)], [ATTRIBUTE.mappedAddress, addressValue(client, false)]]));
    }
    if (method === METHOD.send && kind === CLASS.indication) {
      if (allocation && peer && attributes.has(ATTRIBUTE.data)) toPeer(allocation, peer, attributes.get(ATTRIBUTE.data));
      return;
    }
    if (kind !== CLASS.request) return;
    // A request needs the login: the first Allocate comes without one.
    const signed = integrityAt !== null && attributes.get(ATTRIBUTE.username)?.toString() === username &&
      attributes.get(ATTRIBUTE.messageIntegrity).equals(integrity(message, integrityAt, secret));
    if (!signed) {
      return reply(build(method | CLASS.error, transaction, [[ATTRIBUTE.errorCode, errorCode(401, 'Unauthorized')],
        [ATTRIBUTE.realm, Buffer.from(REALM)], [ATTRIBUTE.nonce, Buffer.from(nonce)]]));
    }
    const answer = (extra = []) => reply(build(method | CLASS.success, transaction, extra, secret));
    const refuse = (code, reason) => reply(build(method | CLASS.error, transaction, [[ATTRIBUTE.errorCode, errorCode(code, reason)]], secret));
    if (method === METHOD.allocate) {
      // A client sends its Allocate again when the answer is late or lost.
      // The same transaction gets the answer of the first time; another
      // transaction of a client with an allocation is refused.
      if (allocation && allocation.transaction !== transaction.toString('hex')) return refuse(437, 'Allocation Mismatch');
      const made = allocation ?? allocate(client, transaction);
      allocations.set(key(client), made);
      try {
        await made.ready;
      } catch {
        allocations.delete(key(client));
        return refuse(508, 'Insufficient Capacity');
      }
      return answer([[ATTRIBUTE.xorRelayedAddress, addressValue(made.relay.address(), true)],
        [ATTRIBUTE.lifetime, uint32(LIFETIME_S)], [ATTRIBUTE.xorMappedAddress, addressValue(client, true)]]);
    }
    if (!allocation) return refuse(437, 'Allocation Mismatch');
    if (method === METHOD.refresh) {
      const lifetime = attributes.get(ATTRIBUTE.lifetime)?.readUInt32BE(0) ?? LIFETIME_S;
      if (lifetime === 0) {
        allocation.relay.close();
        allocations.delete(key(client));
      }
      return answer([[ATTRIBUTE.lifetime, uint32(Math.min(lifetime, LIFETIME_S))]]);
    }
    if (method === METHOD.createPermission) return answer();
    if (method === METHOD.channelBind) {
      const number = attributes.get(ATTRIBUTE.channelNumber)?.readUInt16BE(0);
      if (!peer || number === undefined) return refuse(400, 'Bad Request');
      allocation.channels.set(number, peer);
      allocation.numbers.set(key(peer), number);
      return answer();
    }
    return refuse(400, 'Bad Request');
  });

  const port = server.address().port;
  return {
    iceServer: { urls: `turn:${address}:${port}?transport=udp`, username, credential },
    stunUrl: `stun:${address}:${port}`,
    counts: () => ({ toPeers: counts.toPeers, fromPeers: counts.fromPeers, held: counts.held, peers: [...counts.peers] }),
    close: () => {
      for (const { relay } of allocations.values()) relay.close();
      allocations.clear();
      server.close();
    },
  };
}
