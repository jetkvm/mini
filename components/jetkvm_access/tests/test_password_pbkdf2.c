#include "password_pbkdf2.h"
#include "sha/sha_core.h"
#include <assert.h>
#include <openssl/evp.h>
#include <string.h>
#include <stdio.h>
SHA256_CTX test_sha_state;
bool test_sha_locked;
unsigned test_sha_blocks, test_sha_max_blocks;
static unsigned yields;
static void yield(void) { assert(!test_sha_locked); ++yields; }
int main(void)
{
    const char *passwords[] = {"", "password", "literal\\u0000password",
        "012345678901234567890123456789012345678901234567890123456789012345678901"};
    const unsigned costs[] = {1, 2, 31, 32, 33, 4096, 4097, 200000};
    uint8_t salt[16], expected[32], storage[33];
    uint8_t *actual = storage + 1; /* Real password records need not align the hash. */
    for (unsigned i = 0; i < sizeof(salt); ++i) salt[i] = i;
    for (unsigned p = 0; p < 4; ++p) for (unsigned c = 0; c < sizeof(costs)/sizeof(costs[0]); ++c) {
        yields = 0;
        assert(PKCS5_PBKDF2_HMAC(passwords[p], strlen(passwords[p]), salt, sizeof(salt), costs[c], EVP_sha256(), sizeof(expected), expected));
        assert(jetkvm_password_pbkdf2(passwords[p], salt, costs[c], actual, yield));
        assert(!memcmp(expected, actual, 32));
        assert(!test_sha_locked && test_sha_max_blocks <= 64);
        assert(yields == (costs[c] - 1)/4096);
    }
    assert(!jetkvm_password_pbkdf2("password", salt, 0, actual, yield));
    puts("Batched hardware PBKDF2 matches OpenSSL; shared-engine lock and yields bounded: OK");
}
