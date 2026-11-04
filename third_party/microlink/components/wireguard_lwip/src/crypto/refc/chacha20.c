/*
 * Copyright (c) 2021 Daniel Hope (www.floorsense.nz)
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without modification,
 * are permitted provided that the following conditions are met:
 *
 * 1. Redistributions of source code must retain the above copyright notice, this
 *  list of conditions and the following disclaimer.
 *
 * 2. Redistributions in binary form must reproduce the above copyright notice, this
 *  list of conditions and the following disclaimer in the documentation and/or
 *  other materials provided with the distribution.
 *
 * 3. Neither the name of "Floorsense Ltd", "Agile Workspace Ltd" nor the names of
 *  its contributors may be used to endorse or promote products derived from this
 *   software without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS" AND
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED
 * WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
 * DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT OWNER OR CONTRIBUTORS BE LIABLE FOR
 * ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES
 * (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES;
 * LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON
 * ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS
 * SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 *
 * Author: Daniel Hope <daniel.hope@smartalock.com>
 */

// RFC7539 implementation of ChaCha20 with modified nonce size for WireGuard
// https://tools.ietf.org/html/rfc7539
// Adapted from https://cr.yp.to/streamciphers/timings/estreambench/submissions/salsa20/chacha8/ref/chacha.c by D. J. Bernstein (Public Domain)
// HChaCha20 is described here: https://tools.ietf.org/id/draft-arciszewski-xchacha-02.html

#include "chacha20.h"

#include <string.h>
#include <stdint.h>
#include "../../crypto.h"

// 2.3.  The ChaCha20 Block Function
// The first four words (0-3) are constants: 0x61707865, 0x3320646e, 0x79622d32, 0x6b206574
static const uint32_t CHACHA20_CONSTANT_1 = 0x61707865;
static const uint32_t CHACHA20_CONSTANT_2 = 0x3320646e;
static const uint32_t CHACHA20_CONSTANT_3 = 0x79622d32;
static const uint32_t CHACHA20_CONSTANT_4 = 0x6b206574;

#define ROTL32(v, n) (U32V((v) << (n)) | ((v) >> (32 - (n))))

#define PLUS(v,w) (U32V((v) + (w)))
#define PLUSONE(v) (PLUS((v),1))

// 2.1. The ChaCha Quarter Round
// 1.  a += b; d ^= a; d <<<= 16;
// 2.  c += d; b ^= c; b <<<= 12;
// 3.  a += b; d ^= a; d <<<= 8;
// 4.  c += d; b ^= c; b <<<= 7;

#define QUARTERROUND(a, b, c, d)       \
    a += b;  d ^= a;  d = ROTL32(d, 16);  \
    c += d;  b ^= c;  b = ROTL32(b, 12);  \
    a += b;  d ^= a;  d = ROTL32(d,  8);  \
    c += d;  b ^= c;  b = ROTL32(b,  7)

static inline void INNER_BLOCK(uint32_t *block) {
	QUARTERROUND(block[0], block[4], block[ 8], block[12]); // column 0
	QUARTERROUND(block[1], block[5], block[ 9], block[13]); // column 1
	QUARTERROUND(block[2], block[6], block[10], block[14]); // column 2
	QUARTERROUND(block[3], block[7], block[11], block[15]); // column 3
	QUARTERROUND(block[0], block[5], block[10], block[15]); // diagonal 1
	QUARTERROUND(block[1], block[6], block[11], block[12]); // diagonal 2
	QUARTERROUND(block[2], block[7], block[ 8], block[13]); // diagonal 3
	QUARTERROUND(block[3], block[4], block[ 9], block[14]); // diagonal 4
}

#define TWENTY_ROUNDS(x) ( \
	INNER_BLOCK(x), \
	INNER_BLOCK(x), \
	INNER_BLOCK(x), \
	INNER_BLOCK(x), \
	INNER_BLOCK(x), \
	INNER_BLOCK(x), \
	INNER_BLOCK(x), \
	INNER_BLOCK(x), \
	INNER_BLOCK(x), \
	INNER_BLOCK(x) \
)

