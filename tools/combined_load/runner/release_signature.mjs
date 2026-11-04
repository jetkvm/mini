// The signature of a firmware image as a release has it: a detached OpenPGP
// signature (RFC 9580, version 4) by an Ed25519 signing subkey, in binary
// form, as `gpg --detach-sign` writes it. The Mini refuses an update without
// one, so the tool's update server signs the image that it offers, with the
// test release key, which only a bench build trusts
// (components/jetkvm_ota/test_key).
import { createHash, createPrivateKey, sign } from 'node:crypto';
import fs from 'node:fs';

const KEY_DIRECTORY = new URL('../../../components/jetkvm_ota/test_key/', import.meta.url);

/**
 * The test release key: rootFingerprint (the key that a bench build trusts),
 * signingFingerprint and signingPrivateKey (its signing subkey, hex), and
 * publicKey, the armored key as a keyserver serves it.
 */
export function testReleaseKey() {
  return {
    ...JSON.parse(fs.readFileSync(new URL('test-release-key.json', KEY_DIRECTORY), 'utf8')),
    publicKey: fs.readFileSync(new URL('test-release-key.asc', KEY_DIRECTORY)),
  };
}

// A multiprecision integer (3.2): the number of bits, then the octets without
// leading zeros.
function mpi(octets) {
  let start = 0;
  while (start < octets.length - 1 && octets[start] === 0) start++;
  const value = octets.subarray(start);
  const bits = value.length * 8 - Math.clz32(value[0]) + 24;
  return Buffer.concat([Buffer.from([bits >> 8, bits & 0xff]), value]);
}

const subpacket = (type, value) => Buffer.concat([Buffer.from([value.length + 1, type]), value]);

/**
 * The detached signature of `data` by the Ed25519 key with this fingerprint
 * (40 hex digits) and private key (64 hex digits), over SHA-512, made at
 * `createdSeconds` (seconds since 1970).
 */
export function signDetached(data, { signingFingerprint, signingPrivateKey }, createdSeconds = Math.floor(Date.now() / 1000)) {
  const fingerprint = Buffer.from(signingFingerprint, 'hex');
  const created = Buffer.alloc(4);
  created.writeUInt32BE(createdSeconds);
  // Hashed: the issuer's fingerprint (33) and the creation time (2).
  const hashed = Buffer.concat([subpacket(33, Buffer.concat([Buffer.from([4]), fingerprint])), subpacket(2, created)]);
  // Not hashed: the issuer's key ID (16), the fingerprint's last 8 octets.
  const unhashed = subpacket(16, fingerprint.subarray(12));
  const length = n => Buffer.from([n >> 8, n & 0xff]);
  // Version 4, a binary document (0x00), EdDSA (22), SHA-512 (10).
  const head = Buffer.concat([Buffer.from([4, 0x00, 22, 10]), length(hashed.length), hashed]);
  const trailer = Buffer.alloc(6);
  trailer.writeUInt8(4, 0);
  trailer.writeUInt8(0xff, 1);
  trailer.writeUInt32BE(head.length, 2);
  const digest = createHash('sha512').update(data).update(head).update(trailer).digest();
  // The key in PKCS #8: a fixed prefix for Ed25519, then the 32 octets.
  const key = createPrivateKey({
    key: Buffer.concat([Buffer.from('302e020100300506032b657004220420', 'hex'), Buffer.from(signingPrivateKey, 'hex')]),
    format: 'der', type: 'pkcs8',
  });
  const signature = sign(null, digest, key);
  const body = Buffer.concat([head, length(unhashed.length), unhashed, digest.subarray(0, 2),
    mpi(signature.subarray(0, 32)), mpi(signature.subarray(32))]);
  // A new-format packet header: tag 2, and the one-octet or two-octet length.
  const size = body.length < 192 ? Buffer.from([body.length])
    : Buffer.from([((body.length - 192) >> 8) + 192, (body.length - 192) & 0xff]);
  return Buffer.concat([Buffer.from([0xc0 | 2]), size, body]);
}
