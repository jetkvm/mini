// The bytes that the tool sends through the Mini, and their checksums. Every
// byte comes from a generator with a seed, so that the receiving end can tell
// a file or a block that is not the one that was sent: one with bytes of
// another place, of another file, or of an earlier run. Content that repeats
// after a few bytes would have the same checksum when a part of it lands at
// a wrong offset.
import { createHash } from 'node:crypto';

/**
 * `length` bytes that follow from `seed`, a whole number: a xorshift
 * generator of 32 bits, of which each step gives one byte. The function
 * uses nothing outside itself, so the tool's page can run its text too.
 */
export const seededBytes = (seed, length) => {
  const bytes = new Uint8Array(length);
  // Never 0, at which the generator would stay.
  let x = (Math.imul(seed + 1, 0x9e3779b1) >>> 0) || 0x9e3779b1;
  for (let i = 0; i < length; i++) {
    x ^= x << 13;
    x ^= x >>> 17;
    x ^= x << 5;
    x >>>= 0;
    bytes[i] = x & 0xff;
  }
  return bytes;
};

export const sha256 = bytes => createHash('sha256').update(bytes).digest('hex');

// An image that the Mini mounts is a row of blocks of this size, each with
// content of its own, so that the host can check each block that it has
// read without the rest of the image.
export const IMAGE_BLOCK_BYTES = 1024 * 1024;

/**
 * An image of `blocks` blocks. Block n has the bytes of the seed
 * `seed + n`: two images with seeds that are further apart than their
 * length share no block.
 */
export function imageBytes(seed, blocks) {
  const image = Buffer.alloc(blocks * IMAGE_BLOCK_BYTES);
  for (let n = 0; n < blocks; n++) image.set(seededBytes(seed + n, IMAGE_BLOCK_BYTES), n * IMAGE_BLOCK_BYTES);
  return image;
}

/** The SHA-256 of each block of an image, as the host's reader gets them (hosts.mjs). */
export function blockChecksums(image) {
  const sums = [];
  for (let at = 0; at + IMAGE_BLOCK_BYTES <= image.length; at += IMAGE_BLOCK_BYTES) {
    sums.push(sha256(image.subarray(at, at + IMAGE_BLOCK_BYTES)));
  }
  return sums;
}
