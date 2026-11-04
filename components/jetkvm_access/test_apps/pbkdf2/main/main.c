#include "password_pbkdf2.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include <stdatomic.h>
#include "psa/crypto.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static atomic_bool stop_competitor;
static TaskHandle_t bench_task;
static unsigned competing_hashes;
static int64_t max_competing_us;
static void competing_sha(void *argument)
{
    (void)argument;
    const uint8_t expected[32] = {
        0xba,0x78,0x16,0xbf,0x8f,0x01,0xcf,0xea,0x41,0x41,0x40,0xde,0x5d,0xae,0x22,0x23,
        0xb0,0x03,0x61,0xa3,0x96,0x17,0x7a,0x9c,0xb4,0x10,0xff,0x61,0xf2,0x00,0x15,0xad};
    while (!atomic_load(&stop_competitor)) {
        uint8_t output[32]; size_t written;
        int64_t started = esp_timer_get_time();
        assert(psa_hash_compute(PSA_ALG_SHA_256, (const uint8_t *)"abc", 3,
                                output, sizeof(output), &written) == PSA_SUCCESS);
        int64_t elapsed = esp_timer_get_time() - started;
        if (elapsed > max_competing_us) max_competing_us = elapsed;
        assert(written == 32 && !memcmp(output, expected, 32));
        ++competing_hashes;
        vTaskDelay(1);
    }
    xTaskNotifyGive(bench_task);
    vTaskDelete(NULL);
}
static void cooperate(void) { vTaskDelay(1); }
void app_main(void)
{
    /* Allows a serial reader to attach after flashing. No NVS or user storage
     * is initialized or written by this standalone test application. */
    vTaskDelay(pdMS_TO_TICKS(2000));
    assert(psa_crypto_init() == PSA_SUCCESS);
    bench_task = xTaskGetCurrentTaskHandle();
    assert(xTaskCreate(competing_sha, "sha_competitor", 4096, NULL, 2, NULL) == pdPASS);
    const struct { const char *password; uint8_t expected[32]; } vectors[] = {
    {"password", {0x58, 0x1f, 0xe8, 0x32, 0x7e, 0xbf, 0x9c, 0x19, 0x1e, 0xd4, 0x3b, 0x9a, 0xec, 0x17, 0xf8, 0x21, 0x76, 0x2c, 0x0a, 0x24, 0xaa, 0x35, 0xa9, 0x33, 0xc8, 0xdc, 0x64, 0x9c, 0xdc, 0x60, 0xfe, 0xee}},
    {"012345678901234567890123456789012345678901234567890123456789012345678901", {0x1a, 0x1a, 0xf2, 0xbb, 0x47, 0x98, 0xbc, 0x3b, 0x27, 0xa5, 0xae, 0x71, 0x81, 0xb4, 0x2d, 0x8b, 0xc0, 0x10, 0x8a, 0xf6, 0xa8, 0xaa, 0x86, 0x38, 0x3b, 0x4a, 0xdc, 0x02, 0x02, 0x7d, 0x6f, 0x6c}},
    };
    uint8_t salt[16], output[33];
    for (unsigned i = 0; i < sizeof(salt); ++i) salt[i] = i;
    for (unsigned i = 0; i < sizeof(vectors)/sizeof(vectors[0]); ++i) {
        int64_t started = esp_timer_get_time();
        assert(jetkvm_password_pbkdf2(vectors[i].password, salt, 200000, output + 1, cooperate));
        assert(memcmp(output + 1, vectors[i].expected, 32) == 0);
        printf("PBKDF2 hardware vector %u: PASS, 200000 iterations in %lld ms\n", i,
               (long long)((esp_timer_get_time() - started) / 1000));
    }
    atomic_store(&stop_competitor, true);
    ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
    assert(competing_hashes > 0);
    printf("Competing SHA: %u correct hashes, maximum latency %lld us\n",
           competing_hashes, (long long)max_competing_us);
    puts("PBKDF2 HARDWARE VALIDATION PASSED");
}