// 2.3.  The ChaCha20 Block Function
// The working state stays in sixteen local words so that the compiler keeps
// it in registers through the twenty rounds; the result is the keystream as
// words, which the caller XORs into the data a word at a time.
#define QR(a, b, c, d) \
	a += b; d ^= a; d = ROTL32(d, 16); \
	c += d; b ^= c; b = ROTL32(b, 12); \
	a += b; d ^= a; d = ROTL32(d,  8); \
	c += d; b ^= c; b = ROTL32(b,  7)

static void chacha20_block_words(const uint32_t *s, uint32_t *ks) {
	uint32_t x0 = s[0], x1 = s[1], x2 = s[2], x3 = s[3];
	uint32_t x4 = s[4], x5 = s[5], x6 = s[6], x7 = s[7];
	uint32_t x8 = s[8], x9 = s[9], x10 = s[10], x11 = s[11];
	uint32_t x12 = s[12], x13 = s[13], x14 = s[14], x15 = s[15];
	for (int i = 0; i < 10; ++i) {
		QR(x0, x4, x8, x12);
		QR(x1, x5, x9, x13);
		QR(x2, x6, x10, x14);
		QR(x3, x7, x11, x15);
		QR(x0, x5, x10, x15);
		QR(x1, x6, x11, x12);
		QR(x2, x7, x8, x13);
		QR(x3, x4, x9, x14);
	}
	ks[0] = x0 + s[0];   ks[1] = x1 + s[1];   ks[2] = x2 + s[2];   ks[3] = x3 + s[3];
	ks[4] = x4 + s[4];   ks[5] = x5 + s[5];   ks[6] = x6 + s[6];   ks[7] = x7 + s[7];
	ks[8] = x8 + s[8];   ks[9] = x9 + s[9];   ks[10] = x10 + s[10]; ks[11] = x11 + s[11];
	ks[12] = x12 + s[12]; ks[13] = x13 + s[13]; ks[14] = x14 + s[14]; ks[15] = x15 + s[15];
}

#if defined(__BYTE_ORDER__) && (__BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__)
#define CHACHA20_WORD_XOR 1
#else
#define CHACHA20_WORD_XOR 0
#endif

void chacha20(struct chacha20_ctx *ctx, uint8_t *out, const uint8_t *in, uint32_t len) {
	uint32_t ks[16];
	uint8_t tail[CHACHA20_BLOCK_SIZE];
	int i;

	while (len >= CHACHA20_BLOCK_SIZE) {
		chacha20_block_words(ctx->state, ks);
		// Word 12 is a block counter
		ctx->state[12] = PLUSONE(ctx->state[12]);
#if CHACHA20_WORD_XOR
		if ((((uintptr_t)in | (uintptr_t)out) & 3) == 0) {
			// A WireGuard packet is encrypted in place at a word-aligned offset,
			// so the keystream is applied a word at a time.
			const uint8_t *ia = __builtin_assume_aligned(in, 4);
			uint8_t *oa = __builtin_assume_aligned(out, 4);
			for (i = 0; i < 16; ++i) {
				uint32_t w;
				memcpy(&w, ia + 4 * i, 4);
				w ^= ks[i];
				memcpy(oa + 4 * i, &w, 4);
			}
		} else
#endif
		{
			for (i = 0; i < 16; ++i) {
				uint32_t w = U8TO32_LITTLE(in + 4 * i) ^ ks[i];
				U32TO8_LITTLE(out + 4 * i, w);
			}
		}
		len -= CHACHA20_BLOCK_SIZE;
		out += CHACHA20_BLOCK_SIZE;
		in += CHACHA20_BLOCK_SIZE;
	}
	if (len) {
		chacha20_block_words(ctx->state, ks);
		ctx->state[12] = PLUSONE(ctx->state[12]);
		for (i = 0; i < 16; ++i) {
			U32TO8_LITTLE(tail + 4 * i, ks[i]);
		}
		for (i = 0; i < (int)len; ++i) {
			out[i] = in[i] ^ tail[i];
		}
		crypto_zero(tail, sizeof(tail));
	}
	crypto_zero(ks, sizeof(ks));
}


