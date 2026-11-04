#include "password_core.h"
#include <string.h>

static void wipe(void *memory, size_t size)
{
    volatile uint8_t *bytes = memory;
    while (size--) *bytes++ = 0;
}
void jetkvm_password_defaults(jetkvm_password_record_t *record)
{
    memset(record, 0, sizeof(*record));
    record->magic = JETKVM_PASSWORD_RECORD_MAGIC;
    record->iterations = JETKVM_PASSWORD_ITERATIONS;
}
bool jetkvm_password_equal(const void *left, const void *right, size_t size)
{
    const uint8_t *a = left, *b = right;
    volatile uint8_t difference = 0;
    for (size_t i = 0; i < size; ++i) difference |= a[i] ^ b[i];
    return difference == 0;
}
bool jetkvm_password_record_valid(const jetkvm_password_record_t *record)
{
    if (!record || record->magic != JETKVM_PASSWORD_RECORD_MAGIC ||
        record->iterations != JETKVM_PASSWORD_ITERATIONS || record->enabled > 1) return false;
    if (!record->enabled) {
        const uint8_t zero[32] = {0};
        return jetkvm_password_equal(record->salt, zero, sizeof(record->salt)) &&
               jetkvm_password_equal(record->hash, zero, sizeof(record->hash));
    }
    return true;
}
jetkvm_password_result_t jetkvm_password_verify(const jetkvm_password_record_t *record,
    const char *password, const jetkvm_password_backend_t *backend)
{
    if (!jetkvm_password_record_valid(record)) return JETKVM_PASSWORD_STORAGE_ERROR;
    if (!record->enabled) return JETKVM_PASSWORD_WRONG_MODE;
    if (!password || strlen(password) < 8 || strlen(password) > 72) return JETKVM_PASSWORD_DENIED;
    uint8_t hash[32] = {0};
    bool derived = backend && backend->derive &&
        backend->derive(password, record->salt, record->iterations, hash, backend->ctx);
    bool equal = derived && jetkvm_password_equal(hash, record->hash, sizeof(hash));
    wipe(hash, sizeof(hash));
    return !derived ? JETKVM_PASSWORD_CRYPTO_ERROR : equal ? JETKVM_PASSWORD_OK : JETKVM_PASSWORD_DENIED;
}
jetkvm_password_result_t jetkvm_password_change(jetkvm_password_record_t *current,
    jetkvm_password_action_t action, const char *old_password, const char *new_password,
    const jetkvm_password_backend_t *backend)
{
    if (!jetkvm_password_record_valid(current)) return JETKVM_PASSWORD_STORAGE_ERROR;
    if (!backend || !backend->save || action > JETKVM_PASSWORD_DISABLE) return JETKVM_PASSWORD_INVALID;
    if ((action == JETKVM_PASSWORD_CREATE) == (current->enabled != 0)) return JETKVM_PASSWORD_WRONG_MODE;
    if (action != JETKVM_PASSWORD_DISABLE &&
        (!new_password || strlen(new_password) < 8 || strlen(new_password) > 72)) return JETKVM_PASSWORD_INVALID;
    if (action != JETKVM_PASSWORD_CREATE) {
        jetkvm_password_result_t verified = jetkvm_password_verify(current, old_password, backend);
        if (verified != JETKVM_PASSWORD_OK) return verified;
    }
    jetkvm_password_record_t next;
    jetkvm_password_defaults(&next);
    if (action != JETKVM_PASSWORD_DISABLE) {
        next.enabled = 1;
        if (!backend->random || !backend->derive ||
            !backend->random(next.salt, sizeof(next.salt), backend->ctx) ||
            !backend->derive(new_password, next.salt, next.iterations, next.hash, backend->ctx)) {
            wipe(&next, sizeof(next));
            return JETKVM_PASSWORD_CRYPTO_ERROR;
        }
    }
    bool saved = backend->save(&next, backend->ctx);
    if (saved) *current = next;
    wipe(&next, sizeof(next));
    return saved ? JETKVM_PASSWORD_OK : JETKVM_PASSWORD_STORAGE_ERROR;
}
