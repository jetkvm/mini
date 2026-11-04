#include "password_core.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include <openssl/evp.h>
static bool fail_save, fail_crypto;
static unsigned salt_counter;
static jetkvm_password_record_t persisted;
static bool random_bytes(uint8_t *bytes, size_t size, void *ctx)
{
    (void)ctx;
    memset(bytes, ++salt_counter, size);
    return true;
}
static bool derive(const char *password, const uint8_t salt[16], uint32_t iterations,
                   uint8_t hash[32], void *ctx)
{
    (void)ctx;
    return !fail_crypto && PKCS5_PBKDF2_HMAC(password, strlen(password), salt, 16,
        iterations, EVP_sha256(), 32, hash) == 1;
}
static bool save(const jetkvm_password_record_t *record, void *ctx)
{
    (void)ctx;
    if (fail_save) return false;
    persisted = *record;
    return true;
}
int main(void)
{
    jetkvm_password_record_t record;
    jetkvm_password_defaults(&record);
    const jetkvm_password_backend_t backend = {derive, random_bytes, save, NULL};
    assert(jetkvm_password_record_valid(&record));
    assert(jetkvm_password_change(&record,JETKVM_PASSWORD_CREATE,NULL,"short",&backend)==JETKVM_PASSWORD_INVALID);
    assert(jetkvm_password_change(&record,JETKVM_PASSWORD_CREATE,NULL,"original password",&backend)==JETKVM_PASSWORD_OK);
    assert(record.enabled && memcmp(&record,&persisted,sizeof(record))==0);
    assert(jetkvm_password_verify(&record,"original password",&backend)==JETKVM_PASSWORD_OK);
    assert(jetkvm_password_verify(&record,"wrong password",&backend)==JETKVM_PASSWORD_DENIED);
    assert(jetkvm_password_change(&record,JETKVM_PASSWORD_CREATE,NULL,"replacement",&backend)==JETKVM_PASSWORD_WRONG_MODE);
    jetkvm_password_record_t before=record;
    fail_save=true;
    assert(jetkvm_password_change(&record,JETKVM_PASSWORD_UPDATE,"original password","replacement",&backend)==JETKVM_PASSWORD_STORAGE_ERROR);
    assert(memcmp(&record,&before,sizeof(record))==0);
    fail_save=false;fail_crypto=true;
    assert(jetkvm_password_change(&record,JETKVM_PASSWORD_UPDATE,"original password","replacement",&backend)==JETKVM_PASSWORD_CRYPTO_ERROR);
    assert(memcmp(&record,&before,sizeof(record))==0);
    fail_crypto=false;
    assert(jetkvm_password_change(&record,JETKVM_PASSWORD_UPDATE,"original password","replacement",&backend)==JETKVM_PASSWORD_OK);
    assert(memcmp(record.salt,before.salt,16)!=0);
    assert(jetkvm_password_verify(&record,"original password",&backend)==JETKVM_PASSWORD_DENIED);
    assert(jetkvm_password_change(&record,JETKVM_PASSWORD_DISABLE,"wrong password",NULL,&backend)==JETKVM_PASSWORD_DENIED);
    assert(jetkvm_password_change(&record,JETKVM_PASSWORD_DISABLE,"replacement",NULL,&backend)==JETKVM_PASSWORD_OK);
    assert(!record.enabled && jetkvm_password_record_valid(&record));
    record.hash[0]=1;assert(!jetkvm_password_record_valid(&record));
    record=before;record.enabled=2;assert(!jetkvm_password_record_valid(&record));
    record=before;record.iterations=UINT32_MAX;assert(!jetkvm_password_record_valid(&record));
    puts("Password verification, mutation and persistence failures: OK");
}
