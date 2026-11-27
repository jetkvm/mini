// The stand-in TURN server (turn.mjs) against a client and a peer that are
// UDP sockets of this machine. tools/run_all_tests.sh runs this file; it
// needs no Mini.
import assert from 'node:assert/strict';
import { createHash, randomBytes } from 'node:crypto';
import dgram from 'node:dgram';
import { test } from 'node:test';
import { ATTRIBUTE, CLASS, METHOD, addressValue, build, integrity, parse, turnRelay, xorAddress } from './turn.mjs';

// A UDP socket on this machine: its address, and next() with the next packet
// that it gets, or null when none comes in `ms`.
async function socketHere() {
  const socket = dgram.createSocket('udp4');
  const packets = [];
  let waiting = null;
  socket.on('message', (data, from) => {
    if (waiting) waiting({ data, from });
    else packets.push({ data, from });
  });
  await new Promise(resolve => socket.bind(0, '127.0.0.1', resolve));
  const next = (ms = 2000) => new Promise(resolve => {
    if (packets.length) return resolve(packets.shift());
    const timer = setTimeout(() => {
      waiting = null;
      resolve(null);
    }, ms);
    waiting = packet => {
      clearTimeout(timer);
      waiting = null;
      resolve(packet);
    };
  });
  return { socket, address: { address: '127.0.0.1', port: socket.address().port }, next };
}

const errorOf = attributes => {
  const value = attributes.get(ATTRIBUTE.errorCode);
  return value ? value[2] * 100 + value[3] : null;
};

test('the stand-in TURN server gives a relay address to a login, and passes data to a peer that has sent first', async t => {
  const turn = await turnRelay({ url: 'http://127.0.0.1' }, { peerFirst: true });
  const client = await socketHere();
  const peer = await socketHere();
  t.after(() => {
    turn.close();
    client.socket.close();
    peer.socket.close();
  });
  const server = { address: '127.0.0.1', port: Number(new URL(turn.stunUrl.replace('stun:', 'stun://')).port) };
  const send = message => client.socket.send(message, server.port, server.address);
  // One request and its answer: { type, attributes, message }.
  const ask = async (method, attributes = [], { transaction = randomBytes(12), secret } = {}) => {
    send(build(method | CLASS.request, transaction, attributes, secret));
    const { data } = await client.next();
    assert.deepEqual(data.subarray(8, 20), transaction);
    return { type: data.readUInt16BE(0), message: data, ...parse(data) };
  };

  // Without the login: 401 with the realm and a nonce.
  const refused = await ask(METHOD.allocate);
  assert.equal(refused.type, METHOD.allocate | CLASS.error);
  assert.equal(errorOf(refused.attributes), 401);
  const realm = refused.attributes.get(ATTRIBUTE.realm).toString();
  const secret = createHash('md5').update(`${turn.iceServer.username}:${realm}:${turn.iceServer.credential}`).digest();
  const login = [[ATTRIBUTE.username, Buffer.from(turn.iceServer.username)], [ATTRIBUTE.realm, Buffer.from(realm)],
    [ATTRIBUTE.nonce, refused.attributes.get(ATTRIBUTE.nonce)]];
  // With another password: 401 again.
  assert.equal(errorOf((await ask(METHOD.allocate, login, { secret: Buffer.from('another') })).attributes), 401);

  // With the login: a relay address, in an answer that the server signed.
  const transaction = randomBytes(12);
  const allocated = await ask(METHOD.allocate, login, { transaction, secret });
  assert.equal(allocated.type, METHOD.allocate | CLASS.success);
  assert.deepEqual(allocated.attributes.get(ATTRIBUTE.messageIntegrity), integrity(allocated.message, allocated.integrityAt, secret));
  assert.deepEqual(xorAddress(allocated.attributes.get(ATTRIBUTE.xorMappedAddress)), client.address);
  const relay = xorAddress(allocated.attributes.get(ATTRIBUTE.xorRelayedAddress));
  assert.equal(relay.address, '127.0.0.1');
  // The same request again, as a client sends it when the answer is lost: the same answer.
  const again = await ask(METHOD.allocate, login, { transaction, secret });
  assert.equal(again.type, METHOD.allocate | CLASS.success);
  assert.deepEqual(xorAddress(again.attributes.get(ATTRIBUTE.xorRelayedAddress)), relay);
  // A second allocation of the client is refused.
  assert.equal(errorOf((await ask(METHOD.allocate, login, { secret })).attributes), 437);
  assert.equal((await ask(METHOD.createPermission, [...login, [ATTRIBUTE.xorPeerAddress, addressValue(peer.address, true)]], { secret })).type,
    METHOD.createPermission | CLASS.success);

  // Data for a peer that has not sent to the relay address is held back.
  const toPeer = text => build(METHOD.send | CLASS.indication, randomBytes(12),
    [[ATTRIBUTE.xorPeerAddress, addressValue(peer.address, true)], [ATTRIBUTE.data, Buffer.from(text)]]);
  send(toPeer('early'));
  // The server takes its packets in their order, so after this answer it has had the data.
  const binding = await ask(METHOD.binding);
  assert.deepEqual(xorAddress(binding.attributes.get(ATTRIBUTE.xorMappedAddress)), client.address);
  assert.deepEqual(turn.counts(), { toPeers: 0, fromPeers: 0, held: 1, peers: [] });
  assert.equal(await peer.next(200), null);

  // The peer sends to the relay address: the client gets it with the peer's address.
  peer.socket.send('from the peer', relay.port, relay.address);
  const indication = await client.next();
  assert.equal(indication.data.readUInt16BE(0), METHOD.data | CLASS.indication);
  const told = parse(indication.data).attributes;
  assert.deepEqual(xorAddress(told.get(ATTRIBUTE.xorPeerAddress)), peer.address);
  assert.equal(told.get(ATTRIBUTE.data).toString(), 'from the peer');
  // Now the relay passes the client's data, from the relay address.
  send(toPeer('late'));
  const late = await peer.next();
  assert.equal(late.data.toString(), 'late');
  assert.equal(late.from.port, relay.port);

  // With a channel for the peer, the data goes both ways with the channel's 4 bytes before it.
  const channel = Buffer.from([0x40, 0x00, 0, 0]);
  assert.equal((await ask(METHOD.channelBind, [...login, [ATTRIBUTE.channelNumber, channel],
    [ATTRIBUTE.xorPeerAddress, addressValue(peer.address, true)]], { secret })).type, METHOD.channelBind | CLASS.success);
  send(Buffer.concat([Buffer.from([0x40, 0x00, 0, 5]), Buffer.from('by ch')]));
  assert.equal((await peer.next()).data.toString(), 'by ch');
  peer.socket.send('back', relay.port, relay.address);
  assert.deepEqual((await client.next()).data, Buffer.concat([Buffer.from([0x40, 0x00, 0, 4]), Buffer.from('back')]));
  assert.deepEqual(turn.counts(), { toPeers: 2, fromPeers: 2, held: 1, peers: [`127.0.0.1:${peer.address.port}`] });
});
