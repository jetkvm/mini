#pragma once
#define OPENSSL_SUPPRESS_DEPRECATED
#include <openssl/sha.h>
#include <openssl/crypto.h>
#include <string.h>
typedef SHA256_CTX mbedtls_sha256_context;
static inline void mbedtls_sha256_init(mbedtls_sha256_context *ctx) { memset(ctx, 0, sizeof(*ctx)); }
static inline void mbedtls_sha256_free(mbedtls_sha256_context *ctx) { OPENSSL_cleanse(ctx, sizeof(*ctx)); }
static inline void mbedtls_sha256_clone(mbedtls_sha256_context *dst, const mbedtls_sha256_context *src) { *dst = *src; }
static inline int mbedtls_sha256_starts(mbedtls_sha256_context *ctx, int is224) { return is224 || !SHA256_Init(ctx); }
static inline int mbedtls_sha256_update(mbedtls_sha256_context *ctx, const unsigned char *input, size_t size) { return !SHA256_Update(ctx, input, size); }
static inline int mbedtls_sha256_finish(mbedtls_sha256_context *ctx, unsigned char *output) { return !SHA256_Final(output, ctx); }
