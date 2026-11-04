#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define JETKVM_PASSWORD_ITERATIONS 200000U
#define JETKVM_PASSWORD_RECORD_MAGIC UINT32_C(0x31414b4a)
typedef struct {
    uint32_t magic, iterations;
    uint8_t enabled;
    uint8_t salt[16], hash[32];
} jetkvm_password_record_t;

typedef struct {
    bool (*derive)(const char *, const uint8_t salt[16], uint32_t iterations, uint8_t hash[32], void *);
    bool (*random)(uint8_t *, size_t, void *);
    bool (*save)(const jetkvm_password_record_t *, void *);
    void *ctx;
} jetkvm_password_backend_t;

typedef enum {
    JETKVM_PASSWORD_CREATE,
    JETKVM_PASSWORD_UPDATE,
    JETKVM_PASSWORD_DISABLE,
} jetkvm_password_action_t;
typedef enum {
    JETKVM_PASSWORD_OK,
    JETKVM_PASSWORD_INVALID,
    JETKVM_PASSWORD_WRONG_MODE,
    JETKVM_PASSWORD_DENIED,
    JETKVM_PASSWORD_STORAGE_ERROR,
    JETKVM_PASSWORD_CRYPTO_ERROR,
} jetkvm_password_result_t;

void jetkvm_password_defaults(jetkvm_password_record_t *);
bool jetkvm_password_record_valid(const jetkvm_password_record_t *);
bool jetkvm_password_equal(const void *, const void *, size_t);
jetkvm_password_result_t jetkvm_password_verify(const jetkvm_password_record_t *, const char *, const jetkvm_password_backend_t *);
jetkvm_password_result_t jetkvm_password_change(jetkvm_password_record_t *, jetkvm_password_action_t,
    const char *old_password, const char *new_password, const jetkvm_password_backend_t *);