// 2.3.  The ChaCha20 Block Function
// The first four words (0-3) are constants: 0x61707865, 0x3320646e, 0x79622d32, 0x6b206574
// The next eight words (4-11) are taken from the 256-bit key by reading the bytes in little-endian order, in 4-byte chunks.
// Word 12 is a block counter.  Since each block is 64-byte, a 32-bit word is enough for 256 gigabytes of data.
// Words 13-15 are a nonce, which should not be repeated for the same key.
// For wireguard: "nonce being composed of 32 bits of zeros followed by the 64-bit little-endian value of counter." where counter comes from the Wireguard layer and is separate from the block counter in word 12
void chacha20_init(struct chacha20_ctx *ctx, const uint8_t *key, const uint64_t nonce) {
	ctx->state[0] = CHACHA20_CONSTANT_1;
	ctx->state[1] = CHACHA20_CONSTANT_2;
	ctx->state[2] = CHACHA20_CONSTANT_3;
	ctx->state[3] = CHACHA20_CONSTANT_4;
	ctx->state[4] = U8TO32_LITTLE(key + 0);
	ctx->state[5] = U8TO32_LITTLE(key + 4);
	ctx->state[6] = U8TO32_LITTLE(key + 8);
	ctx->state[7] = U8TO32_LITTLE(key + 12);
	ctx->state[8] = U8TO32_LITTLE(key + 16);
	ctx->state[9] = U8TO32_LITTLE(key + 20);
	ctx->state[10] = U8TO32_LITTLE(key + 24);
	ctx->state[11] = U8TO32_LITTLE(key + 28);
	ctx->state[12] = 0;
	ctx->state[13] = 0;
	ctx->state[14] = nonce & 0xFFFFFFFF;
	ctx->state[15] = nonce >> 32;
}

// 2.2. HChaCha20
// HChaCha20 is initialized the same way as the ChaCha cipher, except that HChaCha20 uses a 128-bit nonce and has no counter.
// After initialization, proceed through the ChaCha rounds as usual.
// Once the 20 ChaCha rounds have been completed, the first 128 bits and last 128 bits of the ChaCha state (both little-endian) are concatenated, and this 256-bit subkey is returned.
void hchacha20(uint8_t *out, const uint8_t *nonce, const uint8_t *key) {
	uint32_t state[16];
	state[0] = CHACHA20_CONSTANT_1;
	state[1] = CHACHA20_CONSTANT_2;
	state[2] = CHACHA20_CONSTANT_3;
	state[3] = CHACHA20_CONSTANT_4;
	state[4] = U8TO32_LITTLE(key + 0);
	state[5] = U8TO32_LITTLE(key + 4);
	state[6] = U8TO32_LITTLE(key + 8);
	state[7] = U8TO32_LITTLE(key + 12);
	state[8] = U8TO32_LITTLE(key + 16);
	state[9] = U8TO32_LITTLE(key + 20);
	state[10] = U8TO32_LITTLE(key + 24);
	state[11] = U8TO32_LITTLE(key + 28);
	state[12] = U8TO32_LITTLE(nonce +  0);
	state[13] = U8TO32_LITTLE(nonce +  4);
	state[14] = U8TO32_LITTLE(nonce +  8);
	state[15] = U8TO32_LITTLE(nonce + 12);

	TWENTY_ROUNDS(state);

	// Concatenate first/last 128 bits into 256bit output (as little endian)
	U32TO8_LITTLE(out + 0, state[0]);
	U32TO8_LITTLE(out + 4, state[1]);
	U32TO8_LITTLE(out + 8, state[2]);
	U32TO8_LITTLE(out + 12, state[3]);
	U32TO8_LITTLE(out + 16, state[12]);
	U32TO8_LITTLE(out + 20, state[13]);
	U32TO8_LITTLE(out + 24, state[14]);
	U32TO8_LITTLE(out + 28, state[15]);
}
