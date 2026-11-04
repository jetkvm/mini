/*
 * SPDX-FileCopyrightText: 2026
 * SPDX-License-Identifier: MIT
 */

#include "jetkvm_virtual_media.h"

#include <inttypes.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "driver/gpio.h"
#include "driver/sdmmc_host.h"
#include "builtin_image.h"
#include "esp_check.h"
#include "esp_attr.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_random.h"
#include "esp_timer.h"
#include "esp_vfs_fat.h"
#include "diskio_sdmmc.h"
#include "ff.h"
#include "freertos/FreeRTOS.h"
#include "freertos/idf_additions.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "http_range_client.h"
#include "jetkvm_board_config.h"
#include "jetkvm_memory_policy.h"
#include "sd_pwr_ctrl_by_on_chip_ldo.h"
#include "sd_protocol_defs.h"
#include "sdmmc_cmd.h"
#include "tf_detect_contract.h"
#include "tf_sd_trace.h"
#include "soc/gpio_struct.h"
#include "soc/soc_caps.h"
#include "soc/sdmmc_struct.h"
#include "storage_format_contract.h"
#include "upload_dma_contract.h"
#include "upload_identity_contract.h"

#if JETKVM_TF_BOOT_SELFTEST
#include "psa/crypto.h"
#endif

#define TF_CARD_DETECT_GPIO GPIO_NUM_45
#define TF_POWER_GPIO GPIO_NUM_46
#ifndef JETKVM_TF_POWER_OFF_MS
#define JETKVM_TF_POWER_OFF_MS 500
#endif
#define TF_POWER_OFF_DELAY_MS JETKVM_TF_POWER_OFF_MS
#define TF_POWER_LOW_DELAY_MS 50
#define TF_POWER_RELEASE_DELAY_MS 40
#define TF_PIN_CLK GPIO_NUM_43
#define TF_PIN_CMD GPIO_NUM_44
#define TF_PIN_D0 GPIO_NUM_39
#define TF_PIN_D1 GPIO_NUM_40
#define TF_PIN_D2 GPIO_NUM_41
#define TF_PIN_D3 GPIO_NUM_42
#define TF_BUS_PIN_MASK ((1ULL << TF_PIN_CLK) | (1ULL << TF_PIN_CMD) | \
                         (1ULL << TF_PIN_D0) | (1ULL << TF_PIN_D1) | \
                         (1ULL << TF_PIN_D2) | (1ULL << TF_PIN_D3))
#define STORAGE_MOUNT_POINT "/sdcard"
#define IMAGES_FOLDER STORAGE_MOUNT_POINT "/jetkvm/images"
#define FAT_IMAGES_FOLDER "/jetkvm/images"
#define TF_UPLOAD_DMA_BOUNCE_BYTES (16U * 1024U)
/* WebRTC receives payloads in PSRAM and therefore needs an Internal/DMA
 * bounce window for SDMMC.  Keep this to one exFAT allocation unit: a 64 KiB
 * request does not fit once the esp_peer WebRTC peer has allocated its
 * buffers. */
#define TF_UPLOAD_OWNER_STAGING_BYTES (32U * 1024U)
#define TF_UPLOAD_DMA_ALIGNMENT 128U
#define TF_UPLOAD_INCOMPLETE_SUFFIX ".incomplete"
#define TF_UPLOAD_IDENTITY_SUFFIX ".identity"
#define TF_UPLOAD_IDENTITY_TEMP_SUFFIX ".tmp"
#define TF_PREFETCH_TASK_STACK_BYTES (4U * 1024U)
#define TF_PREFETCH_TASK_PRIORITY (tskIDLE_PRIORITY + 4U)
#define TF_PREFETCH_TASK_CORE 0
#define TF_PREFETCH_MUTEX_POLL_TICKS 1U
#define TF_DETECT_TASK_STACK_BYTES (4U * 1024U)
#define TF_DETECT_TASK_PRIORITY (tskIDLE_PRIORITY + 1U)
#define TF_DETECT_POLL_MS 250U

typedef enum {
    TF_UPLOAD_METADATA_IDLE = 0,
    TF_UPLOAD_METADATA_READ_IDENTITY,
    TF_UPLOAD_METADATA_TEMPORARY_IDENTITY,
} tf_upload_metadata_role_t;

#if JETKVM_TF_UPLOAD_COPY_PIPELINE && JETKVM_TF_MSC_PREFETCH
typedef enum {
    TF_UPLOAD_COPY_IDLE = 0,
    TF_UPLOAD_COPY_READY,
    TF_UPLOAD_COPY_RUNNING,
} tf_upload_copy_state_t;
#endif

/* The always-resident scratch stays bounded to the MSC read-ahead window and
 * legacy ownerless uploads. HTTP/WebRTC owners materialize one policy-gated
 * 32 KiB internal-DMA staging buffer only for their writer-token lifetime. */
static uint8_t s_tf_upload_dma_bounce[TF_UPLOAD_DMA_BOUNCE_BYTES]
    __attribute__((aligned(TF_UPLOAD_DMA_ALIGNMENT)));

typedef struct {
    bool initialized;
    bool card_present;
    /* Advances with each card that the card-detect task releases. */
    uint32_t card_generation;
    bool filesystem_mounted;
    FIL image;
    bool image_open;
    bool image_writable;
    bool image_dirty;
    jetkvm_virtual_media_source_t source;
    jetkvm_virtual_media_mode_t mode;
    char filename[JETKVM_VIRTUAL_MEDIA_FILENAME_CAPACITY];
    char url[JETKVM_VIRTUAL_MEDIA_URL_CAPACITY];
    uint64_t image_size;
    jetkvm_http_range_reader_t *http_reader;
    FIL upload;
    bool upload_open;
    bool upload_close_pending;
    bool upload_claim_pending;
    FIL upload_metadata;
    tf_upload_metadata_role_t upload_metadata_role;
    bool upload_metadata_open;
    bool upload_metadata_close_pending;
    bool upload_metadata_cleanup_pending;
    char upload_metadata_path[sizeof(IMAGES_FOLDER) +
                              JETKVM_VIRTUAL_MEDIA_FILENAME_CAPACITY + 40U];
    char upload_id[JETKVM_VIRTUAL_MEDIA_UPLOAD_ID_CAPACITY];
    jetkvm_virtual_media_upload_owner_t upload_owner;
    jetkvm_virtual_media_upload_token_t upload_owner_token;
    uint8_t *upload_staging;
    size_t upload_staging_size;
    char upload_final_path[sizeof(IMAGES_FOLDER) +
                           JETKVM_VIRTUAL_MEDIA_FILENAME_CAPACITY + 2U];
    char upload_incomplete_path[sizeof(IMAGES_FOLDER) +
                                JETKVM_VIRTUAL_MEDIA_FILENAME_CAPACITY + 16U];
    char upload_identity_path[sizeof(IMAGES_FOLDER) +
                              JETKVM_VIRTUAL_MEDIA_FILENAME_CAPACITY + 32U];
    char upload_sha256_hex[JETKVM_UPLOAD_SHA256_HEX_LENGTH + 1U];
    uint64_t upload_size;
    uint64_t upload_written;
    jetkvm_virtual_media_io_stats_t io_stats;
    jetkvm_virtual_media_write_io_stats_t write_io_stats;
    jetkvm_virtual_media_upload_io_stats_t upload_io_stats;
#if JETKVM_TF_MSC_PREFETCH
    TaskHandle_t prefetch_task;
    bool prefetch_job_pending;
    bool prefetch_ready;
    uint32_t prefetch_generation;
    uint32_t prefetch_job_generation;
    uint64_t prefetch_job_offset;
    size_t prefetch_job_size;
    size_t prefetch_job_buffer_offset;
    uint64_t prefetch_ready_offset;
    size_t prefetch_ready_size;
    int64_t prefetch_job_scheduled_us;
    int64_t prefetch_ready_us;
#if JETKVM_TF_UPLOAD_COPY_PIPELINE
    /* Uploads make read-ahead idle for the owner-token lifetime. Reuse that
     * already-budgeted worker to fill the alternate half of the same 32 KiB
     * staging allocation while SDMMC consumes the current half. */
    _Atomic uint32_t upload_copy_state;
    _Atomic uint32_t upload_copy_elapsed_us;
    const uint8_t *upload_copy_source;
    uint8_t *upload_copy_destination;
    size_t upload_copy_size;
    SemaphoreHandle_t upload_copy_done;
    StaticSemaphore_t upload_copy_done_storage;
#endif
#endif
    TaskHandle_t card_detect_task;
    BYTE drive;
    sdmmc_card_t *card;
    sd_pwr_ctrl_handle_t io_power;
    SemaphoreHandle_t mutex;
    StaticSemaphore_t mutex_storage;
} virtual_media_context_t;

static const char *TAG = "jetkvm_virtual_media";
static virtual_media_context_t s_ctx;
static esp_err_t fat_result(FRESULT result);
static esp_err_t close_idle_upload_locked(void);

#if JETKVM_TF_RMT_TRACE
static void dump_tf_host_state(void)
{
    ESP_LOGI(TAG,
             "TF_DIAG gpio43_level=%d out_cfg=0x%08" PRIx32
             " out_sel=%u oen_sel=%u sd_clkena=0x%08" PRIx32
             " clkdiv=0x%08" PRIx32 " clksrc=0x%08" PRIx32
             " cmd=0x%08" PRIx32 " status=0x%08" PRIx32
             " rintsts=0x%08" PRIx32,
             gpio_get_level(TF_PIN_CLK), GPIO.func_out_sel_cfg[TF_PIN_CLK].val,
             (unsigned)GPIO.func_out_sel_cfg[TF_PIN_CLK].out_sel,
             (unsigned)GPIO.func_out_sel_cfg[TF_PIN_CLK].oen_sel,
             SDMMC.clkena.val, SDMMC.clkdiv.val, SDMMC.clksrc.val,
             SDMMC.cmd.val, SDMMC.status.val, SDMMC.rintsts.val);
}
#endif

#if JETKVM_TF_Q1_RC_TEST
static void run_tf_q1_rc_test(const char *phase)
{
    static const int64_t sample_us[] = {
        0, 10, 100, 1000, 10000, 100000, 500000,
    };
    int levels[sizeof(sample_us) / sizeof(sample_us[0])] = {0};
    size_t next_sample = 0;
    int64_t first_high_us = -1;

    /* CLK and D0..D3 remain driven low. Releasing only CMD lets its external
     * 51 kOhm pull-up expose whether the nominally switched card rail has
     * really discharged, via the card's input protection path and 10 uF VDD. */
    gpio_set_direction(TF_PIN_CMD, GPIO_MODE_INPUT);
    gpio_pullup_dis(TF_PIN_CMD);
    gpio_pulldown_dis(TF_PIN_CMD);
    int64_t start = esp_timer_get_time();
    while (next_sample < sizeof(sample_us) / sizeof(sample_us[0])) {
        int level = gpio_get_level(TF_PIN_CMD);
        int64_t elapsed = esp_timer_get_time() - start;
        if (level && first_high_us < 0) {
            first_high_us = elapsed;
        }
        while (next_sample < sizeof(sample_us) / sizeof(sample_us[0]) &&
               elapsed >= sample_us[next_sample]) {
            levels[next_sample] = level;
            ++next_sample;
        }
    }
    ESP_LOGI(TAG,
             "TF_Q1_RC phase=%s gpio46=%d first_high_us=%" PRId64
             " cmd_levels t0=%d t10us=%d t100us=%d t1ms=%d t10ms=%d t100ms=%d t500ms=%d",
             phase, gpio_get_level(TF_POWER_GPIO), first_high_us,
             levels[0], levels[1], levels[2], levels[3], levels[4], levels[5],
             levels[6]);

    gpio_set_direction(TF_PIN_CMD, GPIO_MODE_OUTPUT);
    gpio_set_level(TF_PIN_CMD, 0);
}
#endif

#ifndef JETKVM_TF_SDMMC_FREQ_KHZ
#define JETKVM_TF_SDMMC_FREQ_KHZ 40000
#endif
#ifndef JETKVM_TF_SDMMC_SLOT
#define JETKVM_TF_SDMMC_SLOT 1
#endif
#ifndef JETKVM_TF_SDMMC_WIDTH
#define JETKVM_TF_SDMMC_WIDTH 4
#endif
#ifndef JETKVM_TF_INTERNAL_PULLUPS
#define JETKVM_TF_INTERNAL_PULLUPS 0
#endif
#ifndef JETKVM_TF_HTTP_PSRAM_DIRECT
#define JETKVM_TF_HTTP_PSRAM_DIRECT 1
#endif
#ifndef JETKVM_TF_UPLOAD_COPY_PIPELINE
#define JETKVM_TF_UPLOAD_COPY_PIPELINE 0
#endif
#ifndef JETKVM_TF_BOOT_SELFTEST
#define JETKVM_TF_BOOT_SELFTEST 0
#endif
#ifndef JETKVM_TF_BOOT_SELFTEST_MIB
#define JETKVM_TF_BOOT_SELFTEST_MIB 32
#endif
#ifndef JETKVM_TF_RAW_BOOT_SELFTEST
#define JETKVM_TF_RAW_BOOT_SELFTEST 0
#endif
#ifndef JETKVM_TF_RAW_SELFTEST_MIB
#define JETKVM_TF_RAW_SELFTEST_MIB 512
#endif
#ifndef JETKVM_TF_RAW_SELFTEST_CHUNK_KIB
#define JETKVM_TF_RAW_SELFTEST_CHUNK_KIB 64
#endif
#ifndef JETKVM_TF_RAW_DELAY_PHASE
#define JETKVM_TF_RAW_DELAY_PHASE -1
#endif
#ifndef JETKVM_TF_RAW_RETRIES
#define JETKVM_TF_RAW_RETRIES 2
#endif
#ifndef JETKVM_TF_RMT_TRACE
#define JETKVM_TF_RMT_TRACE 0
#endif
#ifndef JETKVM_TF_Q1_RC_TEST
#define JETKVM_TF_Q1_RC_TEST 0
#endif

_Static_assert((JETKVM_TF_SDMMC_FREQ_KHZ >= 400 &&
                JETKVM_TF_SDMMC_FREQ_KHZ <= 40000) ||
               JETKVM_TF_SDMMC_FREQ_KHZ == SDMMC_FREQ_DDR50 ||
               JETKVM_TF_SDMMC_FREQ_KHZ == SDMMC_FREQ_SDR50 ||
               JETKVM_TF_SDMMC_FREQ_KHZ == SDMMC_FREQ_SDR104,
               "TF SDMMC clock must be 400..40000, 50000, 100000 or 200000 kHz");
_Static_assert(JETKVM_TF_SDMMC_SLOT == 0 || JETKVM_TF_SDMMC_SLOT == 1,
               "TF SDMMC slot must be 0 or 1");
_Static_assert(JETKVM_TF_SDMMC_WIDTH == 1 || JETKVM_TF_SDMMC_WIDTH == 4,
               "TF SDMMC width must be 1 or 4");
_Static_assert(JETKVM_TF_INTERNAL_PULLUPS == 0 ||
                   JETKVM_TF_INTERNAL_PULLUPS == 1,
               "TF internal pull-up selector must be 0 or 1");
_Static_assert(JETKVM_TF_HTTP_PSRAM_DIRECT == 0 ||
                   JETKVM_TF_HTTP_PSRAM_DIRECT == 1,
               "TF HTTP PSRAM direct-write selector must be 0 or 1");
#if JETKVM_TF_HTTP_PSRAM_DIRECT
_Static_assert(SOC_SDMMC_PSRAM_DMA_CAPABLE,
               "HTTP PSRAM direct writes require SDMMC PSRAM DMA support");
#endif
_Static_assert(JETKVM_TF_POWER_OFF_MS >= 100 &&
                   JETKVM_TF_POWER_OFF_MS <= 10000,
               "TF power-off interval must be 100..10000 ms");
_Static_assert(JETKVM_TF_BOOT_SELFTEST == 0 || JETKVM_TF_BOOT_SELFTEST == 1,
               "TF boot self-test selector must be 0 or 1");
_Static_assert(JETKVM_TF_BOOT_SELFTEST_MIB >= 1 &&
                   JETKVM_TF_BOOT_SELFTEST_MIB <= 512,
               "TF boot self-test size must be 1..512 MiB");
_Static_assert(JETKVM_TF_RAW_BOOT_SELFTEST == 0 ||
                   JETKVM_TF_RAW_BOOT_SELFTEST == 1,
               "TF raw boot self-test selector must be 0 or 1");
_Static_assert(JETKVM_TF_RAW_SELFTEST_MIB >= 0 &&
                   JETKVM_TF_RAW_SELFTEST_MIB <= 4096,
               "TF raw boot self-test size must be 0 or 1..4096 MiB");
_Static_assert(JETKVM_TF_RAW_SELFTEST_CHUNK_KIB >= 1 &&
                   JETKVM_TF_RAW_SELFTEST_CHUNK_KIB <= 256,
               "TF raw boot self-test chunk must be 1..256 KiB");
_Static_assert(JETKVM_TF_RAW_DELAY_PHASE >= -1 &&
                   JETKVM_TF_RAW_DELAY_PHASE <= 3,
               "TF raw delay phase must be -1 or 0..3");
_Static_assert(JETKVM_TF_RAW_RETRIES >= 0 && JETKVM_TF_RAW_RETRIES <= 10,
               "TF raw read retries must be 0..10");
_Static_assert(!(JETKVM_TF_BOOT_SELFTEST && JETKVM_TF_RAW_BOOT_SELFTEST),
               "TF filesystem and raw boot self-tests are mutually exclusive");
_Static_assert(JETKVM_TF_RMT_TRACE == 0 || JETKVM_TF_RMT_TRACE == 1,
               "TF RMT trace selector must be 0 or 1");
_Static_assert(JETKVM_TF_Q1_RC_TEST == 0 || JETKVM_TF_Q1_RC_TEST == 1,
               "TF Q1 RC diagnostic selector must be 0 or 1");
_Static_assert(JETKVM_TF_MSC_PREFETCH == 0 || JETKVM_TF_MSC_PREFETCH == 1,
               "TF MSC prefetch selector must be 0 or 1");
_Static_assert(JETKVM_TF_UPLOAD_COPY_PIPELINE == 0 ||
                   JETKVM_TF_UPLOAD_COPY_PIPELINE == 1,
               "TF upload copy-pipeline selector must be 0 or 1");
#if JETKVM_TF_UPLOAD_COPY_PIPELINE
_Static_assert(JETKVM_TF_MSC_PREFETCH == 1,
               "TF upload copy pipeline reuses the MSC prefetch worker");
_Static_assert(TF_UPLOAD_OWNER_STAGING_BYTES % 2U == 0U,
               "TF upload staging must split into equal halves");
_Static_assert((TF_UPLOAD_OWNER_STAGING_BYTES / 2U) %
                       JETKVM_VIRTUAL_MEDIA_BLOCK_SIZE == 0U,
               "TF upload pipeline halves must contain whole sectors");
_Static_assert((TF_UPLOAD_OWNER_STAGING_BYTES / 2U) %
                       TF_UPLOAD_DMA_ALIGNMENT == 0U,
               "TF upload pipeline halves must retain DMA alignment");
#endif

static const char *configure_tf_input_delay(sdmmc_host_t *host)
{
    assert(host != NULL);

#if JETKVM_TF_RAW_BOOT_SELFTEST && JETKVM_TF_RAW_DELAY_PHASE >= 0
    host->input_delay_phase =
        (sdmmc_delay_phase_t)(SDMMC_DELAY_PHASE_0 +
                              JETKVM_TF_RAW_DELAY_PHASE);
    return "raw-fixed-phase";
#elif JETKVM_TF_SDMMC_FREQ_KHZ == SDMMC_FREQ_DDR50
    /* DDR50 has no CMD19 tuning procedure. Phase 1 is the stable sampling
     * point qualified on both tested cards on this board revision. */
    host->input_delay_phase = SDMMC_DELAY_PHASE_1;
    return "ddr50-fixed-phase-1";
#elif JETKVM_TF_SDMMC_FREQ_KHZ == SDMMC_FREQ_SDR50 || \
      JETKVM_TF_SDMMC_FREQ_KHZ == SDMMC_FREQ_SDR104
    /* ESP-IDF performs CMD19 phase tuning after entering SDR50/SDR104. Phase
     * 0 is only the safe starting point used before that tuning completes. */
    host->input_delay_phase = SDMMC_DELAY_PHASE_0;
    return "sdr-cmd19-auto";
#else
    host->input_delay_phase = SDMMC_DELAY_PHASE_0;
    return "sdr-fixed-phase-0";
#endif
}

#if JETKVM_TF_RAW_BOOT_SELFTEST
#define TF_RAW_SELFTEST_BYTES \
    ((uint64_t)JETKVM_TF_RAW_SELFTEST_MIB * 1024ULL * 1024ULL)
#define TF_RAW_SELFTEST_CHUNK \
    ((size_t)JETKVM_TF_RAW_SELFTEST_CHUNK_KIB * 1024U)
#define TF_RAW_PROGRESS_BYTES (64ULL * 1024ULL * 1024ULL)

static uint64_t tf_raw_mib_per_second_x100(uint64_t bytes,
                                           int64_t elapsed_us)
{
    if (elapsed_us <= 0) {
        return 0U;
    }
    return bytes * 100000000ULL /
           ((uint64_t)elapsed_us * 1024ULL * 1024ULL);
}

static esp_err_t run_tf_raw_boot_selftest(sdmmc_card_t *card)
{
    ESP_RETURN_ON_FALSE(card != NULL, ESP_ERR_INVALID_ARG, TAG,
                        "TF raw self-test card is null");
    ESP_RETURN_ON_FALSE(card->csd.sector_size > 0,
                        ESP_ERR_INVALID_SIZE, TAG,
                        "TF raw self-test sector size is invalid");
    const size_t sector_size = card->csd.sector_size;
    ESP_RETURN_ON_FALSE(TF_RAW_SELFTEST_CHUNK % sector_size == 0U,
                        ESP_ERR_INVALID_SIZE, TAG,
                        "TF raw self-test chunk is not sector aligned");

    uint8_t *buffer = heap_caps_malloc(
        TF_RAW_SELFTEST_CHUNK,
        MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA | MALLOC_CAP_8BIT);
    ESP_RETURN_ON_FALSE(buffer != NULL, ESP_ERR_NO_MEM, TAG,
                        "TF raw self-test DMA allocation failed");

    const size_t sectors_per_chunk = TF_RAW_SELFTEST_CHUNK / sector_size;
    const uint64_t capacity_sectors = card->csd.capacity;
    uint64_t target_sectors = JETKVM_TF_RAW_SELFTEST_MIB == 0 ?
        capacity_sectors : TF_RAW_SELFTEST_BYTES / sector_size;
    if (target_sectors > capacity_sectors) {
        target_sectors = capacity_sectors;
    }
    target_sectors -= target_sectors % sectors_per_chunk;
    if (target_sectors == 0U) {
        ESP_LOGE(TAG,
                 "TF_RAW_READ FAIL stage=capacity capacity_sectors=%" PRIu64,
                 capacity_sectors);
        heap_caps_free(buffer);
        return ESP_ERR_INVALID_SIZE;
    }

    const uint64_t target_bytes = target_sectors * sector_size;
    uint64_t completed_bytes = 0U;
    uint64_t next_progress_bytes = TF_RAW_PROGRESS_BYTES;
    uint32_t checksum = 0U;
    uint32_t recovered_requests = 0U;
    int64_t started_us = esp_timer_get_time();
    ESP_LOGI(TAG,
             "TF_RAW_READ START bytes=%" PRIu64 " chunk=%zu sector_size=%zu "
             "start_sector=0 retries=%d write_access=no",
             target_bytes, TF_RAW_SELFTEST_CHUNK, sector_size,
             JETKVM_TF_RAW_RETRIES);

    for (uint64_t sector = 0U; sector < target_sectors;
         sector += sectors_per_chunk) {
        esp_err_t error = sdmmc_read_sectors(
            card, buffer, (size_t)sector, sectors_per_chunk);
        esp_err_t first_error = error;
        unsigned retries_used = 0U;
        while (error != ESP_OK && retries_used < JETKVM_TF_RAW_RETRIES) {
            ++retries_used;
            ESP_LOGW(TAG,
                     "TF_RAW_READ RETRY sector=%" PRIu64
                     " count=%zu attempt=%u error=0x%x/%s",
                     sector, sectors_per_chunk, retries_used,
                     (unsigned)error, esp_err_to_name(error));
            error = sdmmc_read_sectors(
                card, buffer, (size_t)sector, sectors_per_chunk);
        }
        if (error != ESP_OK) {
            ESP_LOGE(TAG,
                     "TF_RAW_READ FAIL sector=%" PRIu64
                     " byte_offset=%" PRIu64 " count=%zu chunk=%zu "
                     "first_error=0x%x/%s final_error=0x%x/%s",
                     sector, sector * sector_size, sectors_per_chunk,
                     TF_RAW_SELFTEST_CHUNK, (unsigned)first_error,
                     esp_err_to_name(first_error), (unsigned)error,
                     esp_err_to_name(error));

            /* A sector-at-a-time read over the failed request distinguishes
             * a persistent location failure from a marginal long CMD18. It
             * remains strictly read-only. */
            size_t probe = 0U;
            for (; probe < sectors_per_chunk; ++probe) {
                esp_err_t probe_error = sdmmc_read_sectors(
                    card, buffer, (size_t)sector + probe, 1U);
                if (probe_error != ESP_OK) {
                    ESP_LOGE(TAG,
                             "TF_RAW_READ SECTOR_PROBE_FAIL sector=%" PRIu64
                             " byte_offset=%" PRIu64 " error=0x%x/%s",
                             sector + probe, (sector + probe) * sector_size,
                             (unsigned)probe_error,
                             esp_err_to_name(probe_error));
                    break;
                }
            }
            if (probe == sectors_per_chunk) {
                ESP_LOGW(TAG,
                         "TF_RAW_READ SECTOR_PROBE_PASS sector=%" PRIu64
                         " count=%zu result=multiblock-only-or-transient",
                         sector, sectors_per_chunk);
            }
            heap_caps_free(buffer);
            return error;
        }
        if (retries_used > 0U) {
            ++recovered_requests;
            ESP_LOGW(TAG,
                     "TF_RAW_READ RECOVERED sector=%" PRIu64
                     " attempts=%u first_error=0x%x/%s",
                     sector, retries_used + 1U, (unsigned)first_error,
                     esp_err_to_name(first_error));
        }
        checksum = (checksum << 5U) ^ (checksum >> 2U) ^ buffer[0] ^
                   buffer[TF_RAW_SELFTEST_CHUNK - 1U];
        completed_bytes += TF_RAW_SELFTEST_CHUNK;
        if (completed_bytes >= next_progress_bytes) {
            ESP_LOGI(TAG,
                     "TF_RAW_READ PROGRESS bytes=%" PRIu64
                     " sector=%" PRIu64 " recovered=%" PRIu32,
                     completed_bytes, sector + sectors_per_chunk,
                     recovered_requests);
            next_progress_bytes += TF_RAW_PROGRESS_BYTES;
            vTaskDelay(1);
        }
    }

    int64_t elapsed_us = esp_timer_get_time() - started_us;
    uint64_t mib_s_x100 =
        tf_raw_mib_per_second_x100(completed_bytes, elapsed_us);
    if (recovered_requests > 0U) {
        ESP_LOGE(TAG,
                 "TF_RAW_READ UNSTABLE bytes=%" PRIu64
                 " chunk=%zu elapsed_ms=%lld read_mib_s=%llu.%02llu "
                 "sample_fingerprint=0x%08" PRIx32
                 " recovered=%" PRIu32,
                 completed_bytes, TF_RAW_SELFTEST_CHUNK,
                 (long long)(elapsed_us / 1000),
                 (unsigned long long)(mib_s_x100 / 100U),
                 (unsigned long long)(mib_s_x100 % 100U), checksum,
                 recovered_requests);
        heap_caps_free(buffer);
        return ESP_ERR_INVALID_RESPONSE;
    }
    ESP_LOGI(TAG,
             "TF_RAW_READ PASS bytes=%" PRIu64 " chunk=%zu elapsed_ms=%lld "
             "read_mib_s=%llu.%02llu sample_fingerprint=0x%08" PRIx32
             " recovered=0",
             completed_bytes, TF_RAW_SELFTEST_CHUNK,
             (long long)(elapsed_us / 1000),
             (unsigned long long)(mib_s_x100 / 100U),
             (unsigned long long)(mib_s_x100 % 100U), checksum);
    heap_caps_free(buffer);
    return ESP_OK;
}
#endif
#if JETKVM_TF_RMT_TRACE
_Static_assert(JETKVM_TF_SDMMC_SLOT == 1,
               "passive RMT trace requires slot 1 GPIO-matrix routing");
#endif

_Static_assert(FF_FS_EXFAT == 1, "virtual media requires FatFs exFAT");
_Static_assert(FF_FS_READONLY == 0,
               "writable virtual disks require FatFs write support");
_Static_assert(sizeof(FSIZE_t) == sizeof(uint64_t),
               "virtual media requires 64-bit FatFs FSIZE_t");

static esp_err_t lock_context(void)
{
    return s_ctx.mutex != NULL &&
                   xSemaphoreTake(s_ctx.mutex, portMAX_DELAY) == pdTRUE ?
               ESP_OK : ESP_ERR_INVALID_STATE;
}

static void unlock_context(void)
{
    xSemaphoreGive(s_ctx.mutex);
}

#if JETKVM_TF_BOOT_SELFTEST
#define TF_SELFTEST_BYTES ((uint32_t)JETKVM_TF_BOOT_SELFTEST_MIB * 1024U * 1024U)
#define TF_SELFTEST_CHUNK 4096U
#define TF_THROUGHPUT_BYTES (64U * 1024U * 1024U)
#define TF_THROUGHPUT_CHUNK (64U * 1024U)
static DRAM_ATTR uint8_t s_tf_selftest_buffer[TF_SELFTEST_CHUNK];

static void tf_selftest_fill(uint32_t offset)
{
    for (uint32_t i = 0; i < TF_SELFTEST_CHUNK; ++i) {
        uint32_t position = offset + i;
        s_tf_selftest_buffer[i] =
            (uint8_t)(0x5aU + position * 37U + (position >> 8U) * 17U);
    }
}

static void tf_selftest_digest_hex(const uint8_t digest[32], char output[65])
{
    static const char hex[] = "0123456789abcdef";
    for (size_t i = 0; i < 32U; ++i) {
        output[i * 2U] = hex[digest[i] >> 4U];
        output[i * 2U + 1U] = hex[digest[i] & 0x0fU];
    }
    output[64] = '\0';
}

static uint64_t tf_selftest_mib_per_second_x100(uint64_t bytes,
                                                int64_t elapsed_us)
{
    if (elapsed_us <= 0) {
        return 0U;
    }
    return bytes * 100000000ULL /
           ((uint64_t)elapsed_us * 1024ULL * 1024ULL);
}

static esp_err_t run_tf_boot_selftest(void)
{
    psa_status_t status = psa_crypto_init();
    if (status != PSA_SUCCESS) {
        ESP_LOGE(TAG, "TF_AB SELFTEST_FAIL stage=psa-init status=%ld",
                 (long)status);
        return ESP_FAIL;
    }
    char path[48];
    snprintf(path, sizeof(path), "%u:/jetkvm/.tf-signal-ab.tmp",
             (unsigned)s_ctx.drive);
    /* IDF's per-file FatFs cache is dynamically allocated. f_open() retains a
     * non-NULL FIL::buf, so a stack FIL must start zeroed just like the static
     * production image/upload objects do. */
    FIL file = {0};
    bool opened = false;
    FRESULT fr = f_open(&file, path, FA_WRITE | FA_CREATE_ALWAYS);
    if (fr != FR_OK) {
        ESP_LOGE(TAG, "TF_AB SELFTEST_FAIL stage=open-write fr=%d", (int)fr);
        return fat_result(fr);
    }
    opened = true;
    psa_hash_operation_t expected_op = PSA_HASH_OPERATION_INIT;
    psa_hash_operation_t actual_op = PSA_HASH_OPERATION_INIT;
    status = psa_hash_setup(&expected_op, PSA_ALG_SHA_256);
    if (status != PSA_SUCCESS) {
        ESP_LOGE(TAG, "TF_AB SELFTEST_FAIL stage=sha-setup status=%ld",
                 (long)status);
        (void)f_close(&file);
        (void)f_unlink(path);
        return ESP_FAIL;
    }
    int64_t started_us = esp_timer_get_time();
    int64_t write_started_us = started_us;
    for (uint32_t offset = 0; offset < TF_SELFTEST_BYTES;
         offset += TF_SELFTEST_CHUNK) {
        tf_selftest_fill(offset);
        UINT written = 0U;
        fr = f_write(&file, s_tf_selftest_buffer, TF_SELFTEST_CHUNK, &written);
        status = psa_hash_update(&expected_op, s_tf_selftest_buffer,
                                 TF_SELFTEST_CHUNK);
        if (fr != FR_OK || written != TF_SELFTEST_CHUNK ||
            status != PSA_SUCCESS) {
            ESP_LOGE(TAG,
                     "TF_AB SELFTEST_FAIL stage=write offset=%" PRIu32
                     " fr=%d written=%u sha=%ld",
                     offset, (int)fr, (unsigned)written, (long)status);
            goto failed;
        }
    }
    fr = f_sync(&file);
    if (fr == FR_OK) {
        fr = f_close(&file);
        opened = false;
    }
    if (fr != FR_OK) {
        ESP_LOGE(TAG, "TF_AB SELFTEST_FAIL stage=sync-close fr=%d", (int)fr);
        goto failed;
    }
    int64_t write_elapsed_us = esp_timer_get_time() - write_started_us;
    uint8_t expected[32];
    size_t expected_size = 0U;
    status = psa_hash_finish(&expected_op, expected, sizeof(expected),
                             &expected_size);
    if (status != PSA_SUCCESS || expected_size != sizeof(expected)) {
        ESP_LOGE(TAG, "TF_AB SELFTEST_FAIL stage=sha-write status=%ld",
                 (long)status);
        goto failed;
    }

    fr = f_open(&file, path, FA_READ);
    if (fr != FR_OK) {
        ESP_LOGE(TAG, "TF_AB SELFTEST_FAIL stage=open-read fr=%d", (int)fr);
        goto failed;
    }
    opened = true;
    status = psa_hash_setup(&actual_op, PSA_ALG_SHA_256);
    if (status != PSA_SUCCESS) {
        ESP_LOGE(TAG, "TF_AB SELFTEST_FAIL stage=sha-read-setup status=%ld",
                 (long)status);
        goto failed;
    }
    int64_t read_started_us = esp_timer_get_time();
    for (uint32_t offset = 0; offset < TF_SELFTEST_BYTES;
         offset += TF_SELFTEST_CHUNK) {
        UINT received = 0U;
        fr = f_read(&file, s_tf_selftest_buffer, TF_SELFTEST_CHUNK, &received);
        status = psa_hash_update(&actual_op, s_tf_selftest_buffer, received);
        if (fr != FR_OK || received != TF_SELFTEST_CHUNK ||
            status != PSA_SUCCESS) {
            ESP_LOGE(TAG,
                     "TF_AB SELFTEST_FAIL stage=read offset=%" PRIu32
                     " fr=%d received=%u sha=%ld",
                     offset, (int)fr, (unsigned)received, (long)status);
            goto failed;
        }
    }
    fr = f_close(&file);
    opened = false;
    uint8_t actual[32];
    size_t actual_size = 0U;
    status = psa_hash_finish(&actual_op, actual, sizeof(actual), &actual_size);
    char expected_hex[65];
    char actual_hex[65];
    tf_selftest_digest_hex(expected, expected_hex);
    tf_selftest_digest_hex(actual, actual_hex);
    int64_t elapsed_us = esp_timer_get_time() - started_us;
    int64_t read_elapsed_us = esp_timer_get_time() - read_started_us;
    bool match = fr == FR_OK && status == PSA_SUCCESS &&
                 actual_size == sizeof(actual) &&
                 memcmp(expected, actual, sizeof(expected)) == 0;
    uint64_t write_mib_s_x100 =
        tf_selftest_mib_per_second_x100(TF_SELFTEST_BYTES, write_elapsed_us);
    uint64_t read_mib_s_x100 =
        tf_selftest_mib_per_second_x100(TF_SELFTEST_BYTES, read_elapsed_us);
    ESP_LOGI(TAG,
             "TF_AB SELFTEST_%s bytes=%u total_ms=%lld write_ms=%lld "
             "write_mib_s=%llu.%02llu read_ms=%lld read_mib_s=%llu.%02llu "
             "expected=%s actual=%s",
             match ? "PASS" : "FAIL", TF_SELFTEST_BYTES,
             (long long)(elapsed_us / 1000),
             (long long)(write_elapsed_us / 1000),
             (unsigned long long)(write_mib_s_x100 / 100U),
             (unsigned long long)(write_mib_s_x100 % 100U),
             (long long)(read_elapsed_us / 1000),
             (unsigned long long)(read_mib_s_x100 / 100U),
             (unsigned long long)(read_mib_s_x100 % 100U),
             expected_hex, actual_hex);
    (void)f_unlink(path);
    return match ? ESP_OK : ESP_FAIL;

failed:
    if (opened) {
        (void)f_close(&file);
    }
    (void)psa_hash_abort(&expected_op);
    (void)psa_hash_abort(&actual_op);
    (void)f_unlink(path);
    return ESP_FAIL;
}

static esp_err_t run_tf_throughput_benchmark(void)
{
    uint8_t *buffer = heap_caps_malloc(
        TF_THROUGHPUT_CHUNK,
        MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA | MALLOC_CAP_8BIT);
    if (buffer == NULL) {
        ESP_LOGE(TAG, "TF_BENCH FILE_FAIL stage=alloc bytes=%u",
                 TF_THROUGHPUT_CHUNK);
        return ESP_ERR_NO_MEM;
    }
    for (uint32_t i = 0; i < TF_THROUGHPUT_CHUNK; ++i) {
        buffer[i] = (uint8_t)(0xa7U + i * 29U + (i >> 8U) * 11U);
    }

    char path[48];
    snprintf(path, sizeof(path), "%u:/jetkvm/.tf-throughput.tmp",
             (unsigned)s_ctx.drive);
    FIL file = {0};
    bool opened = false;
    const char *stage = "open-write";
    UINT io_count = 0U;
    FRESULT fr = f_open(&file, path, FA_WRITE | FA_CREATE_ALWAYS);
    if (fr != FR_OK) {
        goto failed;
    }
    opened = true;
    int64_t write_started_us = esp_timer_get_time();
    for (uint32_t offset = 0; offset < TF_THROUGHPUT_BYTES;
         offset += TF_THROUGHPUT_CHUNK) {
        UINT written = 0U;
        stage = "write";
        fr = f_write(&file, buffer, TF_THROUGHPUT_CHUNK, &written);
        io_count = written;
        if (fr != FR_OK || written != TF_THROUGHPUT_CHUNK) {
            goto failed;
        }
    }
    stage = "sync";
    fr = f_sync(&file);
    if (fr == FR_OK) {
        stage = "close-write";
        fr = f_close(&file);
        opened = false;
    }
    if (fr != FR_OK) {
        goto failed;
    }
    int64_t write_elapsed_us = esp_timer_get_time() - write_started_us;

    memset(buffer, 0, TF_THROUGHPUT_CHUNK);
    stage = "open-read";
    fr = f_open(&file, path, FA_READ);
    if (fr != FR_OK) {
        goto failed;
    }
    opened = true;
    uint32_t checksum = 0U;
    int64_t read_started_us = esp_timer_get_time();
    for (uint32_t offset = 0; offset < TF_THROUGHPUT_BYTES;
         offset += TF_THROUGHPUT_CHUNK) {
        UINT received = 0U;
        stage = "read";
        fr = f_read(&file, buffer, TF_THROUGHPUT_CHUNK, &received);
        io_count = received;
        if (fr != FR_OK || received != TF_THROUGHPUT_CHUNK) {
            goto failed;
        }
        checksum = (checksum << 5U) ^ (checksum >> 2U) ^ buffer[0] ^
                   buffer[TF_THROUGHPUT_CHUNK - 1U];
    }
    stage = "close-read";
    fr = f_close(&file);
    opened = false;
    if (fr != FR_OK) {
        goto failed;
    }
    int64_t read_elapsed_us = esp_timer_get_time() - read_started_us;
    uint64_t write_mib_s_x100 = tf_selftest_mib_per_second_x100(
        TF_THROUGHPUT_BYTES, write_elapsed_us);
    uint64_t read_mib_s_x100 = tf_selftest_mib_per_second_x100(
        TF_THROUGHPUT_BYTES, read_elapsed_us);
    ESP_LOGI(TAG,
             "TF_BENCH FILE_PASS bytes=%u chunk=%u write_ms=%lld "
             "write_mib_s=%llu.%02llu read_ms=%lld read_mib_s=%llu.%02llu "
             "checksum=0x%08" PRIx32,
             TF_THROUGHPUT_BYTES, TF_THROUGHPUT_CHUNK,
             (long long)(write_elapsed_us / 1000),
             (unsigned long long)(write_mib_s_x100 / 100U),
             (unsigned long long)(write_mib_s_x100 % 100U),
             (long long)(read_elapsed_us / 1000),
             (unsigned long long)(read_mib_s_x100 / 100U),
             (unsigned long long)(read_mib_s_x100 % 100U), checksum);
    (void)f_unlink(path);

    /* Read-only raw sweep isolates SDMMC/FatFs overhead without risking any
     * filesystem sectors. The file benchmark above remains the write result. */
    const int card_sector_size = s_ctx.card->csd.sector_size;
    const int card_capacity_sectors = s_ctx.card->csd.capacity;
    if (card_sector_size <= 0 || card_capacity_sectors <= 0 ||
        TF_THROUGHPUT_CHUNK % (uint32_t)card_sector_size != 0U) {
        ESP_LOGE(TAG,
                 "TF_BENCH RAW_READ_FAIL sector_size=%d capacity_sectors=%d",
                 card_sector_size, card_capacity_sectors);
        heap_caps_free(buffer);
        return ESP_ERR_INVALID_SIZE;
    }
    const uint32_t sector_size = (uint32_t)card_sector_size;
    const uint32_t sectors_per_chunk = TF_THROUGHPUT_CHUNK / sector_size;
    const uint32_t requested_chunks =
        TF_THROUGHPUT_BYTES / TF_THROUGHPUT_CHUNK;
    const uint64_t available_chunks =
        (uint64_t)card_capacity_sectors / sectors_per_chunk;
    const uint32_t raw_chunk_count =
        available_chunks < requested_chunks ? (uint32_t)available_chunks :
                                               requested_chunks;
    if (raw_chunk_count == 0U) {
        ESP_LOGE(TAG,
                 "TF_BENCH RAW_READ_FAIL capacity_sectors=%" PRIu32
                 " sectors_per_chunk=%" PRIu32,
                 (uint32_t)card_capacity_sectors, sectors_per_chunk);
        heap_caps_free(buffer);
        return ESP_ERR_INVALID_SIZE;
    }
    const uint32_t raw_sector_count = raw_chunk_count * sectors_per_chunk;
    const uint32_t raw_bytes = raw_chunk_count * TF_THROUGHPUT_CHUNK;
    checksum = 0U;
    int64_t raw_started_us = esp_timer_get_time();
    for (uint32_t sector = 0; sector < raw_sector_count;
         sector += sectors_per_chunk) {
        esp_err_t read_error = sdmmc_read_sectors(
            s_ctx.card, buffer, sector, sectors_per_chunk);
        if (read_error != ESP_OK) {
            ESP_LOGE(TAG,
                     "TF_BENCH RAW_READ_FAIL sector=%" PRIu32 " error=%s",
                     sector, esp_err_to_name(read_error));
            heap_caps_free(buffer);
            return read_error;
        }
        checksum = (checksum << 5U) ^ (checksum >> 2U) ^ buffer[0] ^
                   buffer[TF_THROUGHPUT_CHUNK - 1U];
    }
    int64_t raw_elapsed_us = esp_timer_get_time() - raw_started_us;
    uint64_t raw_mib_s_x100 = tf_selftest_mib_per_second_x100(
        raw_bytes, raw_elapsed_us);
    ESP_LOGI(TAG,
             "TF_BENCH RAW_READ_PASS bytes=%u chunk=%u elapsed_ms=%lld "
             "read_mib_s=%llu.%02llu checksum=0x%08" PRIx32,
             raw_bytes, TF_THROUGHPUT_CHUNK,
             (long long)(raw_elapsed_us / 1000),
             (unsigned long long)(raw_mib_s_x100 / 100U),
             (unsigned long long)(raw_mib_s_x100 % 100U), checksum);
    heap_caps_free(buffer);
    return ESP_OK;

failed:
    if (opened) {
        (void)f_close(&file);
    }
    (void)f_unlink(path);
    heap_caps_free(buffer);
    ESP_LOGE(TAG, "TF_BENCH FILE_FAIL stage=%s fr=%d transferred=%u",
             stage, (int)fr, (unsigned)io_count);
    return fr == FR_OK ? ESP_ERR_INVALID_SIZE : fat_result(fr);
}
#endif

static bool valid_filename(const char *filename)
{
    if (filename == NULL || filename[0] == '\0') {
        return false;
    }
    size_t length = strnlen(filename, JETKVM_VIRTUAL_MEDIA_FILENAME_CAPACITY);
    if (length == 0U || length >= JETKVM_VIRTUAL_MEDIA_FILENAME_CAPACITY ||
        strcmp(filename, ".") == 0 || strcmp(filename, "..") == 0) {
        return false;
    }
    /* FatFs create_name() strips trailing ASCII spaces and dots before it
     * resolves an LFN. Reject those spellings at the API boundary so a
     * caller cannot alias a protected upload artifact by appending a byte
     * that the filesystem silently discards. */
    if (filename[length - 1U] == ' ' || filename[length - 1U] == '.') {
        return false;
    }
    for (size_t i = 0U; i < length; ++i) {
        unsigned char byte = (unsigned char)filename[i];
        if (byte < 0x20U || byte == 0x7fU || byte == '/' || byte == '\\') {
            return false;
        }
    }
    return strstr(filename, "..") == NULL;
}

static esp_err_t trim_url(const char *url, char *output, size_t capacity)
{
    if (url == NULL || output == NULL || capacity == 0U) {
        return ESP_ERR_INVALID_ARG;
    }
    while (*url == ' ' || *url == '\t' || *url == '\r' || *url == '\n') {
        url++;
    }
    size_t length = strlen(url);
    while (length > 0U &&
           (url[length - 1U] == ' ' || url[length - 1U] == '\t' ||
            url[length - 1U] == '\r' || url[length - 1U] == '\n')) {
        length--;
    }
    if (length == 0U || length >= capacity) {
        return ESP_ERR_INVALID_ARG;
    }
    memcpy(output, url, length);
    output[length] = '\0';
    return ESP_OK;
}

static esp_err_t make_image_path(const char *filename, char *output,
                                 size_t output_capacity)
{
    if (!valid_filename(filename) || output == NULL || output_capacity == 0U) {
        return ESP_ERR_INVALID_ARG;
    }
    int written = snprintf(output, output_capacity, "%u:%s/%s",
                           (unsigned)s_ctx.drive, FAT_IMAGES_FOLDER, filename);
    return written >= 0 && (size_t)written < output_capacity ?
               ESP_OK : ESP_ERR_INVALID_SIZE;
}

static esp_err_t fat_result(FRESULT result)
{
    switch (result) {
    case FR_OK:
        return ESP_OK;
    case FR_NO_FILE:
    case FR_NO_PATH:
        return ESP_ERR_NOT_FOUND;
    case FR_INVALID_NAME:
    case FR_INVALID_PARAMETER:
        return ESP_ERR_INVALID_ARG;
    case FR_NOT_ENOUGH_CORE:
        return ESP_ERR_NO_MEM;
    case FR_TIMEOUT:
        return ESP_ERR_TIMEOUT;
    case FR_EXIST:
    case FR_DENIED:
    case FR_LOCKED:
        return ESP_ERR_INVALID_STATE;
    default:
        return ESP_FAIL;
    }
}

static bool fat_path_missing(FRESULT result)
{
    return result == FR_NO_FILE || result == FR_NO_PATH;
}

static esp_err_t append_path_suffix(const char *path, const char *suffix,
                                    char *output, size_t output_capacity)
{
    if (path == NULL || suffix == NULL || output == NULL ||
        output_capacity == 0U) {
        return ESP_ERR_INVALID_ARG;
    }
    int written = snprintf(output, output_capacity, "%s%s", path, suffix);
    return written >= 0 && (size_t)written < output_capacity ?
               ESP_OK : ESP_ERR_INVALID_SIZE;
}

/* Delete a reserved upload artifact without treating an already-absent path
 * as an I/O failure. All other FatFs errors are returned to the caller. */
static esp_err_t unlink_upload_artifact_locked(const char *path, bool *removed)
{
    FILINFO info;
    FRESULT result = f_stat(path, &info);
    if (fat_path_missing(result)) {
        if (removed != NULL) {
            *removed = false;
        }
        return ESP_OK;
    }
    if (result != FR_OK) {
        return fat_result(result);
    }
    result = f_unlink(path);
    if (result != FR_OK) {
        return fat_result(result);
    }
    if (removed != NULL) {
        *removed = true;
    }
    return ESP_OK;
}

static void clear_upload_metadata_tracking_locked(void)
{
    s_ctx.upload_metadata_role = TF_UPLOAD_METADATA_IDLE;
    s_ctx.upload_metadata_open = false;
    s_ctx.upload_metadata_close_pending = false;
    s_ctx.upload_metadata_cleanup_pending = false;
    s_ctx.upload_metadata_path[0] = '\0';
}

/* A metadata f_close failure must retain the exact FIL object. Every later
 * volume operation enters here before touching upload artifacts. Temporary
 * publication files are deleted only after their retained FIL closes. */
static esp_err_t service_upload_metadata_locked(void)
{
    if (s_ctx.upload_metadata_role == TF_UPLOAD_METADATA_IDLE) {
        return !s_ctx.upload_metadata_open &&
                       !s_ctx.upload_metadata_close_pending &&
                       !s_ctx.upload_metadata_cleanup_pending ?
                   ESP_OK : ESP_ERR_INVALID_STATE;
    }
    if (s_ctx.upload_metadata_open) {
        if (!s_ctx.upload_metadata_close_pending) {
            return ESP_ERR_INVALID_STATE;
        }
        FRESULT close_result = f_close(&s_ctx.upload_metadata);
        if (close_result != FR_OK) {
            return fat_result(close_result);
        }
        s_ctx.upload_metadata_open = false;
        s_ctx.upload_metadata_close_pending = false;
    }
    if (s_ctx.upload_metadata_cleanup_pending) {
        esp_err_t cleanup_error = unlink_upload_artifact_locked(
            s_ctx.upload_metadata_path, NULL);
        if (cleanup_error != ESP_OK) {
            return cleanup_error;
        }
    }
    clear_upload_metadata_tracking_locked();
    return ESP_OK;
}

static esp_err_t open_upload_metadata_locked(
    const char *path, BYTE mode, tf_upload_metadata_role_t role)
{
    ESP_RETURN_ON_FALSE(
        role == TF_UPLOAD_METADATA_READ_IDENTITY ||
            role == TF_UPLOAD_METADATA_TEMPORARY_IDENTITY,
        ESP_ERR_INVALID_ARG, TAG, "invalid upload metadata role");
    ESP_RETURN_ON_FALSE(
        path != NULL &&
            strnlen(path, sizeof(s_ctx.upload_metadata_path)) <
                sizeof(s_ctx.upload_metadata_path),
        ESP_ERR_INVALID_SIZE, TAG, "upload metadata path too long");
    ESP_RETURN_ON_ERROR(service_upload_metadata_locked(), TAG,
                        "upload metadata close/cleanup still pending");
    FRESULT result = f_open(&s_ctx.upload_metadata, path, mode);
    if (result != FR_OK) {
        return fat_result(result);
    }
    s_ctx.upload_metadata_role = role;
    s_ctx.upload_metadata_open = true;
    s_ctx.upload_metadata_close_pending = false;
    s_ctx.upload_metadata_cleanup_pending = false;
    snprintf(s_ctx.upload_metadata_path,
             sizeof(s_ctx.upload_metadata_path), "%s", path);
    return ESP_OK;
}

static esp_err_t close_upload_metadata_locked(bool cleanup_temporary)
{
    if (!s_ctx.upload_metadata_open ||
        s_ctx.upload_metadata_role == TF_UPLOAD_METADATA_IDLE) {
        return ESP_ERR_INVALID_STATE;
    }
    if (cleanup_temporary &&
        s_ctx.upload_metadata_role !=
            TF_UPLOAD_METADATA_TEMPORARY_IDENTITY) {
        return ESP_ERR_INVALID_STATE;
    }
    s_ctx.upload_metadata_close_pending = true;
    FRESULT result = f_close(&s_ctx.upload_metadata);
    if (result != FR_OK) {
        /* A temp whose close failed is never eligible for rename on a later
         * call. Preserve its FIL and delete it after close eventually works. */
        if (s_ctx.upload_metadata_role ==
            TF_UPLOAD_METADATA_TEMPORARY_IDENTITY) {
            s_ctx.upload_metadata_cleanup_pending = true;
        }
        return fat_result(result);
    }
    s_ctx.upload_metadata_open = false;
    s_ctx.upload_metadata_close_pending = false;
    s_ctx.upload_metadata_cleanup_pending = cleanup_temporary;
    if (cleanup_temporary) {
        return service_upload_metadata_locked();
    }
    clear_upload_metadata_tracking_locked();
    return ESP_OK;
}

static esp_err_t queue_upload_metadata_cleanup_locked(const char *path)
{
    ESP_RETURN_ON_FALSE(
        path != NULL &&
            strnlen(path, sizeof(s_ctx.upload_metadata_path)) <
                sizeof(s_ctx.upload_metadata_path),
        ESP_ERR_INVALID_SIZE, TAG, "upload metadata cleanup path too long");
    ESP_RETURN_ON_ERROR(service_upload_metadata_locked(), TAG,
                        "previous upload metadata operation still pending");
    s_ctx.upload_metadata_role = TF_UPLOAD_METADATA_TEMPORARY_IDENTITY;
    s_ctx.upload_metadata_open = false;
    s_ctx.upload_metadata_close_pending = false;
    s_ctx.upload_metadata_cleanup_pending = true;
    snprintf(s_ctx.upload_metadata_path,
             sizeof(s_ctx.upload_metadata_path), "%s", path);
    return service_upload_metadata_locked();
}

static esp_err_t read_upload_identity_locked(
    const char *identity_path, uint64_t *size,
    char sha256_hex[JETKVM_UPLOAD_SHA256_HEX_LENGTH + 1U])
{
    FILINFO info;
    FRESULT result = f_stat(identity_path, &info);
    if (result != FR_OK) {
        return fat_result(result);
    }
    if ((info.fattrib & AM_DIR) != 0U ||
        (uint64_t)info.fsize != JETKVM_UPLOAD_IDENTITY_RECORD_SIZE) {
        return ESP_ERR_INVALID_STATE;
    }

    ESP_RETURN_ON_ERROR(
        open_upload_metadata_locked(
            identity_path, FA_READ | FA_OPEN_EXISTING,
            TF_UPLOAD_METADATA_READ_IDENTITY),
        TAG, "failed to open upload identity");
    uint8_t record[JETKVM_UPLOAD_IDENTITY_RECORD_SIZE];
    UINT read_size = 0U;
    result = f_read(&s_ctx.upload_metadata, record, sizeof(record), &read_size);
    esp_err_t read_error = result != FR_OK ? fat_result(result) :
                           read_size != sizeof(record) ? ESP_FAIL : ESP_OK;
    esp_err_t close_error = close_upload_metadata_locked(false);
    if (close_error != ESP_OK) {
        return close_error;
    }
    if (read_error != ESP_OK) {
        return read_error;
    }
    return jetkvm_upload_identity_decode(record, sizeof(record), size,
                                         sha256_hex) ?
               ESP_OK : ESP_ERR_INVALID_STATE;
}

/* Publish only through a fully written and synced same-directory temporary
 * file. FatFs rename does not replace an existing destination, so callers
 * must establish that the canonical sidecar is absent before entering. */
static esp_err_t publish_upload_identity_locked(
    const char *identity_path, const char *temporary_path, uint64_t size,
    const char *sha256_hex)
{
    uint8_t record[JETKVM_UPLOAD_IDENTITY_RECORD_SIZE];
    if (!jetkvm_upload_identity_encode(size, sha256_hex, record)) {
        return ESP_ERR_INVALID_ARG;
    }

    FILINFO info;
    FRESULT result = f_stat(identity_path, &info);
    if (result == FR_OK) {
        return ESP_ERR_INVALID_STATE;
    }
    if (!fat_path_missing(result)) {
        return fat_result(result);
    }
    ESP_RETURN_ON_ERROR(
        queue_upload_metadata_cleanup_locked(temporary_path), TAG,
        "failed to remove stale upload identity temporary file");

    ESP_RETURN_ON_ERROR(
        open_upload_metadata_locked(
            temporary_path, FA_WRITE | FA_CREATE_NEW,
            TF_UPLOAD_METADATA_TEMPORARY_IDENTITY),
        TAG, "failed to open upload identity temporary file");
    UINT written = 0U;
    result = f_write(&s_ctx.upload_metadata, record,
                     sizeof(record), &written);
    esp_err_t operation_error = result != FR_OK ? fat_result(result) :
        written != sizeof(record) ? ESP_FAIL : ESP_OK;
    if (operation_error == ESP_OK) {
        result = f_sync(&s_ctx.upload_metadata);
        operation_error = fat_result(result);
    }
    esp_err_t close_error = close_upload_metadata_locked(
        operation_error != ESP_OK);
    if (close_error != ESP_OK) {
        return close_error;
    }
    if (operation_error != ESP_OK) {
        return operation_error;
    }
    result = f_rename(temporary_path, identity_path);
    if (result != FR_OK) {
        esp_err_t cleanup_error = queue_upload_metadata_cleanup_locked(
            temporary_path);
        if (cleanup_error != ESP_OK) {
            ESP_LOGE(TAG,
                     "upload identity temporary cleanup failed after rename: %s",
                     esp_err_to_name(cleanup_error));
        }
        return fat_result(result);
    }
    return ESP_OK;
}

/* Existing non-empty data is resumable only with a valid persisted identity.
 * A real requested digest must match exactly. The reserved Go-compatibility
 * sentinel may reuse any valid same-size sidecar, while preserving its stored
 * digest as the effective live identity. Empty/no-payload orphans contain no
 * trusted prefix, so their stale sidecar can be replaced atomically. */
static esp_err_t ensure_upload_identity_locked(
    const char *identity_path, const char *temporary_path, uint64_t existing,
    uint64_t expected_size, const char *expected_sha256_hex,
    char effective_sha256_hex[JETKVM_UPLOAD_SHA256_HEX_LENGTH + 1U])
{
    FILINFO info;
    FRESULT result = f_stat(identity_path, &info);
    if (result == FR_OK) {
        uint64_t stored_size = 0U;
        char stored_sha256_hex[JETKVM_UPLOAD_SHA256_HEX_LENGTH + 1U];
        esp_err_t read_error = read_upload_identity_locked(
            identity_path, &stored_size, stored_sha256_hex);
        if (read_error == ESP_OK && jetkvm_upload_resume_identity_matches(
                expected_size, expected_sha256_hex,
                stored_size, stored_sha256_hex)) {
            esp_err_t cleanup =
                queue_upload_metadata_cleanup_locked(temporary_path);
            if (cleanup == ESP_OK) {
                jetkvm_upload_sha256_hex_normalize(
                    stored_sha256_hex, effective_sha256_hex);
            }
            return cleanup;
        }
        if (existing > 0U) {
            return read_error != ESP_OK ? read_error : ESP_ERR_INVALID_STATE;
        }
        if (read_error != ESP_OK && read_error != ESP_ERR_INVALID_STATE) {
            return read_error;
        }
        ESP_RETURN_ON_ERROR(
            unlink_upload_artifact_locked(identity_path, NULL), TAG,
            "failed to replace empty upload identity");
    } else if (!fat_path_missing(result)) {
        return fat_result(result);
    } else if (existing > 0U) {
        return ESP_ERR_INVALID_STATE;
    }

    esp_err_t publish = publish_upload_identity_locked(
        identity_path, temporary_path, expected_size, expected_sha256_hex);
    if (publish == ESP_OK) {
        jetkvm_upload_sha256_hex_normalize(
            expected_sha256_hex, effective_sha256_hex);
    }
    return publish;
}

static uint64_t logical_capacity_locked(void)
{
    const uint64_t block_size =
        s_ctx.mode == JETKVM_VIRTUAL_MEDIA_MODE_CDROM ? 2048U : 512U;
    return ((s_ctx.image_size + block_size - 1U) / block_size) * block_size;
}

static bool storage_image_writable_locked(void)
{
    return s_ctx.image_open && s_ctx.image_writable &&
           s_ctx.source == JETKVM_VIRTUAL_MEDIA_SOURCE_STORAGE &&
           s_ctx.mode == JETKVM_VIRTUAL_MEDIA_MODE_DISK &&
           s_ctx.image_size > 0U &&
           s_ctx.image_size % JETKVM_VIRTUAL_MEDIA_BLOCK_SIZE == 0U;
}

/* Caller owns s_ctx.mutex. A fixed-size image normally has no metadata change,
 * but FatFs and the SD card can still retain data until the host explicitly
 * flushes or ejects the LUN. */
static esp_err_t storage_sync_locked(void)
{
    if (!storage_image_writable_locked()) {
        return ESP_OK;
    }
    int64_t started_us = esp_timer_get_time();
    FRESULT result = s_ctx.image_dirty ? f_sync(&s_ctx.image) : FR_OK;
    uint32_t elapsed_us = (uint32_t)(esp_timer_get_time() - started_us);
    s_ctx.write_io_stats.sync_calls++;
    s_ctx.write_io_stats.sync_us += elapsed_us;
    if (elapsed_us > s_ctx.write_io_stats.max_sync_us) {
        s_ctx.write_io_stats.max_sync_us = elapsed_us;
    }
    if (result == FR_OK) {
        s_ctx.image_dirty = false;
        s_ctx.write_io_stats.dirty = false;
    } else {
        s_ctx.write_io_stats.sync_errors++;
    }
    return fat_result(result);
}

/* Caller owns s_ctx.mutex. This is the only local-image read primitive used by
 * both the foreground MSC callback and the read-ahead worker. */
static esp_err_t storage_read_locked(uint64_t offset, void *buffer, size_t size,
                                     UINT *read_size,
                                     uint32_t *seek_elapsed_us,
                                     uint32_t *read_elapsed_us)
{
    size_t readable = offset < s_ctx.image_size ?
        (size_t)((s_ctx.image_size - offset) < size ?
                     (s_ctx.image_size - offset) : size) : 0U;
    *read_size = 0U;
    *seek_elapsed_us = 0U;
    *read_elapsed_us = 0U;
    if (readable == 0U) {
        memset(buffer, 0, size);
        return ESP_OK;
    }

    FRESULT result = FR_OK;
    /* f_read advances FIL::fptr. The MSC/prefetch path is overwhelmingly
     * sequential, so avoid re-walking the exFAT cluster chain when it already
     * points at the requested byte. Random reads retain the full f_lseek path.
     * s_ctx.mutex serializes every access to this FIL. */
    if (f_tell(&s_ctx.image) != (FSIZE_t)offset) {
        int64_t seek_started_us = esp_timer_get_time();
        result = f_lseek(&s_ctx.image, (FSIZE_t)offset);
        *seek_elapsed_us =
            (uint32_t)(esp_timer_get_time() - seek_started_us);
    }
    if (result == FR_OK) {
        int64_t read_started_us = esp_timer_get_time();
        result = f_read(&s_ctx.image, buffer, (UINT)readable, read_size);
        *read_elapsed_us =
            (uint32_t)(esp_timer_get_time() - read_started_us);
    }
    esp_err_t error = fat_result(result);
    if (error == ESP_OK && *read_size == readable) {
        if (readable < size) {
            memset((uint8_t *)buffer + readable, 0, size - readable);
        }
        return ESP_OK;
    }
    memset(buffer, 0, size);
    return error != ESP_OK ? error : ESP_FAIL;
}

#if JETKVM_TF_MSC_PREFETCH
static void prefetch_invalidate_locked(void)
{
    s_ctx.prefetch_generation++;
    s_ctx.prefetch_job_pending = false;
    s_ctx.prefetch_ready = false;
    s_ctx.prefetch_ready_size = 0U;
    s_ctx.prefetch_job_scheduled_us = 0;
    s_ctx.prefetch_ready_us = 0;
}

#if JETKVM_TF_UPLOAD_COPY_PIPELINE
/* This worker hand-off is intentionally independent of s_ctx.mutex: the
 * upload caller keeps that mutex across f_write(), while the prefetch task
 * copies only between caller-owned PSRAM and the inactive staging half. The
 * atomic state publishes the pointer triplet and the static semaphore keeps
 * the source buffer alive until the copy has definitely completed. */
static bool upload_copy_service(void)
{
    uint32_t expected = TF_UPLOAD_COPY_READY;
    if (!atomic_compare_exchange_strong_explicit(
            &s_ctx.upload_copy_state, &expected, TF_UPLOAD_COPY_RUNNING,
            memory_order_acquire, memory_order_relaxed)) {
        return false;
    }

    const uint8_t *source = s_ctx.upload_copy_source;
    uint8_t *destination = s_ctx.upload_copy_destination;
    size_t size = s_ctx.upload_copy_size;
    int64_t started_us = esp_timer_get_time();
    memcpy(destination, source, size);
    uint64_t elapsed_us = (uint64_t)(esp_timer_get_time() - started_us);
    atomic_store_explicit(
        &s_ctx.upload_copy_elapsed_us,
        elapsed_us > UINT32_MAX ? UINT32_MAX : (uint32_t)elapsed_us,
        memory_order_relaxed);
    atomic_store_explicit(&s_ctx.upload_copy_state, TF_UPLOAD_COPY_IDLE,
                          memory_order_release);
    (void)xSemaphoreGive(s_ctx.upload_copy_done);
    return true;
}

/* Caller owns s_ctx.mutex and there can be only one upload writer token. */
static bool upload_copy_schedule_locked(uint8_t *destination,
                                        const uint8_t *source, size_t size)
{
    if (s_ctx.prefetch_task == NULL || s_ctx.upload_copy_done == NULL ||
        destination == NULL || source == NULL || size == 0U ||
        atomic_load_explicit(&s_ctx.upload_copy_state,
                             memory_order_acquire) != TF_UPLOAD_COPY_IDLE) {
        return false;
    }
    while (xSemaphoreTake(s_ctx.upload_copy_done, 0) == pdTRUE) {
        /* A completed job is normally consumed by upload_copy_wait_locked().
         * Draining defensively keeps a stale completion from acknowledging a
         * newly published pointer triplet. */
    }
    s_ctx.upload_copy_source = source;
    s_ctx.upload_copy_destination = destination;
    s_ctx.upload_copy_size = size;
    atomic_store_explicit(&s_ctx.upload_copy_elapsed_us, 0U,
                          memory_order_relaxed);
    atomic_store_explicit(&s_ctx.upload_copy_state, TF_UPLOAD_COPY_READY,
                          memory_order_release);
    xTaskNotifyGive(s_ctx.prefetch_task);
    return true;
}

/* A scheduled copy must be joined even when the parallel f_write fails: its
 * source belongs to the transport and may be recycled as soon as this API
 * returns. The prefetch worker is persistent, so an unbounded join is the
 * only fail-closed lifetime rule. */
static uint32_t upload_copy_wait_locked(void)
{
    (void)xSemaphoreTake(s_ctx.upload_copy_done, portMAX_DELAY);
    return atomic_load_explicit(&s_ctx.upload_copy_elapsed_us,
                                memory_order_acquire);
}
#endif

static void prefetch_schedule_locked(uint64_t offset)
{
    if (s_ctx.prefetch_task == NULL || !s_ctx.image_open ||
        s_ctx.upload_owner_token !=
            JETKVM_VIRTUAL_MEDIA_UPLOAD_TOKEN_INVALID ||
        s_ctx.source != JETKVM_VIRTUAL_MEDIA_SOURCE_STORAGE) {
        return;
    }
    uint64_t capacity = logical_capacity_locked();
    if (offset >= capacity) {
        return;
    }
    uint64_t available = capacity - offset;
    size_t target_size = available < sizeof(s_tf_upload_dma_bounce) ?
                             (size_t)available :
                             sizeof(s_tf_upload_dma_bounce);
    if (!s_ctx.prefetch_ready || s_ctx.prefetch_ready_offset != offset ||
        s_ctx.prefetch_ready_size > target_size) {
        s_ctx.prefetch_ready_offset = offset;
        s_ctx.prefetch_ready_size = 0U;
        s_ctx.prefetch_ready = false;
    }
    if (s_ctx.prefetch_ready_size == target_size) {
        s_ctx.prefetch_job_pending = false;
        return;
    }
    s_ctx.prefetch_job_buffer_offset = s_ctx.prefetch_ready_size;
    s_ctx.prefetch_job_offset = offset + s_ctx.prefetch_ready_size;
    s_ctx.prefetch_job_size = target_size - s_ctx.prefetch_ready_size;
    s_ctx.prefetch_job_generation = s_ctx.prefetch_generation;
    s_ctx.prefetch_job_scheduled_us = esp_timer_get_time();
    s_ctx.prefetch_job_pending = true;
    xTaskNotifyGive(s_ctx.prefetch_task);
}

static void tf_prefetch_task(void *argument)
{
    (void)argument;
    for (;;) {
        (void)ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
        for (;;) {
#if JETKVM_TF_UPLOAD_COPY_PIPELINE
            if (upload_copy_service()) {
                continue;
            }
#endif
            int64_t worker_run_us = esp_timer_get_time();
            while (xSemaphoreTake(s_ctx.mutex, 0) != pdTRUE) {
#if JETKVM_TF_UPLOAD_COPY_PIPELINE
                if (upload_copy_service()) {
                    worker_run_us = esp_timer_get_time();
                    continue;
                }
#endif
                /* Never block indefinitely behind an upload writer: that
                 * writer may shortly publish a copy job which this same task
                 * must service before f_write() can advance to the next half.
                 * A notification wakes the poll immediately. */
                (void)ulTaskNotifyTake(pdTRUE, TF_PREFETCH_MUTEX_POLL_TICKS);
            }
            int64_t lock_acquired_us = esp_timer_get_time();
            if (!s_ctx.prefetch_job_pending) {
                unlock_context();
                break;
            }

            uint64_t offset = s_ctx.prefetch_job_offset;
            size_t size = s_ctx.prefetch_job_size;
            size_t buffer_offset = s_ctx.prefetch_job_buffer_offset;
            uint32_t generation = s_ctx.prefetch_job_generation;
            int64_t scheduled_us = s_ctx.prefetch_job_scheduled_us;
            s_ctx.prefetch_job_pending = false;
            if (!s_ctx.image_open ||
                s_ctx.source != JETKVM_VIRTUAL_MEDIA_SOURCE_STORAGE ||
                s_ctx.upload_owner_token !=
                    JETKVM_VIRTUAL_MEDIA_UPLOAD_TOKEN_INVALID ||
                generation != s_ctx.prefetch_generation || size == 0U ||
                buffer_offset > sizeof(s_tf_upload_dma_bounce) ||
                size > sizeof(s_tf_upload_dma_bounce) - buffer_offset) {
                unlock_context();
                continue;
            }

            int64_t started_us = lock_acquired_us;
            uint32_t schedule_delay_us =
                scheduled_us > 0 && worker_run_us >= scheduled_us ?
                    (uint32_t)(worker_run_us - scheduled_us) : 0U;
            uint32_t worker_lock_wait_us =
                lock_acquired_us >= worker_run_us ?
                    (uint32_t)(lock_acquired_us - worker_run_us) : 0U;
            UINT read_size = 0U;
            uint32_t seek_us = 0U;
            uint32_t read_us = 0U;
            esp_err_t error = storage_read_locked(
                offset, s_tf_upload_dma_bounce + buffer_offset, size,
                &read_size, &seek_us, &read_us);
            uint32_t elapsed_us =
                (uint32_t)(esp_timer_get_time() - started_us);
            s_ctx.io_stats.prefetch_jobs++;
            s_ctx.io_stats.prefetch_total_us += elapsed_us;
            s_ctx.io_stats.prefetch_seek_us += seek_us;
            s_ctx.io_stats.prefetch_read_us += read_us;
            s_ctx.io_stats.prefetch_bytes += read_size;
            s_ctx.io_stats.prefetch_schedule_delay_us += schedule_delay_us;
            s_ctx.io_stats.prefetch_worker_lock_wait_us +=
                worker_lock_wait_us;
            if (schedule_delay_us >
                s_ctx.io_stats.prefetch_max_schedule_delay_us) {
                s_ctx.io_stats.prefetch_max_schedule_delay_us =
                    schedule_delay_us;
            }
            if (worker_lock_wait_us >
                s_ctx.io_stats.prefetch_max_worker_lock_wait_us) {
                s_ctx.io_stats.prefetch_max_worker_lock_wait_us =
                    worker_lock_wait_us;
            }
            if (elapsed_us > s_ctx.io_stats.prefetch_max_us) {
                s_ctx.io_stats.prefetch_max_us = elapsed_us;
            }
            s_ctx.io_stats.prefetch_stack_high_watermark =
                (uint32_t)uxTaskGetStackHighWaterMark(NULL);
            if (error == ESP_OK && generation == s_ctx.prefetch_generation) {
                if (buffer_offset == 0U) {
                    s_ctx.prefetch_ready_offset = offset;
                }
                s_ctx.prefetch_ready_size = buffer_offset + size;
                s_ctx.prefetch_ready = true;
                s_ctx.prefetch_ready_us = esp_timer_get_time();
            } else {
                s_ctx.io_stats.prefetch_errors++;
                s_ctx.prefetch_ready = false;
                s_ctx.prefetch_ready_size = 0U;
                s_ctx.prefetch_ready_us = 0;
            }
            unlock_context();
        }
    }
}

static void prefetch_start(void)
{
    if (s_ctx.prefetch_task != NULL) {
        return;
    }
    BaseType_t created = xTaskCreatePinnedToCoreWithCaps(
        tf_prefetch_task, "tf_prefetch", TF_PREFETCH_TASK_STACK_BYTES, NULL,
        TF_PREFETCH_TASK_PRIORITY, &s_ctx.prefetch_task, TF_PREFETCH_TASK_CORE,
        MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (created != pdPASS) {
        s_ctx.prefetch_task = NULL;
        ESP_LOGW(TAG, "TF MSC prefetch unavailable; using synchronous reads");
    } else {
        ESP_LOGI(TAG,
                 "TF MSC prefetch uses shared %u-byte DMA scratch; stack in PSRAM",
                 (unsigned)sizeof(s_tf_upload_dma_bounce));
#if JETKVM_TF_UPLOAD_COPY_PIPELINE
        ESP_LOGI(TAG,
                 "TF upload copy pipeline ready: one 32 KiB Internal/DMA "
                 "staging block split into 2 x 16 KiB");
#endif
    }
}
#endif

static esp_err_t mkdir_if_missing(const char *path)
{
    FRESULT result = f_mkdir(path);
    return result == FR_EXIST ? ESP_OK : fat_result(result);
}

static time_t fat_timestamp(WORD date, WORD time_value)
{
    struct tm value = {
        .tm_year = (int)((date >> 9U) & 0x7fU) + 80,
        .tm_mon = (int)((date >> 5U) & 0x0fU) - 1,
        .tm_mday = (int)(date & 0x1fU),
        .tm_hour = (int)((time_value >> 11U) & 0x1fU),
        .tm_min = (int)((time_value >> 5U) & 0x3fU),
        .tm_sec = (int)(time_value & 0x1fU) * 2,
        .tm_isdst = -1,
    };
    return mktime(&value);
}

static bool filename_has_suffix_ascii_case_insensitive(
    const char *filename, const char *suffix)
{
    return jetkvm_upload_filename_has_suffix_ci(filename, suffix);
}

static bool filename_is_upload_identity_metadata(const char *filename)
{
    return filename_has_suffix_ascii_case_insensitive(
               filename, TF_UPLOAD_INCOMPLETE_SUFFIX
                         TF_UPLOAD_IDENTITY_SUFFIX) ||
           filename_has_suffix_ascii_case_insensitive(
               filename, TF_UPLOAD_INCOMPLETE_SUFFIX
                         TF_UPLOAD_IDENTITY_SUFFIX
                         TF_UPLOAD_IDENTITY_TEMP_SUFFIX);
}

static bool image_fits_lun(uint64_t size, jetkvm_virtual_media_mode_t mode)
{
    uint64_t block_size =
        mode == JETKVM_VIRTUAL_MEDIA_MODE_CDROM ? 2048U : 512U;
    return size > 0U && ((size - 1U) / block_size + 1U) <= UINT32_MAX;
}

static esp_err_t get_space_locked(jetkvm_virtual_media_space_t *space)
{
    char drive[5];
    snprintf(drive, sizeof(drive), "%u:", (unsigned)s_ctx.drive);
    DWORD free_clusters = 0U;
    FATFS *filesystem = NULL;
    FRESULT result = f_getfree(drive, &free_clusters, &filesystem);
    if (result != FR_OK) {
        return fat_result(result);
    }
    if (filesystem == NULL || s_ctx.card == NULL) {
        return ESP_FAIL;
    }
    uint64_t cluster_bytes =
        (uint64_t)filesystem->csize * (uint64_t)s_ctx.card->csd.sector_size;
    uint64_t total_clusters = filesystem->n_fatent - 2U;
    space->bytes_free = cluster_bytes * free_clusters;
    space->bytes_used = cluster_bytes * (total_clusters - free_clusters);
    return ESP_OK;
}

#if JETKVM_TF_CARD_TEST
/* Bench builds: a card that the test has taken out reads as absent. */
static _Atomic bool s_card_test_removed;
#endif

/* GPIO45, the socket's card-detect switch, is low with a card in the slot. */
static bool card_detected(void)
{
#if JETKVM_TF_CARD_TEST
    if (atomic_load(&s_card_test_removed)) {
        return false;
    }
#endif
    return gpio_get_level(TF_CARD_DETECT_GPIO) == 0;
}

bool jetkvm_virtual_media_card_in_slot(void)
{
    return card_detected();
}

/* Powers the slot through a reset, reads the card-detect pin and mounts the
 * card in the slot. Caller owns s_ctx.mutex. */
static esp_err_t init_storage_locked(void)
{
    /*
     * On-chip LDO channel 4 (LDO_VO4) powers the VDD_IO_5 domain behind
     * GPIO39..48.  LDO_VO4 defaults to 1.8 V, while this board's SD
     * socket and external pull-ups use 3.3 V.  Raise the pad domain before
     * touching GPIO39..46; otherwise the card cannot recognize CLK/CMD highs.
     * Keep the official SD power-control handle so sdmmc_card_init() can
     * reaffirm host.io_voltage before issuing commands.
     */
    if (s_ctx.io_power == NULL) {
        const sd_pwr_ctrl_ldo_config_t ldo_config = {
            .ldo_chan_id = 4,
        };
        esp_err_t ldo_error =
            sd_pwr_ctrl_new_on_chip_ldo(&ldo_config, &s_ctx.io_power);
        if (ldo_error != ESP_OK) {
            ESP_LOGE(TAG, "failed to enable SD VDD_IO_5 LDO4 at 3.3 V: %s",
                     esp_err_to_name(ldo_error));
            return ldo_error;
        }
        ESP_LOGI(TAG,
                 "TF IO voltage ready: LDO_VO4/VDD_IO_5=3300 mV for GPIO39..48");
        vTaskDelay(pdMS_TO_TICKS(2));
    }

    /* Clamp every card signal before opening Q1. The board's five 51 kOhm
     * pulls use the separately controlled LDO_VO4 signalling rail and could
     * otherwise back-power an unpowered card through its IO diodes. */
    gpio_config_t bus_low = {
        .pin_bit_mask = TF_BUS_PIN_MASK,
        .mode = GPIO_MODE_INPUT_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    esp_err_t error = gpio_config(&bus_low);
    if (error != ESP_OK) {
        return error;
    }
    const gpio_num_t bus_pins[] = {
        TF_PIN_CLK, TF_PIN_CMD, TF_PIN_D0,
        TF_PIN_D1, TF_PIN_D2, TF_PIN_D3,
    };
    for (size_t i = 0; i < sizeof(bus_pins) / sizeof(bus_pins[0]); ++i) {
        error = gpio_set_level(bus_pins[i], 0);
        if (error != ESP_OK) {
            return error;
        }
    }
    ESP_LOGI(TAG, "TF bus forced low before power-off clk/cmd/d0/d1/d2/d3=%d%d%d%d%d%d",
             gpio_get_level(TF_PIN_CLK), gpio_get_level(TF_PIN_CMD),
             gpio_get_level(TF_PIN_D0), gpio_get_level(TF_PIN_D1),
             gpio_get_level(TF_PIN_D2), gpio_get_level(TF_PIN_D3));

    /* Q1 is a high-side P-MOS: GPIO46 high removes card VDD, low enables it. */
    gpio_config_t power = {
        .pin_bit_mask = 1ULL << TF_POWER_GPIO,
        .mode = GPIO_MODE_INPUT_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    error = gpio_config(&power);
    if (error != ESP_OK) {
        return error;
    }
#if JETKVM_TF_Q1_RC_TEST
    error = gpio_set_level(TF_POWER_GPIO, 0);
    if (error != ESP_OK) {
        return error;
    }
    vTaskDelay(pdMS_TO_TICKS(TF_POWER_LOW_DELAY_MS));
    run_tf_q1_rc_test("powered");
#endif

    error = gpio_set_level(TF_POWER_GPIO, 1);
    if (error != ESP_OK) {
        return error;
    }
    ESP_LOGI(TAG, "TF power off GPIO46=%d discharge=%d ms",
             gpio_get_level(TF_POWER_GPIO), TF_POWER_OFF_DELAY_MS);
    vTaskDelay(pdMS_TO_TICKS(TF_POWER_OFF_DELAY_MS));
#if JETKVM_TF_Q1_RC_TEST
    run_tf_q1_rc_test("off_5s");
#endif
    error = gpio_set_level(TF_POWER_GPIO, 0);
    if (error != ESP_OK) {
        return error;
    }
    ESP_LOGI(TAG, "TF power on GPIO46=%d bus-low hold=%d ms",
             gpio_get_level(TF_POWER_GPIO), TF_POWER_LOW_DELAY_MS);
    vTaskDelay(pdMS_TO_TICKS(TF_POWER_LOW_DELAY_MS));

    for (size_t i = 0; i < sizeof(bus_pins) / sizeof(bus_pins[0]); ++i) {
        gpio_reset_pin(bus_pins[i]);
        gpio_set_direction(bus_pins[i], GPIO_MODE_INPUT);
        gpio_pullup_dis(bus_pins[i]);
        gpio_pulldown_dis(bus_pins[i]);
    }
    vTaskDelay(pdMS_TO_TICKS(TF_POWER_RELEASE_DELAY_MS));
    ESP_LOGI(TAG,
             "TF bus released %d ms external-idle clk/cmd/d0/d1/d2/d3=%d%d%d%d%d%d",
             TF_POWER_RELEASE_DELAY_MS,
             gpio_get_level(TF_PIN_CLK), gpio_get_level(TF_PIN_CMD),
             gpio_get_level(TF_PIN_D0), gpio_get_level(TF_PIN_D1),
             gpio_get_level(TF_PIN_D2), gpio_get_level(TF_PIN_D3));

    gpio_config_t detect = {
        .pin_bit_mask = 1ULL << TF_CARD_DETECT_GPIO,
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    error = gpio_config(&detect);
    if (error != ESP_OK) {
        return error;
    }
    s_ctx.card_present = card_detected();
    if (!s_ctx.card_present) {
        s_ctx.initialized = true;
        ESP_LOGI(TAG, "HTTP virtual-media backend ready; no TF card present");
        return ESP_OK;
    }

    /* GPIO39..44 are slot 0's dedicated pins. Slot 1 deliberately routes the
     * same pins through the GPIO matrix and is retained as a board-level A/B:
     * slot 0 also depends on the ESP32-P4 VDDPST_5 / SD_VREF domain. */
    sdmmc_host_t host = SDMMC_HOST_DEFAULT();
    host.slot = JETKVM_TF_SDMMC_SLOT;
    host.max_freq_khz = JETKVM_TF_SDMMC_FREQ_KHZ;
    const char *input_delay_policy = configure_tf_input_delay(&host);
    host.pwr_ctrl_handle = s_ctx.io_power;
    /* FatFs metadata buffers are not guaranteed to satisfy the SDMMC DMA
     * alignment contract. Reserve the driver's one-sector bounce buffer at
     * card init so directory/FAT I/O never needs a fallible heap allocation
     * while HDMI/WebRTC has internal SRAM under pressure. Aligned bulk image
     * transfers still DMA directly to/from PSRAM. */
    host.flags |= SDMMC_HOST_FLAG_ALLOC_ALIGNED_BUF;
    host.flags &= ~SDMMC_HOST_FLAG_8BIT;
#if JETKVM_TF_SDMMC_WIDTH == 1
    host.flags &= ~SDMMC_HOST_FLAG_4BIT;
#endif
    sdmmc_slot_config_t slot = SDMMC_SLOT_CONFIG_DEFAULT();
    slot.clk = TF_PIN_CLK;
    slot.cmd = TF_PIN_CMD;
    slot.d0 = TF_PIN_D0;
    slot.d1 = TF_PIN_D1;
    slot.d2 = TF_PIN_D2;
    slot.d3 = TF_PIN_D3;
    slot.width = JETKVM_TF_SDMMC_WIDTH;
#if JETKVM_TF_SDMMC_FREQ_KHZ > SDMMC_FREQ_HIGHSPEED
    /* The board connects CMD/DAT pull-ups and VDD_IO_5 to adjustable
     * LDO_VO4. The protocol layer may therefore execute CMD11 and lower the
     * complete host/pull-up domain from 3.3 V to 1.8 V for UHS-I. */
    slot.flags |= SDMMC_SLOT_FLAG_UHS1;
#endif
#if JETKVM_TF_INTERNAL_PULLUPS
    slot.flags |= SDMMC_SLOT_FLAG_INTERNAL_PULLUP;
#endif
    esp_vfs_fat_sdmmc_mount_config_t mount = {
        .format_if_mount_failed = false,
        .max_files = 12,
        .allocation_unit_size = 0,
    };
    sdmmc_card_t *card = NULL;
    ESP_LOGI(TAG,
             "TF native SDMMC slot=%d route=%s "
             "pins=GPIO43/44/39/40/41/42 width=%d clock=%d kHz "
             "uhs1=%s internal_pullups=%s input_delay=%s driver_phase=%d",
             JETKVM_TF_SDMMC_SLOT,
             JETKVM_TF_SDMMC_SLOT == 0 ? "iomux" : "gpio-matrix",
             JETKVM_TF_SDMMC_WIDTH, JETKVM_TF_SDMMC_FREQ_KHZ,
             JETKVM_TF_SDMMC_FREQ_KHZ > SDMMC_FREQ_HIGHSPEED ? "on" : "off",
             JETKVM_TF_INTERNAL_PULLUPS ? "on" : "off",
             input_delay_policy, (int)host.input_delay_phase);
#if JETKVM_TF_RAW_BOOT_SELFTEST
    /* The raw qualification path deliberately never registers a disk with
     * FatFs. This permits read-only testing of blank, corrupt, or unknown
     * cards without changing a single sector. */
    bool raw_host_initialized = false;
    card = calloc(1, sizeof(*card));
    error = card == NULL ? ESP_ERR_NO_MEM : host.init();
    if (error == ESP_OK) {
        raw_host_initialized = true;
        error = sdmmc_host_init_slot(host.slot, &slot);
    }
    if (error == ESP_OK) {
        error = sdmmc_card_init(&host, card);
    }
    if (error == ESP_OK) {
        ESP_LOGI(TAG,
                 "TF raw card negotiated width=%u real_clock=%d kHz "
                 "card_max=%" PRIu32 " kHz uhs1=%u io_voltage=%s "
                 "capacity=%" PRIu64 " bytes sector=%" PRIu32,
                 1U << card->log_bus_width, card->real_freq_khz,
                 card->max_freq_khz, card->is_uhs1,
                 card->is_uhs1 ? "1.8V" : "3.3V",
                 (uint64_t)card->csd.capacity * card->csd.sector_size,
                 card->csd.sector_size);
        ESP_LOGI(TAG,
                 "TF raw card identity name=%s mid=0x%02x oid=0x%04x "
                 "rev=0x%02x serial=0x%08x ocr=0x%08" PRIx32 " s18a=%u",
                 card->cid.name, card->cid.mfg_id, card->cid.oem_id,
                 card->cid.revision, card->cid.serial, card->ocr,
                 (card->ocr & SD_OCR_S18_RA) != 0);
        error = run_tf_raw_boot_selftest(card);
    }
    if (card != NULL && card->host.dma_aligned_buffer != NULL) {
        heap_caps_free(card->host.dma_aligned_buffer);
        card->host.dma_aligned_buffer = NULL;
    }
    if (raw_host_initialized) {
        (void)host.deinit_p(host.slot);
    }
    free(card);
    card = NULL;
    s_ctx.initialized = true;
    ESP_LOGI(TAG,
             "HTTP virtual-media backend ready; read-only raw TF self-test %s: %s",
             error == ESP_OK ? "passed" : "failed", esp_err_to_name(error));
    return ESP_OK;
#endif
#if JETKVM_TF_RMT_TRACE
    /* Keep the host and GPIO routing alive until diagnostics are captured.
     * The convenience mount helper tears both down on card-init failure. */
    bool host_initialized = false;
    card = calloc(1, sizeof(*card));
    error = card == NULL ? ESP_ERR_NO_MEM : host.init();
    if (error == ESP_OK) {
        host_initialized = true;
        error = sdmmc_host_init_slot(host.slot, &slot);
    }
    esp_err_t trace_error = ESP_ERR_INVALID_STATE;
    if (error == ESP_OK) {
        trace_error = tf_sd_trace_start();
        if (trace_error != ESP_OK) {
            ESP_LOGE(TAG, "TF passive trace unavailable: %s",
                     esp_err_to_name(trace_error));
        }
        error = sdmmc_card_init(&host, card);
    }
    dump_tf_host_state();
    tf_sd_trace_dump_and_stop();
    if (error == ESP_OK) {
        /* Diagnostics are only needed during bring-up. Re-enter through the
         * public mount helper if probing unexpectedly succeeds. */
        (void)host.deinit_p(host.slot);
        host_initialized = false;
        free(card);
        card = NULL;
        error = esp_vfs_fat_sdmmc_mount(STORAGE_MOUNT_POINT, &host, &slot,
                                        &mount, &card);
    }
    if (error != ESP_OK) {
        if (host_initialized) {
            (void)host.deinit_p(host.slot);
        }
        free(card);
        card = NULL;
    }
#else
    error = esp_vfs_fat_sdmmc_mount(STORAGE_MOUNT_POINT, &host, &slot,
                                    &mount, &card);
#endif
    if (error == ESP_OK) {
        s_ctx.filesystem_mounted = true;
        s_ctx.card = card;
        s_ctx.drive = ff_diskio_get_pdrv_card(card);
        ESP_LOGI(TAG,
                 "TF card negotiated width=%u real_clock=%d kHz card_max=%" PRIu32
                 " kHz uhs1=%u io_voltage=%s"
                 " capacity=%" PRIu64 " bytes sector=%" PRIu32
                 " discard=%u au=%" PRIu32 "KiB erase_size=%" PRIu32 "AU",
                 1U << card->log_bus_width, card->real_freq_khz,
                 card->max_freq_khz, card->is_uhs1,
                 card->is_uhs1 ? "1.8V" : "3.3V",
                 (uint64_t)card->csd.capacity * card->csd.sector_size,
                 card->csd.sector_size, card->ssr.discard_support,
                 card->ssr.alloc_unit_kb, card->ssr.erase_size_au);
        ESP_LOGI(TAG,
                 "TF card identity name=%s mid=0x%02x oid=0x%04x rev=0x%02x"
                 " serial=0x%08x ocr=0x%08" PRIx32 " s18a=%u",
                 card->cid.name, card->cid.mfg_id, card->cid.oem_id,
                 card->cid.revision, card->cid.serial, card->ocr,
                 (card->ocr & SD_OCR_S18_RA) != 0);
        error = s_ctx.drive < FF_VOLUMES ? ESP_OK : ESP_ERR_INVALID_STATE;
    }
    if (error == ESP_OK) {
        char directory[24];
        snprintf(directory, sizeof(directory), "%u:/jetkvm",
                 (unsigned)s_ctx.drive);
        error = mkdir_if_missing(directory);
    }
    if (error == ESP_OK) {
        char directory[32];
        snprintf(directory, sizeof(directory), "%u:%s",
                 (unsigned)s_ctx.drive, FAT_IMAGES_FOLDER);
        error = mkdir_if_missing(directory);
    }
#if JETKVM_TF_BOOT_SELFTEST
    if (error == ESP_OK) {
        (void)run_tf_boot_selftest();
        (void)run_tf_throughput_benchmark();
    }
#endif
    if (error == ESP_OK) {
        jetkvm_virtual_media_space_t space = {0};
        esp_err_t space_error = get_space_locked(&space);
        if (space_error == ESP_OK) {
            ESP_LOGI(TAG, "TF filesystem space used=%" PRIu64
                          " free=%" PRIu64 " bytes",
                     space.bytes_used, space.bytes_free);
        } else {
            ESP_LOGW(TAG, "TF filesystem space query failed: %s",
                     esp_err_to_name(space_error));
        }
    }
    if (error != ESP_OK) {
        if (s_ctx.filesystem_mounted) {
            (void)esp_vfs_fat_sdcard_unmount(STORAGE_MOUNT_POINT, card);
            s_ctx.filesystem_mounted = false;
            s_ctx.card = NULL;
        }
        s_ctx.initialized = true;
        ESP_LOGW(TAG, "HTTP virtual-media backend ready; TF storage unavailable: %s",
                 esp_err_to_name(error));
        return ESP_OK;
    }
    s_ctx.initialized = true;
    ESP_LOGI(TAG, "TF images storage ready at %s (read-only USB media)",
             IMAGES_FOLDER);
    return ESP_OK;
}

static void start_card_detect_locked(void);

esp_err_t jetkvm_virtual_media_init(void)
{
    if (s_ctx.mutex == NULL) {
        s_ctx.mutex = xSemaphoreCreateMutexStatic(&s_ctx.mutex_storage);
    }
    ESP_RETURN_ON_FALSE(s_ctx.mutex != NULL, ESP_ERR_NO_MEM, TAG,
                        "failed to create mutex");
#if JETKVM_TF_MSC_PREFETCH
#if JETKVM_TF_UPLOAD_COPY_PIPELINE
    if (s_ctx.upload_copy_done == NULL) {
        atomic_init(&s_ctx.upload_copy_state, TF_UPLOAD_COPY_IDLE);
        atomic_init(&s_ctx.upload_copy_elapsed_us, 0U);
        s_ctx.upload_copy_done = xSemaphoreCreateBinaryStatic(
            &s_ctx.upload_copy_done_storage);
    }
#endif
    prefetch_start();
#endif
    ESP_RETURN_ON_ERROR(lock_context(), TAG, "failed to lock");
    if (s_ctx.initialized) {
        unlock_context();
        return ESP_OK;
    }
#if JETKVM_TF_SD_ACMD23
    sdmmc_acmd23_reset_stats();
    sdmmc_acmd23_set_enabled(true);
    ESP_LOGI(TAG, "TF SD ACMD23 pre-erase hints enabled");
#else
    ESP_LOGI(TAG, "TF SD ACMD23 pre-erase hints disabled");
#endif
    esp_err_t error = init_storage_locked();
    if (error == ESP_OK) {
        start_card_detect_locked();
    }
    unlock_context();
    return error;
}

/* A card whose filesystem did not mount at boot has no registered disk. The
 * public helper initializes the card and creates a full-card partition (it
 * formats only because the mount failed), which the caller then remakes as
 * exFAT. */
static esp_err_t register_unmounted_card_locked(void)
{
    sdmmc_host_t host = SDMMC_HOST_DEFAULT();
    host.slot = JETKVM_TF_SDMMC_SLOT;
    host.max_freq_khz = JETKVM_TF_SDMMC_FREQ_KHZ;
    (void)configure_tf_input_delay(&host);
    host.pwr_ctrl_handle = s_ctx.io_power;
    host.flags |= SDMMC_HOST_FLAG_ALLOC_ALIGNED_BUF;
    host.flags &= ~SDMMC_HOST_FLAG_8BIT;
#if JETKVM_TF_SDMMC_WIDTH == 1
    host.flags &= ~SDMMC_HOST_FLAG_4BIT;
#endif

    sdmmc_slot_config_t slot = SDMMC_SLOT_CONFIG_DEFAULT();
    slot.clk = TF_PIN_CLK;
    slot.cmd = TF_PIN_CMD;
    slot.d0 = TF_PIN_D0;
    slot.d1 = TF_PIN_D1;
    slot.d2 = TF_PIN_D2;
    slot.d3 = TF_PIN_D3;
    slot.width = JETKVM_TF_SDMMC_WIDTH;
#if JETKVM_TF_SDMMC_FREQ_KHZ > SDMMC_FREQ_HIGHSPEED
    slot.flags |= SDMMC_SLOT_FLAG_UHS1;
#endif
#if JETKVM_TF_INTERNAL_PULLUPS
    slot.flags |= SDMMC_SLOT_FLAG_INTERNAL_PULLUP;
#endif

    const esp_vfs_fat_sdmmc_mount_config_t mount = {
        .format_if_mount_failed = true,
        .max_files = 12,
        .allocation_unit_size = 64U * 1024U,
        .use_one_fat = false,
    };
    sdmmc_card_t *card = NULL;
    esp_err_t error = esp_vfs_fat_sdmmc_mount(
        STORAGE_MOUNT_POINT, &host, &slot, &mount, &card);
    if (error != ESP_OK) {
        ESP_LOGE(TAG, "failed to initialize TF card for formatting: %s",
                 esp_err_to_name(error));
        return error;
    }
    s_ctx.drive = ff_diskio_get_pdrv_card(card);
    if (s_ctx.drive >= FF_VOLUMES) {
        (void)esp_vfs_fat_sdcard_unmount(STORAGE_MOUNT_POINT, card);
        return ESP_ERR_INVALID_STATE;
    }
    s_ctx.card = card;
    s_ctx.filesystem_mounted = true;
    return ESP_OK;
}

jetkvm_virtual_media_format_result_t jetkvm_virtual_media_format_exfat(
    jetkvm_virtual_media_space_t *space)
{
    if (space == NULL || lock_context() != ESP_OK) {
        return JETKVM_VIRTUAL_MEDIA_FORMAT_FAILED;
    }
    /* The card-detect task notices a removal within half a second; check
     * the pin once more immediately before the destructive operation. The
     * task owns card_present and ends what uses a removed card. */
    const jetkvm_storage_format_inputs_t inputs = {
        .initialized = s_ctx.initialized,
        .card_present = s_ctx.card_present && card_detected(),
        .image_mounted = s_ctx.image_open,
        .upload_has_writer =
            s_ctx.upload_owner_token !=
                JETKVM_VIRTUAL_MEDIA_UPLOAD_TOKEN_INVALID ||
            s_ctx.upload_claim_pending || s_ctx.upload_staging != NULL,
        .upload_close_failed = s_ctx.upload_close_pending,
    };
    jetkvm_virtual_media_format_result_t admitted =
        jetkvm_storage_format_admit(&inputs);
    if (admitted != JETKVM_VIRTUAL_MEDIA_FORMAT_COMPLETED) {
        unlock_context();
        return admitted;
    }

    esp_err_t error = ESP_OK;
    if (s_ctx.filesystem_mounted) {
        /* An upload without a writer and the upload identity files hold
         * FatFs objects of the old filesystem: close them first. */
        if (service_upload_metadata_locked() != ESP_OK ||
            close_idle_upload_locked() != ESP_OK) {
            unlock_context();
            return JETKVM_VIRTUAL_MEDIA_FORMAT_UPLOAD_ACTIVE;
        }
        ESP_LOGW(TAG, "formatting the mounted TF card as exFAT; its files "
                      "are erased");
    } else {
        ESP_LOGW(TAG, "formatting startup-detected TF card as exFAT");
        error = register_unmounted_card_locked();
    }
    sdmmc_card_t *card = s_ctx.card;

    /* The VFS registration stays; only the FatFs volume under it is
     * unmounted, remade and mounted again on the same FATFS object. */
    char drive[5] = {0};
    FATFS *filesystem = NULL;
    DWORD free_clusters = 0U;
    if (error == ESP_OK) {
        snprintf(drive, sizeof(drive), "%u:", (unsigned)s_ctx.drive);
        FRESULT result = f_getfree(drive, &free_clusters, &filesystem);
        error = result != FR_OK ? fat_result(result) :
                filesystem == NULL ? ESP_FAIL : ESP_OK;
        if (error == ESP_OK) {
            result = f_mount(NULL, drive, 0);
            error = fat_result(result);
            if (error == ESP_OK) {
                s_ctx.filesystem_mounted = false;
            }
        }
    }

    if (error == ESP_OK) {
        const MKFS_PARM format = {
            .fmt = FM_EXFAT,
            .n_fat = 2,
            .align = 0,
            .n_root = 0,
            .au_size = 64U * 1024U,
        };
        FRESULT result = f_mkfs(drive, &format, s_tf_upload_dma_bounce,
                                sizeof(s_tf_upload_dma_bounce));
        error = fat_result(result);
    }
    if (error == ESP_OK) {
        FRESULT result = f_mount(filesystem, drive, 1);
        error = fat_result(result);
        if (error == ESP_OK) {
            s_ctx.filesystem_mounted = true;
            if (filesystem->fs_type != FS_EXFAT) {
                error = ESP_ERR_INVALID_RESPONSE;
            }
        }
    }
    if (error == ESP_OK) {
        char directory[24];
        snprintf(directory, sizeof(directory), "%u:/jetkvm",
                 (unsigned)s_ctx.drive);
        error = mkdir_if_missing(directory);
    }
    if (error == ESP_OK) {
        char directory[32];
        snprintf(directory, sizeof(directory), "%u:%s",
                 (unsigned)s_ctx.drive, FAT_IMAGES_FOLDER);
        error = mkdir_if_missing(directory);
    }
    if (error == ESP_OK) {
        error = get_space_locked(space);
    }

    if (error != ESP_OK) {
        ESP_LOGE(TAG, "TF exFAT format failed: %s", esp_err_to_name(error));
        /* The volume was unmounted for f_mkfs: mount whatever the card holds
         * now, the old filesystem when f_mkfs did not run or wrote nothing,
         * so that a failure leaves a readable card mounted. */
        if (!s_ctx.filesystem_mounted && filesystem != NULL &&
            f_mount(filesystem, drive, 1) == FR_OK) {
            s_ctx.filesystem_mounted = true;
        }
        /* The card is dropped only when no filesystem on it mounts. */
        if (!s_ctx.filesystem_mounted) {
            if (card != NULL) {
                (void)esp_vfs_fat_sdcard_unmount(STORAGE_MOUNT_POINT, card);
            }
            s_ctx.card = NULL;
        }
        unlock_context();
        return JETKVM_VIRTUAL_MEDIA_FORMAT_FAILED;
    }
    ESP_LOGI(TAG,
             "TF exFAT format complete; used=%" PRIu64 " free=%" PRIu64
             " bytes",
             space->bytes_used, space->bytes_free);
    unlock_context();
    return JETKVM_VIRTUAL_MEDIA_FORMAT_COMPLETED;
}

/* Names the medium: every mount, and every unmount that closes an image,
 * advances it, under the context lock. */
static _Atomic uint32_t s_medium_generation;

uint32_t jetkvm_virtual_media_medium_generation(void)
{
    return atomic_load(&s_medium_generation);
}

esp_err_t jetkvm_virtual_media_get_state(jetkvm_virtual_media_state_t *state)
{
    ESP_RETURN_ON_FALSE(state != NULL, ESP_ERR_INVALID_ARG, TAG, "state null");
    ESP_RETURN_ON_ERROR(lock_context(), TAG, "failed to lock");
    memset(state, 0, sizeof(*state));
    state->initialized = s_ctx.initialized;
    state->card_present = s_ctx.card_present;
    state->card_generation = s_ctx.card_generation;
    state->filesystem_mounted = s_ctx.filesystem_mounted;
    state->image_mounted = s_ctx.image_open;
    state->source = s_ctx.source;
    state->mode = s_ctx.mode;
    state->medium_generation = atomic_load(&s_medium_generation);
    state->size = s_ctx.image_size;
    memcpy(state->filename, s_ctx.filename, sizeof(state->filename));
    memcpy(state->url, s_ctx.url, sizeof(state->url));
    unlock_context();
    return ESP_OK;
}

esp_err_t jetkvm_virtual_media_get_space(jetkvm_virtual_media_space_t *space)
{
    ESP_RETURN_ON_FALSE(space != NULL, ESP_ERR_INVALID_ARG, TAG, "space null");
    ESP_RETURN_ON_ERROR(lock_context(), TAG, "failed to lock");
    if (!s_ctx.filesystem_mounted) {
        unlock_context();
        return ESP_ERR_INVALID_STATE;
    }
    esp_err_t result = get_space_locked(space);
    unlock_context();
    return result;
}

esp_err_t jetkvm_virtual_media_list_files(jetkvm_virtual_media_file_list_t *list)
{
    ESP_RETURN_ON_FALSE(list != NULL, ESP_ERR_INVALID_ARG, TAG, "list null");
    ESP_RETURN_ON_ERROR(lock_context(), TAG, "failed to lock");
    memset(list, 0, sizeof(*list));
    if (!s_ctx.filesystem_mounted) {
        unlock_context();
        return ESP_ERR_INVALID_STATE;
    }
    esp_err_t metadata_error = service_upload_metadata_locked();
    if (metadata_error != ESP_OK) {
        unlock_context();
        return metadata_error;
    }
    char directory_path[32];
    snprintf(directory_path, sizeof(directory_path), "%u:%s",
             (unsigned)s_ctx.drive, FAT_IMAGES_FOLDER);
    FF_DIR directory;
    FRESULT result = f_opendir(&directory, directory_path);
    if (result != FR_OK) {
        unlock_context();
        return fat_result(result);
    }
    FILINFO entry;
    while ((result = f_readdir(&directory, &entry)) == FR_OK &&
           entry.fname[0] != '\0') {
        if ((entry.fattrib & AM_DIR) != 0U || strcmp(entry.fname, ".") == 0 ||
            strcmp(entry.fname, "..") == 0) {
            continue;
        }
        if (filename_is_upload_identity_metadata(entry.fname)) {
            /* Persistent upload identities are implementation metadata, not
             * mountable/listable virtual-media images. */
            continue;
        }
        if (list->count >= JETKVM_VIRTUAL_MEDIA_MAX_LISTED_FILES) {
            list->truncated = true;
            continue;
        }
        jetkvm_virtual_media_file_t *file = &list->files[list->count++];
        snprintf(file->filename, sizeof(file->filename), "%s", entry.fname);
        file->size = (uint64_t)entry.fsize;
        file->created_at = fat_timestamp(entry.fdate, entry.ftime);
    }
    FRESULT close_result = f_closedir(&directory);
    unlock_context();
    return result != FR_OK ? fat_result(result) : fat_result(close_result);
}

esp_err_t jetkvm_virtual_media_delete_file(const char *filename)
{
    char path[sizeof(IMAGES_FOLDER) +
              JETKVM_VIRTUAL_MEDIA_FILENAME_CAPACITY + 2U];
    ESP_RETURN_ON_ERROR(make_image_path(filename, path, sizeof(path)), TAG,
                        "invalid filename");
    ESP_RETURN_ON_FALSE(!filename_is_upload_identity_metadata(filename),
                        ESP_ERR_INVALID_ARG, TAG,
                        "reserved upload identity filename");
    ESP_RETURN_ON_ERROR(lock_context(), TAG, "failed to lock");
    if (!s_ctx.filesystem_mounted) {
        unlock_context();
        return ESP_ERR_INVALID_STATE;
    }
    esp_err_t metadata_error = service_upload_metadata_locked();
    if (metadata_error != ESP_OK) {
        unlock_context();
        return metadata_error;
    }
    /* FatFs is case-insensitive and FF_FS_LOCK is intentionally zero.
     * Reject every deletion while any long-lived FIL is open: comparing
     * user spelling cannot exclude case or Unicode aliases safely. */
    esp_err_t open_error = s_ctx.image_open ? ESP_ERR_INVALID_STATE :
                                              close_idle_upload_locked();
    if (open_error != ESP_OK) {
        unlock_context();
        return open_error;
    }
    FRESULT result = f_unlink(path);
    if (!filename_has_suffix_ascii_case_insensitive(
            filename, TF_UPLOAD_INCOMPLETE_SUFFIX)) {
        unlock_context();
        return fat_result(result);
    }

    /* An explicit deletion of an incomplete upload also removes both forms
     * of its hidden identity metadata. Missing payload/sidecar paths are
     * tolerated so a sidecar-first crash can still be cleaned up. */
    if (result != FR_OK && !fat_path_missing(result)) {
        unlock_context();
        return fat_result(result);
    }
    char identity_path[sizeof(s_ctx.upload_identity_path)];
    char temporary_path[sizeof(s_ctx.upload_identity_path) +
                        sizeof(TF_UPLOAD_IDENTITY_TEMP_SUFFIX)];
    esp_err_t path_error = append_path_suffix(
        path, TF_UPLOAD_IDENTITY_SUFFIX, identity_path,
        sizeof(identity_path));
    if (path_error == ESP_OK) {
        path_error = append_path_suffix(
            identity_path, TF_UPLOAD_IDENTITY_TEMP_SUFFIX,
            temporary_path, sizeof(temporary_path));
    }
    bool identity_removed = false;
    bool temporary_removed = false;
    if (path_error == ESP_OK) {
        path_error = unlink_upload_artifact_locked(
            identity_path, &identity_removed);
    }
    if (path_error == ESP_OK) {
        path_error = unlink_upload_artifact_locked(
            temporary_path, &temporary_removed);
    }
    unlock_context();
    if (path_error != ESP_OK) {
        return path_error;
    }
    return result == FR_OK || identity_removed || temporary_removed ?
               ESP_OK : ESP_ERR_NOT_FOUND;
}

esp_err_t jetkvm_virtual_media_file_size(const char *filename, uint64_t *size)
{
    ESP_RETURN_ON_FALSE(size != NULL, ESP_ERR_INVALID_ARG, TAG, "size null");
    char path[sizeof(IMAGES_FOLDER) +
              JETKVM_VIRTUAL_MEDIA_FILENAME_CAPACITY + 2U];
    ESP_RETURN_ON_ERROR(make_image_path(filename, path, sizeof(path)), TAG,
                        "invalid filename");
    ESP_RETURN_ON_ERROR(lock_context(), TAG, "failed to lock");
    if (!s_ctx.filesystem_mounted) {
        unlock_context();
        return ESP_ERR_INVALID_STATE;
    }
    FILINFO info;
    FRESULT result = f_stat(path, &info);
    unlock_context();
    if (fat_path_missing(result)) {
        return ESP_ERR_NOT_FOUND;
    }
    if (result != FR_OK) {
        return fat_result(result);
    }
    if ((info.fattrib & AM_DIR) != 0U) {
        return ESP_ERR_INVALID_STATE;
    }
    *size = (uint64_t)info.fsize;
    return ESP_OK;
}

bool jetkvm_virtual_media_upload_claimed(void)
{
    if (lock_context() != ESP_OK) {
        return true;
    }
    bool claimed = s_ctx.upload_owner_token !=
                       JETKVM_VIRTUAL_MEDIA_UPLOAD_TOKEN_INVALID ||
                   s_ctx.upload_claim_pending;
    unlock_context();
    return claimed;
}

esp_err_t jetkvm_virtual_media_mount(const char *filename,
                                    jetkvm_virtual_media_mode_t mode)
{
    if (mode != JETKVM_VIRTUAL_MEDIA_MODE_CDROM &&
        mode != JETKVM_VIRTUAL_MEDIA_MODE_DISK) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!valid_filename(filename) ||
        filename_has_suffix_ascii_case_insensitive(filename, ".incomplete") ||
        filename_is_upload_identity_metadata(filename)) {
        return ESP_ERR_INVALID_ARG;
    }
    char path[sizeof(IMAGES_FOLDER) +
              JETKVM_VIRTUAL_MEDIA_FILENAME_CAPACITY + 2U];
    ESP_RETURN_ON_ERROR(make_image_path(filename, path, sizeof(path)), TAG,
                        "invalid filename");
    ESP_RETURN_ON_ERROR(lock_context(), TAG, "failed to lock");
    if (!s_ctx.filesystem_mounted) {
        unlock_context();
        return ESP_ERR_INVALID_STATE;
    }
    esp_err_t error = service_upload_metadata_locked();
    if (error == ESP_OK) {
        error = s_ctx.image_open ? ESP_ERR_INVALID_STATE :
                                   close_idle_upload_locked();
    }
    if (error != ESP_OK) {
        unlock_context();
        return error;
    }
    FILINFO info;
    FRESULT result = f_stat(path, &info);
    if (result != FR_OK || (info.fattrib & AM_DIR) != 0U ||
        !image_fits_lun((uint64_t)info.fsize, mode)) {
        unlock_context();
        return result != FR_OK ? fat_result(result) : ESP_ERR_INVALID_SIZE;
    }
    BYTE open_mode = FA_READ | FA_OPEN_EXISTING;
    if (mode == JETKVM_VIRTUAL_MEDIA_MODE_DISK) {
        open_mode |= FA_WRITE;
    }
    result = f_open(&s_ctx.image, path, open_mode);
    if (result != FR_OK) {
        unlock_context();
        return fat_result(result);
    }
#if JETKVM_TF_MSC_PREFETCH
    prefetch_invalidate_locked();
#endif
    atomic_fetch_add(&s_medium_generation, 1U);
    s_ctx.image_open = true;
    s_ctx.image_writable = mode == JETKVM_VIRTUAL_MEDIA_MODE_DISK;
    s_ctx.image_dirty = false;
    s_ctx.source = JETKVM_VIRTUAL_MEDIA_SOURCE_STORAGE;
    s_ctx.mode = mode;
    s_ctx.image_size = (uint64_t)info.fsize;
    memset(&s_ctx.io_stats, 0, sizeof(s_ctx.io_stats));
    memset(&s_ctx.write_io_stats, 0, sizeof(s_ctx.write_io_stats));
    snprintf(s_ctx.filename, sizeof(s_ctx.filename), "%s", filename);
    s_ctx.url[0] = '\0';
    unlock_context();
    return ESP_OK;
}

static esp_err_t check_trimmed_url(
    const char *trimmed, jetkvm_virtual_media_url_info_t *info)
{
    jetkvm_http_range_check_t check;
    esp_err_t result = jetkvm_http_range_check(trimmed, &check);
    if (result != ESP_OK) {
        return result;
    }
    memset(info, 0, sizeof(*info));
    info->usable = check.usable;
    info->size = check.size;
    snprintf(info->reason, sizeof(info->reason), "%s", check.reason);
    return ESP_OK;
}

esp_err_t jetkvm_virtual_media_check_url(
    const char *url, jetkvm_virtual_media_url_info_t *info)
{
    ESP_RETURN_ON_FALSE(info != NULL, ESP_ERR_INVALID_ARG, TAG, "info null");
    char *trimmed = jetkvm_memory_policy_control_alloc(
        JETKVM_VIRTUAL_MEDIA_URL_CAPACITY);
    if (!trimmed) {
        return ESP_ERR_NO_MEM;
    }
    if (trim_url(url, trimmed, JETKVM_VIRTUAL_MEDIA_URL_CAPACITY) != ESP_OK) {
        jetkvm_memory_policy_control_free(trimmed);
        memset(info, 0, sizeof(*info));
        snprintf(info->reason, sizeof(info->reason),
                 "Enter a valid HTTP or HTTPS image URL.");
        return ESP_OK;
    }
    esp_err_t result = check_trimmed_url(trimmed, info);
    jetkvm_memory_policy_control_free(trimmed);
    return result;
}

esp_err_t jetkvm_virtual_media_mount_http(
    const char *url, jetkvm_virtual_media_mode_t mode)
{
    if (mode != JETKVM_VIRTUAL_MEDIA_MODE_CDROM &&
        mode != JETKVM_VIRTUAL_MEDIA_MODE_DISK) {
        return ESP_ERR_INVALID_ARG;
    }
    char *trimmed = jetkvm_memory_policy_control_alloc(
        JETKVM_VIRTUAL_MEDIA_URL_CAPACITY);
    if (!trimmed) {
        return ESP_ERR_NO_MEM;
    }
    esp_err_t result = trim_url(
        url, trimmed, JETKVM_VIRTUAL_MEDIA_URL_CAPACITY);
    if (result != ESP_OK) {
        jetkvm_memory_policy_control_free(trimmed);
        return result;
    }
    jetkvm_virtual_media_url_info_t info;
    result = check_trimmed_url(trimmed, &info);
    if (result != ESP_OK) {
        jetkvm_memory_policy_control_free(trimmed);
        return result;
    }
    if (!info.usable) {
        /* The server answered but not usably (404, no range support, ...).
         * Distinct from storage being unavailable: the RPC layer reports
         * upstream's "The URL is not available" to the UI. */
        jetkvm_memory_policy_control_free(trimmed);
        return ESP_ERR_INVALID_RESPONSE;
    }
    if (!image_fits_lun(info.size, mode)) {
        jetkvm_memory_policy_control_free(trimmed);
        return ESP_ERR_INVALID_SIZE;
    }
    jetkvm_http_range_reader_t *reader = NULL;
    result = jetkvm_http_range_reader_open(trimmed, &reader);
    if (result != ESP_OK) {
        jetkvm_memory_policy_control_free(trimmed);
        return result;
    }
    esp_err_t lock_error = lock_context();
    if (lock_error != ESP_OK) {
        jetkvm_http_range_reader_close(reader);
        jetkvm_memory_policy_control_free(trimmed);
        return lock_error;
    }
    if (!s_ctx.initialized || s_ctx.image_open) {
        unlock_context();
        jetkvm_http_range_reader_close(reader);
        jetkvm_memory_policy_control_free(trimmed);
        return ESP_ERR_INVALID_STATE;
    }
#if JETKVM_TF_MSC_PREFETCH
    prefetch_invalidate_locked();
#endif
    atomic_fetch_add(&s_medium_generation, 1U);
    s_ctx.http_reader = reader;
    s_ctx.image_open = true;
    s_ctx.image_writable = false;
    s_ctx.image_dirty = false;
    s_ctx.source = JETKVM_VIRTUAL_MEDIA_SOURCE_HTTP;
    s_ctx.mode = mode;
    s_ctx.image_size = info.size;
    memset(&s_ctx.io_stats, 0, sizeof(s_ctx.io_stats));
    memset(&s_ctx.write_io_stats, 0, sizeof(s_ctx.write_io_stats));
    s_ctx.filename[0] = '\0';
    snprintf(s_ctx.url, sizeof(s_ctx.url), "%s", trimmed);
    unlock_context();
    jetkvm_memory_policy_control_free(trimmed);
    return ESP_OK;
}

esp_err_t jetkvm_virtual_media_mount_builtin_installer(void)
{
    ESP_RETURN_ON_ERROR(lock_context(), TAG, "failed to lock");
    if (!s_ctx.initialized) {
        unlock_context();
        return ESP_ERR_NOT_SUPPORTED;
    }
    if (s_ctx.image_open) {
        unlock_context();
        return ESP_ERR_INVALID_STATE;
    }
#if JETKVM_TF_MSC_PREFETCH
    prefetch_invalidate_locked();
#endif
    atomic_fetch_add(&s_medium_generation, 1U);
    s_ctx.image_open = true;
    s_ctx.image_writable = false;
    s_ctx.image_dirty = false;
    s_ctx.source = JETKVM_VIRTUAL_MEDIA_SOURCE_BUILTIN;
    s_ctx.mode = JETKVM_VIRTUAL_MEDIA_MODE_CDROM;
    s_ctx.image_size = jetkvm_builtin_installer_image.size;
    memset(&s_ctx.io_stats, 0, sizeof(s_ctx.io_stats));
    memset(&s_ctx.write_io_stats, 0, sizeof(s_ctx.write_io_stats));
    snprintf(s_ctx.filename, sizeof(s_ctx.filename), "%s",
             JETKVM_VIRTUAL_MEDIA_BUILTIN_INSTALLER_FILENAME);
    s_ctx.url[0] = '\0';
    unlock_context();
    return ESP_OK;
}

/* Closes the mounted image. With a generation, only the medium of that
 * generation: another medium stays as it is, and the call fails. */
static esp_err_t unmount_medium(const uint32_t *medium_generation)
{
    ESP_RETURN_ON_ERROR(lock_context(), TAG, "failed to lock");
    if (medium_generation != NULL &&
        *medium_generation != atomic_load(&s_medium_generation)) {
        unlock_context();
        return ESP_ERR_INVALID_STATE;
    }
#if JETKVM_TF_MSC_PREFETCH
    prefetch_invalidate_locked();
#endif
    FRESULT result = FR_OK;
    if (s_ctx.image_open &&
        s_ctx.source == JETKVM_VIRTUAL_MEDIA_SOURCE_STORAGE) {
        /* f_close() already performs FatFs' terminal sync. Do not precede it
         * with f_sync(), which would duplicate the same metadata flush. */
        result = f_close(&s_ctx.image);
        if (result != FR_OK) {
            /* The patched FatFs close keeps the FIL live after an I/O error.
             * Preserve all mount state so a later flush/eject can retry. */
            unlock_context();
            return fat_result(result);
        }
    } else if (s_ctx.http_reader != NULL) {
        jetkvm_http_range_reader_close(s_ctx.http_reader);
        s_ctx.http_reader = NULL;
    }
    if (s_ctx.image_open) {
        atomic_fetch_add(&s_medium_generation, 1U);
    }
    s_ctx.image_open = false;
    s_ctx.image_writable = false;
    s_ctx.image_dirty = false;
    s_ctx.source = JETKVM_VIRTUAL_MEDIA_SOURCE_STORAGE;
    s_ctx.filename[0] = '\0';
    s_ctx.url[0] = '\0';
    s_ctx.image_size = 0U;
    s_ctx.mode = JETKVM_VIRTUAL_MEDIA_MODE_CDROM;
    unlock_context();
    return fat_result(result);
}

esp_err_t jetkvm_virtual_media_unmount(void)
{
    return unmount_medium(NULL);
}

esp_err_t jetkvm_virtual_media_eject(uint32_t medium_generation)
{
    return unmount_medium(&medium_generation);
}

esp_err_t jetkvm_virtual_media_pause_io(void)
{
    return lock_context();
}

void jetkvm_virtual_media_resume_io(void)
{
    unlock_context();
}

/* On the Wi-Fi Mini, an HTTPS image read at full speed (about 500 KiB/s)
 * takes enough CPU and Wi-Fi time that video sent beside it drops below
 * 27 fps and freezes. While video frames are being sent, HTTP image reads
 * start at most HTTP_READ_PACE_BYTES_PER_S apart; without video they run at
 * full speed. */
#define HTTP_READ_PACE_BYTES_PER_S (256 * 1024)
#define VIDEO_FRAME_RECENT_MS 1000U

static _Atomic uint32_t s_video_frame_ms;
static int64_t s_http_read_next_us;

void jetkvm_virtual_media_note_video_frame(void)
{
    atomic_store(&s_video_frame_ms, (uint32_t)(esp_timer_get_time() / 1000));
}

static void pace_http_read_locked(size_t size)
{
    const uint32_t video_frame_ms = atomic_load(&s_video_frame_ms);
    int64_t now_us = esp_timer_get_time();
    if (!JETKVM_BOARD_WIFI ||
        (uint32_t)(now_us / 1000) - video_frame_ms > VIDEO_FRAME_RECENT_MS) {
        s_http_read_next_us = 0;
        return;
    }
    if (now_us < s_http_read_next_us) {
        /* vTaskDelay(n) can return up to one tick early. */
        vTaskDelay(pdMS_TO_TICKS((s_http_read_next_us - now_us) / 1000) + 1U);
        now_us = esp_timer_get_time();
    }
    s_http_read_next_us =
        now_us + (int64_t)size * 1000000 / HTTP_READ_PACE_BYTES_PER_S;
}

esp_err_t jetkvm_virtual_media_read(uint32_t medium_generation,
                                   uint64_t offset, void *buffer, size_t size)
{
    ESP_RETURN_ON_FALSE(buffer != NULL || size == 0U, ESP_ERR_INVALID_ARG,
                        TAG, "buffer null");
    int64_t call_started_us = esp_timer_get_time();
    ESP_RETURN_ON_ERROR(lock_context(), TAG, "failed to lock");
    uint32_t lock_wait_us =
        (uint32_t)(esp_timer_get_time() - call_started_us);
    if (medium_generation != atomic_load(&s_medium_generation)) {
        unlock_context();
        return ESP_ERR_INVALID_STATE;
    }
    if (!s_ctx.image_open || offset > s_ctx.image_size ||
        size > UINT_MAX || size > UINT64_MAX - offset ||
        offset + size > logical_capacity_locked()) {
        unlock_context();
        return ESP_ERR_INVALID_ARG;
    }
    if (s_ctx.source == JETKVM_VIRTUAL_MEDIA_SOURCE_HTTP &&
        s_ctx.upload_owner_token !=
            JETKVM_VIRTUAL_MEDIA_UPLOAD_TOKEN_INVALID) {
        /* A remote Range read can hold the network for seconds. Once an
         * upload writer owns the shared storage path, fail new remote-LUN
         * reads quickly so a stalled upstream cannot starve SD writes. */
        if (size > 0U) {
            memset(buffer, 0, size);
        }
        unlock_context();
        return ESP_ERR_INVALID_STATE;
    }
    uint32_t seek_elapsed_us = 0U;
    uint32_t read_elapsed_us = 0U;
    size_t readable = offset < s_ctx.image_size ?
        (size_t)((s_ctx.image_size - offset) < size ?
                     (s_ctx.image_size - offset) : size) : 0U;
    esp_err_t read_error = ESP_OK;
    UINT read_size = 0U;
#if JETKVM_TF_MSC_PREFETCH
    const size_t logical_block_size =
        s_ctx.mode == JETKVM_VIRTUAL_MEDIA_MODE_CDROM ? 2048U : 512U;
    const bool prefetch_eligible =
        s_ctx.source == JETKVM_VIRTUAL_MEDIA_SOURCE_STORAGE && size > 0U &&
        s_ctx.upload_owner_token ==
            JETKVM_VIRTUAL_MEDIA_UPLOAD_TOKEN_INVALID &&
        size <= sizeof(s_tf_upload_dma_bounce) &&
        offset % logical_block_size == 0U &&
        size % logical_block_size == 0U;
#endif
    if (s_ctx.source == JETKVM_VIRTUAL_MEDIA_SOURCE_HTTP) {
        if (readable > 0U) {
            pace_http_read_locked(readable);
            int64_t read_started_us = esp_timer_get_time();
            read_error = jetkvm_http_range_reader_read(
                s_ctx.http_reader, s_ctx.image_size, offset, buffer, readable);
            read_elapsed_us =
                (uint32_t)(esp_timer_get_time() - read_started_us);
            read_size = read_error == ESP_OK ? (UINT)readable : 0U;
        }
        if (read_error != ESP_OK) {
            memset(buffer, 0, size);
        } else if (readable < size) {
            memset((uint8_t *)buffer + readable, 0, size - readable);
        }
    } else if (s_ctx.source == JETKVM_VIRTUAL_MEDIA_SOURCE_BUILTIN) {
        if (jetkvm_builtin_image_read(&jetkvm_builtin_installer_image, offset,
                                      buffer, readable)) {
            read_size = (UINT)readable;
            if (readable < size) {
                memset((uint8_t *)buffer + readable, 0, size - readable);
            }
        } else {
            read_error = ESP_ERR_INVALID_SIZE;
            memset(buffer, 0, size);
        }
    } else if (s_ctx.source == JETKVM_VIRTUAL_MEDIA_SOURCE_STORAGE) {
#if JETKVM_TF_MSC_PREFETCH
        if (prefetch_eligible && s_ctx.prefetch_ready &&
            s_ctx.prefetch_ready_offset == offset &&
            s_ctx.prefetch_ready_size >= size) {
            int64_t copy_started_us = esp_timer_get_time();
            uint32_t ready_age_us =
                s_ctx.prefetch_ready_us > 0 &&
                        copy_started_us >= s_ctx.prefetch_ready_us ?
                    (uint32_t)(copy_started_us - s_ctx.prefetch_ready_us) : 0U;
            memcpy(buffer, s_tf_upload_dma_bounce, size);
            size_t remaining = s_ctx.prefetch_ready_size - size;
            if (remaining > 0U) {
                memmove(s_tf_upload_dma_bounce,
                        s_tf_upload_dma_bounce + size, remaining);
            }
            s_ctx.io_stats.prefetch_copy_us +=
                (uint32_t)(esp_timer_get_time() - copy_started_us);
            s_ctx.io_stats.prefetch_ready_age_us += ready_age_us;
            if (ready_age_us > s_ctx.io_stats.prefetch_max_ready_age_us) {
                s_ctx.io_stats.prefetch_max_ready_age_us = ready_age_us;
            }
            read_size = (UINT)readable;
            s_ctx.io_stats.prefetch_hits++;
            s_ctx.io_stats.prefetch_hit_bytes += read_size;
            s_ctx.prefetch_ready_offset += size;
            s_ctx.prefetch_ready_size = remaining;
            s_ctx.prefetch_ready = remaining > 0U;
            if (!s_ctx.prefetch_ready) {
                s_ctx.prefetch_ready_us = 0;
            }
        } else {
            s_ctx.io_stats.prefetch_misses++;
            s_ctx.prefetch_job_pending = false;
            s_ctx.prefetch_ready = false;
            s_ctx.prefetch_ready_size = 0U;
            s_ctx.prefetch_ready_us = 0;
            read_error = storage_read_locked(
                offset, buffer, size, &read_size, &seek_elapsed_us,
                &read_elapsed_us);
            s_ctx.io_stats.prefetch_miss_bytes += read_size;
        }
#else
        s_ctx.io_stats.prefetch_misses++;
        read_error = storage_read_locked(
            offset, buffer, size, &read_size, &seek_elapsed_us,
            &read_elapsed_us);
        s_ctx.io_stats.prefetch_miss_bytes += read_size;
#endif
    }

#if JETKVM_TF_MSC_PREFETCH
    if (read_error == ESP_OK &&
        s_ctx.source == JETKVM_VIRTUAL_MEDIA_SOURCE_STORAGE &&
        prefetch_eligible) {
        /* Always stage a full scratch-sized span at the next byte offset. This
         * also pipelines across Linux's 120 KiB READ(10) boundary, whose final
         * TinyUSB fragment is only 8 KiB with a 16 KiB endpoint buffer. */
        prefetch_schedule_locked(offset + size);
    }
#endif
    uint32_t call_elapsed_us =
        (uint32_t)(esp_timer_get_time() - call_started_us);
    s_ctx.io_stats.calls++;
    s_ctx.io_stats.bytes_requested += size;
    s_ctx.io_stats.bytes_read += read_size;
    s_ctx.io_stats.total_us += call_elapsed_us;
    s_ctx.io_stats.lock_wait_us += lock_wait_us;
    s_ctx.io_stats.seek_us += seek_elapsed_us;
    s_ctx.io_stats.read_us += read_elapsed_us;
    if (read_error != ESP_OK || read_size != readable) {
        s_ctx.io_stats.errors++;
    }
    if (call_elapsed_us > s_ctx.io_stats.max_us) {
        s_ctx.io_stats.max_us = call_elapsed_us;
    }
    if (lock_wait_us > s_ctx.io_stats.max_lock_wait_us) {
        s_ctx.io_stats.max_lock_wait_us = lock_wait_us;
    }
    if (seek_elapsed_us > s_ctx.io_stats.max_seek_us) {
        s_ctx.io_stats.max_seek_us = seek_elapsed_us;
    }
    if (read_elapsed_us > s_ctx.io_stats.max_read_us) {
        s_ctx.io_stats.max_read_us = read_elapsed_us;
    }
    if (read_error != ESP_OK || read_size != readable) {
        ESP_LOGW(TAG,
                 "image read failed error=%s offset=%" PRIu64
                 " size=%u readable=%u read=%u image=%" PRIu64,
                 esp_err_to_name(read_error), offset, (unsigned)size,
                 (unsigned)readable,
                 (unsigned)read_size, s_ctx.image_size);
    }
    unlock_context();
    return read_error != ESP_OK ? read_error :
           read_size == readable ? ESP_OK : ESP_FAIL;
}

bool jetkvm_virtual_media_is_writable(void)
{
    if (lock_context() != ESP_OK) {
        return false;
    }
    bool writable = storage_image_writable_locked();
    unlock_context();
    return writable;
}

/* Without the medium lock, which an HTTP read holds for up to its timeout:
 * a stale answer during a mount change sends one read down the other path,
 * and the read itself checks the mount under the lock. */
bool jetkvm_virtual_media_is_remote(void)
{
    return __atomic_load_n(&s_ctx.source, __ATOMIC_RELAXED) ==
           JETKVM_VIRTUAL_MEDIA_SOURCE_HTTP;
}

esp_err_t jetkvm_virtual_media_write(uint32_t medium_generation,
                                    uint64_t offset, const void *buffer,
                                    size_t size)
{
    ESP_RETURN_ON_FALSE(buffer != NULL || size == 0U, ESP_ERR_INVALID_ARG,
                        TAG, "write buffer null");
    int64_t call_started_us = esp_timer_get_time();
    ESP_RETURN_ON_ERROR(lock_context(), TAG, "failed to lock");
    uint32_t lock_wait_us =
        (uint32_t)(esp_timer_get_time() - call_started_us);
    if (medium_generation != atomic_load(&s_medium_generation) ||
        !storage_image_writable_locked()) {
        unlock_context();
        return ESP_ERR_INVALID_STATE;
    }
    if (size > UINT_MAX || offset > s_ctx.image_size ||
        size > UINT64_MAX - offset || offset + size > s_ctx.image_size ||
        offset % JETKVM_VIRTUAL_MEDIA_BLOCK_SIZE != 0U ||
        size % JETKVM_VIRTUAL_MEDIA_BLOCK_SIZE != 0U) {
        unlock_context();
        return ESP_ERR_INVALID_ARG;
    }
    if (size == 0U) {
        unlock_context();
        return ESP_OK;
    }

#if JETKVM_TF_MSC_PREFETCH
    /* The worker holds this same mutex for its complete f_read(). Once here,
     * no stale read can still be in flight, and invalidating the generation
     * prevents a queued/ready span from being returned after the write. */
    prefetch_invalidate_locked();
#endif
    FRESULT result = FR_OK;
    UINT written = 0U;
    uint32_t seek_elapsed_us = 0U;
    uint32_t write_elapsed_us = 0U;
    if (f_tell(&s_ctx.image) != (FSIZE_t)offset) {
        int64_t seek_started_us = esp_timer_get_time();
        result = f_lseek(&s_ctx.image, (FSIZE_t)offset);
        seek_elapsed_us =
            (uint32_t)(esp_timer_get_time() - seek_started_us);
    }
    if (result == FR_OK) {
        int64_t write_started_us = esp_timer_get_time();
        result = f_write(&s_ctx.image, buffer, (UINT)size, &written);
        write_elapsed_us =
            (uint32_t)(esp_timer_get_time() - write_started_us);
    }
    if (written > 0U) {
        s_ctx.image_dirty = true;
        s_ctx.write_io_stats.dirty = true;
    }

    uint32_t call_elapsed_us =
        (uint32_t)(esp_timer_get_time() - call_started_us);
    s_ctx.write_io_stats.calls++;
    s_ctx.write_io_stats.bytes_requested += size;
    s_ctx.write_io_stats.bytes_written += written;
    s_ctx.write_io_stats.total_us += call_elapsed_us;
    s_ctx.write_io_stats.lock_wait_us += lock_wait_us;
    s_ctx.write_io_stats.seek_us += seek_elapsed_us;
    s_ctx.write_io_stats.write_us += write_elapsed_us;
    if (result != FR_OK || written != size) {
        s_ctx.write_io_stats.errors++;
    }
    if (call_elapsed_us > s_ctx.write_io_stats.max_us) {
        s_ctx.write_io_stats.max_us = call_elapsed_us;
    }
    if (lock_wait_us > s_ctx.write_io_stats.max_lock_wait_us) {
        s_ctx.write_io_stats.max_lock_wait_us = lock_wait_us;
    }
    if (seek_elapsed_us > s_ctx.write_io_stats.max_seek_us) {
        s_ctx.write_io_stats.max_seek_us = seek_elapsed_us;
    }
    if (write_elapsed_us > s_ctx.write_io_stats.max_write_us) {
        s_ctx.write_io_stats.max_write_us = write_elapsed_us;
    }
    if (result != FR_OK || written != size) {
        ESP_LOGW(TAG,
                 "image write failed error=%s offset=%" PRIu64
                 " size=%u written=%u image=%" PRIu64,
                 esp_err_to_name(fat_result(result)), offset, (unsigned)size,
                 (unsigned)written, s_ctx.image_size);
    }
    unlock_context();
    esp_err_t error = fat_result(result);
    return error != ESP_OK ? error : written == size ? ESP_OK : ESP_FAIL;
}

esp_err_t jetkvm_virtual_media_sync(void)
{
    ESP_RETURN_ON_ERROR(lock_context(), TAG, "failed to lock");
    if (!s_ctx.image_open) {
        unlock_context();
        return ESP_ERR_INVALID_STATE;
    }
    esp_err_t result = storage_sync_locked();
    unlock_context();
    return result;
}

esp_err_t jetkvm_virtual_media_get_io_stats(
    jetkvm_virtual_media_io_stats_t *stats)
{
    ESP_RETURN_ON_FALSE(stats != NULL, ESP_ERR_INVALID_ARG, TAG,
                        "I/O stats output null");
    ESP_RETURN_ON_ERROR(lock_context(), TAG, "failed to lock");
    *stats = s_ctx.io_stats;
    unlock_context();
    return ESP_OK;
}

esp_err_t jetkvm_virtual_media_get_write_io_stats(
    jetkvm_virtual_media_write_io_stats_t *stats)
{
    ESP_RETURN_ON_FALSE(stats != NULL, ESP_ERR_INVALID_ARG, TAG,
                        "write I/O stats output null");
    ESP_RETURN_ON_ERROR(lock_context(), TAG, "failed to lock");
    *stats = s_ctx.write_io_stats;
    stats->dirty = s_ctx.image_dirty;
    unlock_context();
    return ESP_OK;
}

esp_err_t jetkvm_virtual_media_get_upload_io_stats(
    jetkvm_virtual_media_upload_io_stats_t *stats)
{
    ESP_RETURN_ON_FALSE(stats != NULL, ESP_ERR_INVALID_ARG, TAG,
                        "upload I/O stats output null");
    ESP_RETURN_ON_ERROR(lock_context(), TAG, "failed to lock");
    *stats = s_ctx.upload_io_stats;
    unlock_context();
    return ESP_OK;
}

esp_err_t jetkvm_virtual_media_get_sd_write_hint_stats(
    jetkvm_virtual_media_sd_write_hint_stats_t *stats)
{
    ESP_RETURN_ON_FALSE(stats != NULL, ESP_ERR_INVALID_ARG, TAG,
                        "SD write-hint stats output null");
#if JETKVM_TF_SD_ACMD23
    sdmmc_acmd23_stats_t driver_stats = {0};
    ESP_RETURN_ON_ERROR(sdmmc_acmd23_get_stats(&driver_stats), TAG,
                        "failed to read SD write-hint stats");
    *stats = (jetkvm_virtual_media_sd_write_hint_stats_t) {
        .enabled = driver_stats.enabled,
        .unsupported_latched = driver_stats.unsupported_latched,
        .attempts = driver_stats.attempts,
        .successes = driver_stats.successes,
        .unsupported = driver_stats.unsupported,
        .failures = driver_stats.failures,
        .last_block_count = driver_stats.last_block_count,
        .last_error = driver_stats.last_error,
    };
#else
    *stats = (jetkvm_virtual_media_sd_write_hint_stats_t) {
        .enabled = false,
        .last_error = ESP_OK,
    };
#endif
    return ESP_OK;
}

esp_err_t jetkvm_virtual_media_capacity(uint32_t *block_count,
                                       uint16_t *block_size)
{
    ESP_RETURN_ON_FALSE(block_count != NULL && block_size != NULL,
                        ESP_ERR_INVALID_ARG, TAG, "capacity output null");
    ESP_RETURN_ON_ERROR(lock_context(), TAG, "failed to lock");
    const uint16_t logical_block_size =
        s_ctx.mode == JETKVM_VIRTUAL_MEDIA_MODE_CDROM ? 2048U :
                                                       JETKVM_VIRTUAL_MEDIA_BLOCK_SIZE;
    uint64_t count = (s_ctx.image_size + logical_block_size - 1U) /
                     logical_block_size;
    if (!s_ctx.image_open || count == 0U || count > UINT32_MAX) {
        unlock_context();
        return ESP_ERR_INVALID_STATE;
    }
    *block_count = (uint32_t)count;
    *block_size = logical_block_size;
    unlock_context();
    return ESP_OK;
}

static bool upload_matches(const char *upload_id)
{
    return upload_id != NULL && s_ctx.upload_open &&
           strcmp(upload_id, s_ctx.upload_id) == 0;
}

static bool upload_token_matches(
    const char *upload_id, jetkvm_virtual_media_upload_token_t token)
{
    return upload_matches(upload_id) &&
           token != JETKVM_VIRTUAL_MEDIA_UPLOAD_TOKEN_INVALID &&
           token == s_ctx.upload_owner_token;
}

static bool upload_owner_requires_staging(
    jetkvm_virtual_media_upload_owner_t owner)
{
    if (owner == JETKVM_VIRTUAL_MEDIA_UPLOAD_OWNER_HTTP) {
        return JETKVM_TF_HTTP_PSRAM_DIRECT == 0;
    }
    return owner == JETKVM_VIRTUAL_MEDIA_UPLOAD_OWNER_WEBRTC;
}

static jetkvm_virtual_media_upload_writer_fn s_upload_writer_hook;
static void *s_upload_writer_hook_ctx;

void jetkvm_virtual_media_set_upload_writer_hook(
    jetkvm_virtual_media_upload_writer_fn hook, void *ctx)
{
    s_upload_writer_hook = hook;
    s_upload_writer_hook_ctx = ctx;
}

/* The device's own download is not a browser upload; it has its own hooks. */
static void notify_upload_writer_locked(
    jetkvm_virtual_media_upload_owner_t owner, bool active)
{
    if (s_upload_writer_hook != NULL &&
        (owner == JETKVM_VIRTUAL_MEDIA_UPLOAD_OWNER_HTTP ||
         owner == JETKVM_VIRTUAL_MEDIA_UPLOAD_OWNER_WEBRTC)) {
        s_upload_writer_hook(active, s_upload_writer_hook_ctx);
    }
}

/* The one place where a writer lease ends, whatever ended it. */
static uint8_t *clear_upload_owner_locked(void)
{
    notify_upload_writer_locked(s_ctx.upload_owner, false);
    uint8_t *staging = s_ctx.upload_staging;
    if (staging != NULL) {
        s_ctx.upload_io_stats.staging_releases++;
    }
    s_ctx.upload_staging = NULL;
    s_ctx.upload_staging_size = 0U;
    s_ctx.upload_io_stats.staging_active = false;
    s_ctx.upload_io_stats.staging_bytes = 0U;
    s_ctx.upload_owner = 0;
    s_ctx.upload_owner_token = JETKVM_VIRTUAL_MEDIA_UPLOAD_TOKEN_INVALID;
    return staging;
}

/* Call only after the tracked FIL has closed (or was never opened). Paths and
 * digest remain intact while a close is retryable; clearing them sooner could
 * let a different upload overwrite a still-live FatFs object. */
static void clear_upload_session_locked(void)
{
    s_ctx.upload_open = false;
    s_ctx.upload_close_pending = false;
    s_ctx.upload_claim_pending = false;
    s_ctx.upload_id[0] = '\0';
    s_ctx.upload_final_path[0] = '\0';
    s_ctx.upload_incomplete_path[0] = '\0';
    s_ctx.upload_identity_path[0] = '\0';
    s_ctx.upload_sha256_hex[0] = '\0';
    s_ctx.upload_size = 0U;
    s_ctx.upload_written = 0U;
}

/* A session without a writer holds only its open FIL: its POST never came, or
 * the request ended before it claimed the session. Close it, keeping the
 * .incomplete data and identity for a later resume, so it cannot block other
 * operations until a reboot. A session with a writer stays open. */
static esp_err_t close_idle_upload_locked(void)
{
    if (!s_ctx.upload_open) {
        return ESP_OK;
    }
    if (s_ctx.upload_owner_token != JETKVM_VIRTUAL_MEDIA_UPLOAD_TOKEN_INVALID ||
        s_ctx.upload_staging != NULL || s_ctx.upload_claim_pending) {
        return ESP_ERR_INVALID_STATE;
    }
    FRESULT result = f_close(&s_ctx.upload);
    if (result != FR_OK) {
        s_ctx.upload_close_pending = true;
        return fat_result(result);
    }
    clear_upload_session_locked();
    return ESP_OK;
}

static void finish_upload_claim_attempt(const char *upload_id,
                                        bool allocation_failed)
{
    if (lock_context() != ESP_OK) {
        return;
    }
    if (upload_matches(upload_id) && s_ctx.upload_claim_pending) {
        s_ctx.upload_claim_pending = false;
        if (allocation_failed) {
            s_ctx.upload_io_stats.staging_allocation_failures++;
        }
    }
    unlock_context();
}

/* Every start hands out a fresh ID, as upstream does: a start supersedes
 * the previous one, so a stale browser transfer can no longer append next
 * to the new writer even when it resumes the same partial file. */
static void assign_upload_id_locked(void)
{
    uint32_t random[4] = {
        esp_random(), esp_random(), esp_random(), esp_random()
    };
    snprintf(s_ctx.upload_id, sizeof(s_ctx.upload_id),
             "upload_%08lx-%08lx-%08lx-%08lx",
             (unsigned long)random[0], (unsigned long)random[1],
             (unsigned long)random[2], (unsigned long)random[3]);
}

static void initialize_upload_session_locked(
    const char *final_path, const char *incomplete_path,
    const char *identity_path, uint64_t size, const char *sha256_hex,
    uint64_t existing, bool close_pending)
{
    assign_upload_id_locked();
    snprintf(s_ctx.upload_final_path, sizeof(s_ctx.upload_final_path), "%s",
             final_path);
    snprintf(s_ctx.upload_incomplete_path,
             sizeof(s_ctx.upload_incomplete_path), "%s", incomplete_path);
    snprintf(s_ctx.upload_identity_path, sizeof(s_ctx.upload_identity_path),
             "%s", identity_path);
    jetkvm_upload_sha256_hex_normalize(sha256_hex,
                                       s_ctx.upload_sha256_hex);
    s_ctx.upload_open = true;
    s_ctx.upload_close_pending = close_pending;
    s_ctx.upload_claim_pending = false;
    s_ctx.upload_owner = 0;
    s_ctx.upload_owner_token = JETKVM_VIRTUAL_MEDIA_UPLOAD_TOKEN_INVALID;
    s_ctx.upload_size = size;
    s_ctx.upload_written = existing;
    memset(&s_ctx.upload_io_stats, 0, sizeof(s_ctx.upload_io_stats));
}

esp_err_t jetkvm_virtual_media_start_upload(
    const char *filename, uint64_t size, const char *sha256_hex,
    jetkvm_virtual_media_upload_t *upload)
{
    ESP_RETURN_ON_FALSE(
        upload != NULL && size > 0U &&
            jetkvm_upload_sha256_hex_valid(sha256_hex),
        ESP_ERR_INVALID_ARG, TAG, "invalid upload identity");
    ESP_RETURN_ON_FALSE(
        valid_filename(filename) &&
            !filename_has_suffix_ascii_case_insensitive(
                filename, TF_UPLOAD_INCOMPLETE_SUFFIX) &&
            !filename_is_upload_identity_metadata(filename),
        ESP_ERR_INVALID_ARG, TAG, "reserved upload filename");
    char final_path[sizeof(s_ctx.upload_final_path)];
    ESP_RETURN_ON_ERROR(make_image_path(filename, final_path,
                                        sizeof(final_path)), TAG,
                        "invalid filename");
    char incomplete_path[sizeof(s_ctx.upload_incomplete_path)];
    char identity_path[sizeof(s_ctx.upload_identity_path)];
    char temporary_path[sizeof(s_ctx.upload_identity_path) +
                        sizeof(TF_UPLOAD_IDENTITY_TEMP_SUFFIX)];
    ESP_RETURN_ON_ERROR(
        append_path_suffix(final_path, TF_UPLOAD_INCOMPLETE_SUFFIX,
                           incomplete_path, sizeof(incomplete_path)), TAG,
        "upload incomplete path too long");
    ESP_RETURN_ON_ERROR(
        append_path_suffix(incomplete_path, TF_UPLOAD_IDENTITY_SUFFIX,
                           identity_path, sizeof(identity_path)), TAG,
        "upload identity path too long");
    ESP_RETURN_ON_ERROR(
        append_path_suffix(identity_path, TF_UPLOAD_IDENTITY_TEMP_SUFFIX,
                           temporary_path, sizeof(temporary_path)), TAG,
        "upload identity temporary path too long");

    ESP_RETURN_ON_ERROR(lock_context(), TAG, "failed to lock");
    if (!s_ctx.filesystem_mounted) {
        unlock_context();
        return ESP_ERR_INVALID_STATE;
    }
    esp_err_t metadata_error = service_upload_metadata_locked();
    if (metadata_error != ESP_OK) {
        unlock_context();
        return metadata_error;
    }
    if (s_ctx.upload_open) {
        /* A released writer lease, including an abort whose FatFs close could
         * not yet be synchronized, remains resumable without reopening or
         * discarding the live FIL. Real digests remain strict; the reserved
         * no-digest sentinel deliberately falls back to filename and size. */
        bool resumable_open =
            s_ctx.upload_owner_token ==
                JETKVM_VIRTUAL_MEDIA_UPLOAD_TOKEN_INVALID &&
            s_ctx.upload_staging == NULL && !s_ctx.upload_claim_pending &&
            jetkvm_upload_resume_identity_matches(
                size, sha256_hex, s_ctx.upload_size,
                s_ctx.upload_sha256_hex) &&
            strcmp(s_ctx.upload_final_path, final_path) == 0;
        if (resumable_open) {
            if (s_ctx.upload_close_pending) {
                /* Never hand a FIL that has already reported an I/O/setup
                 * error back to a writer: FatFs may latch fp->err. Retry the
                 * pending close first, then reopen the .incomplete path below
                 * so a recovered card starts with a clean FIL object. */
                FRESULT close_result = f_close(&s_ctx.upload);
                if (close_result != FR_OK) {
                    unlock_context();
                    return fat_result(close_result);
                }
                clear_upload_session_locked();
            } else {
                FRESULT seek_result =
                    f_lseek(&s_ctx.upload, (FSIZE_t)s_ctx.upload_written);
                if (seek_result != FR_OK) {
                    /* FatFs can latch a disk error in FIL::err. Do not offer
                     * that poisoned object again on every exact resume. Close
                     * it now when possible, otherwise force the next start
                     * through the close-pending recovery path. */
                    FRESULT close_result = f_close(&s_ctx.upload);
                    if (close_result == FR_OK) {
                        clear_upload_session_locked();
                    } else {
                        s_ctx.upload_close_pending = true;
                    }
                    unlock_context();
                    return fat_result(seek_result);
                }
                memset(upload, 0, sizeof(*upload));
                assign_upload_id_locked();
                snprintf(upload->upload_id, sizeof(upload->upload_id), "%s",
                         s_ctx.upload_id);
                upload->already_uploaded_bytes = s_ctx.upload_written;
                unlock_context();
                return ESP_OK;
            }
        } else {
            esp_err_t close_error = close_idle_upload_locked();
            if (close_error != ESP_OK) {
                unlock_context();
                return close_error;
            }
        }
    }
    if (s_ctx.upload_owner_token !=
            JETKVM_VIRTUAL_MEDIA_UPLOAD_TOKEN_INVALID ||
        s_ctx.upload_staging != NULL) {
        unlock_context();
        return ESP_ERR_INVALID_STATE;
    }
#if JETKVM_TF_MSC_PREFETCH
    /* Upload and read-ahead intentionally share one internal DMA scratch.
     * Pause speculative reads for the upload lifetime; foreground MSC reads
     * remain correct through their caller-provided TinyUSB buffer. */
    prefetch_invalidate_locked();
#endif
    FILINFO info;
    FRESULT final_result = f_stat(final_path, &info);
    if (final_result == FR_OK) {
        unlock_context();
        return ESP_ERR_INVALID_STATE;
    }
    if (final_result != FR_NO_FILE && final_result != FR_NO_PATH) {
        unlock_context();
        return fat_result(final_result);
    }
    FRESULT result = f_stat(incomplete_path, &info);
    uint64_t existing = result == FR_OK ? (uint64_t)info.fsize : 0U;
    if (result != FR_OK && result != FR_NO_FILE && result != FR_NO_PATH) {
        unlock_context();
        return fat_result(result);
    }
    if (result == FR_OK && (info.fattrib & AM_DIR) != 0U) {
        unlock_context();
        return ESP_ERR_INVALID_STATE;
    }
    if (existing > size) {
        unlock_context();
        return ESP_ERR_INVALID_SIZE;
    }
    jetkvm_virtual_media_space_t space = {0};
    esp_err_t space_error = get_space_locked(&space);
    if (space_error != ESP_OK || size - existing > space.bytes_free) {
        unlock_context();
        return space_error != ESP_OK ? space_error : ESP_ERR_NO_MEM;
    }
    char effective_sha256_hex[JETKVM_UPLOAD_SHA256_HEX_LENGTH + 1U];
    esp_err_t identity_error = ensure_upload_identity_locked(
        identity_path, temporary_path, existing, size, sha256_hex,
        effective_sha256_hex);
    if (identity_error != ESP_OK) {
        unlock_context();
        return identity_error;
    }

    result = f_open(&s_ctx.upload, incomplete_path,
                    FA_WRITE | FA_OPEN_ALWAYS);
    bool opened = result == FR_OK;
    if (opened && existing == 0U) {
        /* Locate a contiguous extent without allocating it or changing the
         * visible .incomplete size. A fragmented card may have no extent this
         * large; normal incremental allocation is the safe fallback. */
        FRESULT expand_result = f_expand(&s_ctx.upload, (FSIZE_t)size, 0);
        if (expand_result != FR_OK && expand_result != FR_DENIED) {
            result = expand_result;
        }
    }
    if (result == FR_OK) {
        result = f_lseek(&s_ctx.upload, (FSIZE_t)existing);
    }
    if (result != FR_OK) {
        if (opened) {
            FRESULT close_result = f_close(&s_ctx.upload);
            if (close_result != FR_OK) {
                /* With the retry-safe FatFs patch, a failed close retains a
                 * live FIL. Track it as an ownerless session instead of
                 * allowing the next start to overwrite the object. The exact
                 * filename/size/digest can retry the terminal sync later. */
                initialize_upload_session_locked(
                    final_path, incomplete_path, identity_path,
                    size, effective_sha256_hex, existing, true);
                ESP_LOGE(TAG,
                         "upload setup failed and close remains retryable: "
                         "setup=%u close=%u",
                         (unsigned)result, (unsigned)close_result);
            }
        }
        unlock_context();
        return fat_result(result);
    }
    initialize_upload_session_locked(
        final_path, incomplete_path, identity_path,
        size, effective_sha256_hex, existing, false);
    memset(upload, 0, sizeof(*upload));
    snprintf(upload->upload_id, sizeof(upload->upload_id), "%s",
             s_ctx.upload_id);
    upload->already_uploaded_bytes = existing;
    unlock_context();
    return ESP_OK;
}

esp_err_t jetkvm_virtual_media_claim_upload(
    const char *upload_id, jetkvm_virtual_media_upload_owner_t owner,
    jetkvm_virtual_media_upload_token_t *token,
    uint64_t *uploaded_bytes, uint64_t *expected_bytes)
{
    ESP_RETURN_ON_FALSE(
        token != NULL &&
            (owner == JETKVM_VIRTUAL_MEDIA_UPLOAD_OWNER_HTTP ||
             owner == JETKVM_VIRTUAL_MEDIA_UPLOAD_OWNER_WEBRTC ||
             owner == JETKVM_VIRTUAL_MEDIA_UPLOAD_OWNER_DOWNLOAD),
        ESP_ERR_INVALID_ARG, TAG, "invalid upload owner claim");
    *token = JETKVM_VIRTUAL_MEDIA_UPLOAD_TOKEN_INVALID;
    ESP_RETURN_ON_ERROR(lock_context(), TAG, "failed to lock");
    if (!upload_matches(upload_id)) {
        unlock_context();
        return ESP_ERR_NOT_FOUND;
    }
    if (s_ctx.upload_owner_token !=
            JETKVM_VIRTUAL_MEDIA_UPLOAD_TOKEN_INVALID ||
        s_ctx.upload_staging != NULL || s_ctx.upload_close_pending ||
        s_ctx.upload_claim_pending) {
        unlock_context();
        return ESP_ERR_INVALID_STATE;
    }
    /* Serialize the out-of-lock staging phase. HTTP can use its aligned
     * ingress buffers directly on ESP32-P4; WebRTC retains the conservative
     * internal-DMA staging path. Without this gate, racing claims could both
     * pass the ownership preflight before either publishes its token. */
    s_ctx.upload_claim_pending = true;
    unlock_context();

    const bool staging_required = upload_owner_requires_staging(owner);
    uint8_t *staging = NULL;
    if (staging_required) {
        const jetkvm_memory_policy_request_t staging_request = {
            .internal_bytes = TF_UPLOAD_OWNER_STAGING_BYTES,
            .internal_largest_block = TF_UPLOAD_OWNER_STAGING_BYTES,
            .dma_bytes = TF_UPLOAD_OWNER_STAGING_BYTES,
            .dma_largest_block = TF_UPLOAD_OWNER_STAGING_BYTES,
        };
        jetkvm_memory_policy_reservation_t staging_reservation = {0};
        if (!jetkvm_memory_policy_reserve(
                "tf-upload-staging", &staging_request,
                &staging_reservation)) {
            finish_upload_claim_attempt(upload_id, true);
            return ESP_ERR_NO_MEM;
        }
        staging = heap_caps_aligned_alloc(
            TF_UPLOAD_DMA_ALIGNMENT, TF_UPLOAD_OWNER_STAGING_BYTES,
            MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA | MALLOC_CAP_8BIT);
        jetkvm_memory_policy_release(&staging_reservation);
        if (staging == NULL) {
            finish_upload_claim_attempt(upload_id, true);
            return ESP_ERR_NO_MEM;
        }
    }

    esp_err_t lock_error = lock_context();
    if (lock_error != ESP_OK) {
        heap_caps_free(staging);
        return lock_error;
    }
    if (!upload_matches(upload_id)) {
        unlock_context();
        heap_caps_free(staging);
        return ESP_ERR_NOT_FOUND;
    }
    bool claim_current = s_ctx.upload_claim_pending;
    s_ctx.upload_claim_pending = false;
    if (s_ctx.upload_owner_token !=
            JETKVM_VIRTUAL_MEDIA_UPLOAD_TOKEN_INVALID ||
        s_ctx.upload_staging != NULL || s_ctx.upload_close_pending ||
        !claim_current) {
        unlock_context();
        heap_caps_free(staging);
        return ESP_ERR_INVALID_STATE;
    }
#if JETKVM_TF_MSC_PREFETCH
    /* The owner token, not the resumable upload session, is the dynamic
     * staging lease. A failed HTTP/RTC admission releases both while keeping
     * the .incomplete file resumable, so prefetch can resume immediately. */
    prefetch_invalidate_locked();
#endif
    jetkvm_virtual_media_upload_token_t claimed =
        ((uint64_t)esp_random() << 32U) | esp_random();
    if (claimed == JETKVM_VIRTUAL_MEDIA_UPLOAD_TOKEN_INVALID) {
        /* A capability must never alias the public invalid sentinel. */
        claimed = UINT64_C(1);
    }
    s_ctx.upload_staging = staging;
    s_ctx.upload_staging_size =
        staging != NULL ? TF_UPLOAD_OWNER_STAGING_BYTES : 0U;
    if (staging != NULL) {
        s_ctx.upload_io_stats.staging_allocations++;
        s_ctx.upload_io_stats.staging_active = true;
        s_ctx.upload_io_stats.staging_bytes = TF_UPLOAD_OWNER_STAGING_BYTES;
    }
    s_ctx.upload_owner = owner;
    s_ctx.upload_owner_token = claimed;
    notify_upload_writer_locked(owner, true);
    *token = claimed;
    if (uploaded_bytes != NULL) {
        *uploaded_bytes = s_ctx.upload_written;
    }
    if (expected_bytes != NULL) {
        *expected_bytes = s_ctx.upload_size;
    }
    unlock_context();
    return ESP_OK;
}

esp_err_t jetkvm_virtual_media_release_upload(
    const char *upload_id, jetkvm_virtual_media_upload_token_t token)
{
    ESP_RETURN_ON_FALSE(
        token != JETKVM_VIRTUAL_MEDIA_UPLOAD_TOKEN_INVALID,
        ESP_ERR_INVALID_ARG, TAG, "invalid upload owner token");
    ESP_RETURN_ON_ERROR(lock_context(), TAG, "failed to lock");
    if (!upload_matches(upload_id)) {
        unlock_context();
        return ESP_ERR_NOT_FOUND;
    }
    if (!upload_token_matches(upload_id, token)) {
        unlock_context();
        return ESP_ERR_INVALID_STATE;
    }
    uint8_t *staging = clear_upload_owner_locked();
    unlock_context();
    heap_caps_free(staging);
    return ESP_OK;
}

#if JETKVM_TF_UPLOAD_COPY_PIPELINE && JETKVM_TF_MSC_PREFETCH
/* Caller owns s_ctx.mutex. The first half is filled synchronously; thereafter
 * the prefetch worker fills the inactive half while this task blocks in the
 * current f_write()/SDMMC transaction. Exactly one 32 KiB allocation is split
 * into two 16 KiB windows, so the dynamic Internal/DMA staging budget does not
 * change. */
static FRESULT write_upload_staged_pipeline_locked(
    const uint8_t *source, size_t size, size_t sector_size,
    size_t half_capacity, UINT *total_written)
{
    uint8_t *halves[2] = {
        s_ctx.upload_staging,
        s_ctx.upload_staging + half_capacity,
    };
    size_t remaining = size;
    unsigned current_half = 0U;
    size_t current_size = jetkvm_upload_bounce_slice_size(
        remaining, half_capacity, s_ctx.upload_written, sector_size);
    if (current_size == 0U) {
        return FR_INVALID_PARAMETER;
    }

    int64_t copy_started_us = esp_timer_get_time();
    memcpy(halves[current_half], source, current_size);
    s_ctx.upload_io_stats.bounce_copy_us +=
        (uint64_t)(esp_timer_get_time() - copy_started_us);

    FRESULT result = FR_OK;
    while (remaining > 0U) {
        const unsigned next_half = current_half ^ 1U;
        const size_t remaining_after_current = remaining - current_size;
        size_t next_size = 0U;
        bool next_copy_scheduled = false;
        if (remaining_after_current > 0U) {
            next_size = jetkvm_upload_bounce_slice_size(
                remaining_after_current, half_capacity,
                s_ctx.upload_written + current_size, sector_size);
            if (next_size == 0U) {
                return FR_INVALID_PARAMETER;
            }
            next_copy_scheduled = upload_copy_schedule_locked(
                halves[next_half], source + current_size, next_size);
        }

        UINT slice_written = 0U;
        int64_t write_started_us = esp_timer_get_time();
        result = f_write(&s_ctx.upload, halves[current_half],
                         (UINT)current_size, &slice_written);
        uint64_t write_elapsed_us =
            (uint64_t)(esp_timer_get_time() - write_started_us);
        s_ctx.upload_io_stats.bounce_calls++;
        s_ctx.upload_io_stats.bounce_bytes += slice_written;
        s_ctx.upload_io_stats.bounce_write_us += write_elapsed_us;
        *total_written += slice_written;
        s_ctx.upload_written += slice_written;

        /* Join before examining the write result. Even an unused speculative
         * next half may still reference the transport's source buffer. */
        if (next_copy_scheduled) {
            s_ctx.upload_io_stats.bounce_copy_us +=
                upload_copy_wait_locked();
        }
        if (result != FR_OK || slice_written != current_size) {
            break;
        }

        source += slice_written;
        remaining -= slice_written;
        if (remaining == 0U) {
            break;
        }
        if (!next_copy_scheduled) {
            copy_started_us = esp_timer_get_time();
            memcpy(halves[next_half], source, next_size);
            s_ctx.upload_io_stats.bounce_copy_us +=
                (uint64_t)(esp_timer_get_time() - copy_started_us);
        }
        current_half = next_half;
        current_size = next_size;
    }
    return result;
}
#endif

static esp_err_t write_upload_with_token(
    const char *upload_id, jetkvm_virtual_media_upload_token_t token,
    const void *data, size_t size,
    uint64_t *uploaded_bytes, uint64_t *expected_bytes)
{
    ESP_RETURN_ON_FALSE(data != NULL || size == 0U, ESP_ERR_INVALID_ARG,
                        TAG, "data null");
    ESP_RETURN_ON_ERROR(lock_context(), TAG, "failed to lock");
    bool exists = upload_matches(upload_id);
    if (!upload_token_matches(upload_id, token) || s_ctx.upload_close_pending) {
        esp_err_t error = exists ? ESP_ERR_INVALID_STATE : ESP_ERR_NOT_FOUND;
        unlock_context();
        return error;
    }
    const bool owner_staging =
        upload_owner_requires_staging(s_ctx.upload_owner);
    if (owner_staging &&
        (s_ctx.upload_staging == NULL ||
         s_ctx.upload_staging_size != TF_UPLOAD_OWNER_STAGING_BYTES)) {
        unlock_context();
        return ESP_ERR_INVALID_STATE;
    }
    if (!owner_staging && s_ctx.upload_staging != NULL) {
        unlock_context();
        return ESP_ERR_INVALID_STATE;
    }
    if (size > s_ctx.upload_size - s_ctx.upload_written) {
        unlock_context();
        return ESP_ERR_INVALID_SIZE;
    }
    if (size > UINT_MAX) {
        unlock_context();
        return ESP_ERR_INVALID_SIZE;
    }
    if (size == 0U) {
        if (uploaded_bytes != NULL) {
            *uploaded_bytes = s_ctx.upload_written;
        }
        if (expected_bytes != NULL) {
            *expected_bytes = s_ctx.upload_size;
        }
        unlock_context();
        return ESP_OK;
    }
    if ((uint64_t)f_tell(&s_ctx.upload) != s_ctx.upload_written) {
        unlock_context();
        return ESP_ERR_INVALID_STATE;
    }
    const uint8_t *source = data;
    size_t remaining = size;
    UINT total_written = 0U;
    FRESULT result = FR_OK;
    size_t sector_size = s_ctx.card != NULL &&
                                 s_ctx.card->csd.sector_size > 0 ?
                             (size_t)s_ctx.card->csd.sector_size : 0U;
    int64_t call_started_us = esp_timer_get_time();
#if JETKVM_TF_MSC_PREFETCH
    /* The storage mutex serializes USB reads and upload writes. Invalidate a
     * ready read-ahead job once before reusing the shared DMA scratch. */
    prefetch_invalidate_locked();
#endif
    bool pipeline_handled = false;
#if JETKVM_TF_UPLOAD_COPY_PIPELINE && JETKVM_TF_MSC_PREFETCH
    const bool source_span_valid =
        size - 1U <= UINTPTR_MAX - (uintptr_t)source;
    size_t pipeline_half_capacity = 0U;
    if (owner_staging && source_span_valid &&
        jetkvm_upload_owner_staging_required(true, source, size) &&
        s_ctx.prefetch_task != NULL && s_ctx.upload_copy_done != NULL &&
        atomic_load_explicit(&s_ctx.upload_copy_state,
                             memory_order_acquire) == TF_UPLOAD_COPY_IDLE) {
        pipeline_half_capacity = jetkvm_upload_pipeline_half_capacity(
            s_ctx.upload_staging_size, sector_size,
            TF_UPLOAD_DMA_ALIGNMENT);
    }
    if (pipeline_half_capacity > 0U) {
        result = write_upload_staged_pipeline_locked(
            source, size, sector_size, pipeline_half_capacity,
            &total_written);
        pipeline_handled = true;
    }
#endif
    while (!pipeline_handled && remaining > 0U) {
        size_t direct_size = sector_size > 0U ?
                                 remaining - remaining % sector_size : 0U;
        bool staging_required = jetkvm_upload_owner_staging_required(
            owner_staging, source,
            direct_size > 0U ? direct_size : remaining);
        bool host_aligned =
            !staging_required && direct_size > 0U &&
            s_ctx.card->host.check_buffer_alignment != NULL &&
            s_ctx.card->host.check_buffer_alignment(
                s_ctx.card->host.slot, source, direct_size);
        bool direct = !staging_required &&
            jetkvm_upload_direct_write_eligible(
                source, direct_size, s_ctx.upload_written, sector_size,
                host_aligned);
        uint8_t *bounce = owner_staging ? s_ctx.upload_staging :
                                          s_tf_upload_dma_bounce;
        size_t bounce_capacity = owner_staging ? s_ctx.upload_staging_size :
                                                sizeof(s_tf_upload_dma_bounce);
        size_t slice_size = direct ? direct_size :
            jetkvm_upload_bounce_slice_size(
                remaining, bounce_capacity, s_ctx.upload_written,
                sector_size);
        const void *write_data = source;
        if (slice_size == 0U) {
            result = FR_INVALID_PARAMETER;
            break;
        }
        if (!direct) {
            int64_t copy_started_us = esp_timer_get_time();
            memcpy(bounce, source, slice_size);
            s_ctx.upload_io_stats.bounce_copy_us +=
                (uint64_t)(esp_timer_get_time() - copy_started_us);
            write_data = bounce;
        }
        if (slice_size > UINT_MAX) {
            result = FR_INVALID_PARAMETER;
            break;
        }
        UINT slice_written = 0U;
        int64_t write_started_us = esp_timer_get_time();
        result = f_write(&s_ctx.upload, write_data, (UINT)slice_size,
                         &slice_written);
        uint64_t write_elapsed_us =
            (uint64_t)(esp_timer_get_time() - write_started_us);
        if (direct) {
            s_ctx.upload_io_stats.direct_calls++;
            s_ctx.upload_io_stats.direct_bytes += slice_written;
            s_ctx.upload_io_stats.direct_us += write_elapsed_us;
        } else {
            s_ctx.upload_io_stats.bounce_calls++;
            s_ctx.upload_io_stats.bounce_bytes += slice_written;
            s_ctx.upload_io_stats.bounce_write_us += write_elapsed_us;
        }
        total_written += slice_written;
        s_ctx.upload_written += slice_written;
        if (result != FR_OK || slice_written != slice_size) {
            break;
        }
        source += slice_written;
        remaining -= slice_written;
    }
    uint64_t call_elapsed_us =
        (uint64_t)(esp_timer_get_time() - call_started_us);
    s_ctx.upload_io_stats.calls++;
    s_ctx.upload_io_stats.bytes_requested += size;
    s_ctx.upload_io_stats.bytes_written += total_written;
    s_ctx.upload_io_stats.total_us += call_elapsed_us;
    if (result != FR_OK || total_written != size) {
        s_ctx.upload_io_stats.errors++;
    }
    if (uploaded_bytes != NULL) {
        *uploaded_bytes = s_ctx.upload_written;
    }
    if (expected_bytes != NULL) {
        *expected_bytes = s_ctx.upload_size;
    }
    unlock_context();
    return result != FR_OK ? fat_result(result) :
           total_written == size ? ESP_OK : ESP_FAIL;
}

esp_err_t jetkvm_virtual_media_write_upload_owned(
    const char *upload_id, jetkvm_virtual_media_upload_token_t token,
    const void *data, size_t size,
    uint64_t *uploaded_bytes, uint64_t *expected_bytes)
{
    ESP_RETURN_ON_FALSE(
        token != JETKVM_VIRTUAL_MEDIA_UPLOAD_TOKEN_INVALID,
        ESP_ERR_INVALID_ARG, TAG, "invalid upload owner token");
    return write_upload_with_token(upload_id, token, data, size,
                                   uploaded_bytes, expected_bytes);
}

static esp_err_t finish_upload_with_token(
    const char *upload_id, jetkvm_virtual_media_upload_token_t token)
{
    ESP_RETURN_ON_ERROR(lock_context(), TAG, "failed to lock");
    bool exists = upload_matches(upload_id);
    if (!upload_token_matches(upload_id, token)) {
        esp_err_t error = exists ? ESP_ERR_INVALID_STATE : ESP_ERR_NOT_FOUND;
        unlock_context();
        return error;
    }
    if (s_ctx.upload_written != s_ctx.upload_size) {
        unlock_context();
        return ESP_ERR_INVALID_SIZE;
    }
    /* f_close() calls f_sync() internally. Calling both back-to-back repeats
     * the same metadata flush without adding a durability boundary. */
    FRESULT result = f_close(&s_ctx.upload);
    if (result != FR_OK) {
        s_ctx.upload_close_pending = true;
        unlock_context();
        return fat_result(result);
    }
    s_ctx.upload_open = false;
    s_ctx.upload_close_pending = false;
    s_ctx.upload_claim_pending = false;
    uint8_t *staging = clear_upload_owner_locked();
    result = f_rename(s_ctx.upload_incomplete_path, s_ctx.upload_final_path);
    esp_err_t finish_error = fat_result(result);
    if (result == FR_OK) {
        /* The payload is published first. Only then remove the identity that
         * authorized its resumable prefix. A successful rename cannot be
         * rolled back on faulty media, so a sidecar cleanup error stays in
         * the persistent retry state but must not turn a completed upload
         * into a false failure. */
        esp_err_t cleanup_error = queue_upload_metadata_cleanup_locked(
            s_ctx.upload_identity_path);
        if (cleanup_error != ESP_OK) {
            ESP_LOGE(TAG,
                     "completed upload identity cleanup queued for retry: %s",
                     esp_err_to_name(cleanup_error));
        }
    }
    /* On rename failure the incomplete payload and canonical sidecar remain
     * paired on disk; the exact size/digest can reopen them on the next start.
     * The FIL is already closed, so no in-memory capability may stay live. */
    clear_upload_session_locked();
    unlock_context();
    heap_caps_free(staging);
    return finish_error;
}

esp_err_t jetkvm_virtual_media_finish_upload_owned(
    const char *upload_id, jetkvm_virtual_media_upload_token_t token)
{
    ESP_RETURN_ON_FALSE(
        token != JETKVM_VIRTUAL_MEDIA_UPLOAD_TOKEN_INVALID,
        ESP_ERR_INVALID_ARG, TAG, "invalid upload owner token");
    return finish_upload_with_token(upload_id, token);
}

static esp_err_t abort_upload_with_token(
    const char *upload_id, jetkvm_virtual_media_upload_token_t token)
{
    ESP_RETURN_ON_ERROR(lock_context(), TAG, "failed to lock");
    bool exists = upload_matches(upload_id);
    bool matched = token == JETKVM_VIRTUAL_MEDIA_UPLOAD_TOKEN_INVALID ?
        exists && !s_ctx.upload_claim_pending &&
            s_ctx.upload_owner_token ==
                JETKVM_VIRTUAL_MEDIA_UPLOAD_TOKEN_INVALID :
        upload_token_matches(upload_id, token);
    if (!matched) {
        esp_err_t error = exists ? ESP_ERR_INVALID_STATE : ESP_ERR_NOT_FOUND;
        unlock_context();
        return error;
    }
    FRESULT result = f_close(&s_ctx.upload);
    if (result != FR_OK) {
        /* f_close is retryable with the IDF dynamic-buffer fix. Drop the
         * transport lease and its 32 KiB staging allocation, but preserve the
         * live FIL plus upload identity so the exact upload can be resumed and
         * a later terminal operation can retry the metadata sync. */
        s_ctx.upload_close_pending = true;
        s_ctx.upload_claim_pending = false;
        uint8_t *staging = clear_upload_owner_locked();
        unlock_context();
        heap_caps_free(staging);
        return fat_result(result);
    }
    s_ctx.upload_open = false;
    uint8_t *staging = clear_upload_owner_locked();
    /* Abort preserves both the incomplete data and its persisted identity so
     * only the same complete file can claim the committed prefix later. */
    clear_upload_session_locked();
    unlock_context();
    heap_caps_free(staging);
    return fat_result(result);
}

esp_err_t jetkvm_virtual_media_abort_upload_owned(
    const char *upload_id, jetkvm_virtual_media_upload_token_t token)
{
    ESP_RETURN_ON_FALSE(
        token != JETKVM_VIRTUAL_MEDIA_UPLOAD_TOKEN_INVALID,
        ESP_ERR_INVALID_ARG, TAG, "invalid upload owner token");
    return abort_upload_with_token(upload_id, token);
}

esp_err_t jetkvm_virtual_media_abort_upload(const char *upload_id)
{
    return abort_upload_with_token(
        upload_id, JETKVM_VIRTUAL_MEDIA_UPLOAD_TOKEN_INVALID);
}

/* The card has left the slot. Ends what uses it: the image mounted from it,
 * whose drive the host then finds empty; an upload or a download to it,
 * whose next write fails with ESP_ERR_NOT_FOUND; and the filesystem. Open
 * files are dropped without a write, because the slot may already hold
 * another card. An image mounted from a URL and the built-in installer disc
 * stay mounted. Caller owns
 * s_ctx.mutex. */
static void remove_storage_locked(void)
{
#if JETKVM_TF_MSC_PREFETCH
    prefetch_invalidate_locked();
#endif
    if (s_ctx.image_open &&
        s_ctx.source == JETKVM_VIRTUAL_MEDIA_SOURCE_STORAGE) {
        ESP_LOGW(TAG, "TF card removed: unmounted image %s", s_ctx.filename);
        atomic_fetch_add(&s_medium_generation, 1U);
        s_ctx.image_open = false;
        s_ctx.image_writable = false;
        s_ctx.image_dirty = false;
        s_ctx.filename[0] = '\0';
        s_ctx.image_size = 0U;
        s_ctx.mode = JETKVM_VIRTUAL_MEDIA_MODE_CDROM;
    }
    if (s_ctx.upload_open) {
        ESP_LOGW(TAG, "TF card removed: ended the upload to %s",
                 s_ctx.upload_final_path);
    }
    heap_caps_free(clear_upload_owner_locked());
    clear_upload_session_locked();
    clear_upload_metadata_tracking_locked();
    if (s_ctx.card != NULL) {
        /* Unmounting forgets the volume without a write and releases only
         * the TF slot: on the Wi-Fi board the controller stays with the
         * Wi-Fi chip's slot. */
        esp_err_t error = esp_vfs_fat_sdcard_unmount(STORAGE_MOUNT_POINT,
                                                     s_ctx.card);
        if (error != ESP_OK) {
            ESP_LOGW(TAG, "TF unmount after removal failed: %s",
                     esp_err_to_name(error));
        }
        s_ctx.card = NULL;
    }
    memset(&s_ctx.image, 0, sizeof(s_ctx.image));
    memset(&s_ctx.upload, 0, sizeof(s_ctx.upload));
    memset(&s_ctx.upload_metadata, 0, sizeof(s_ctx.upload_metadata));
    s_ctx.filesystem_mounted = false;
    s_ctx.card_present = false;
    s_ctx.card_generation++;
    ESP_LOGI(TAG, "TF card removed");
}

/* Reads the card-detect pin every TF_DETECT_POLL_MS and acts on a change
 * that two readings in a row show (tf_detect_contract.h). */
static void tf_card_detect_task(void *argument)
{
    (void)argument;
    jetkvm_tf_detect_t detect;
    jetkvm_tf_detect_start(&detect, card_detected());
    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(TF_DETECT_POLL_MS));
        const bool present = card_detected();
        /* A transfer holds the lock for one card command at a time; wait
         * no longer than one period for it. */
        const bool storage_free =
            xSemaphoreTake(s_ctx.mutex, pdMS_TO_TICKS(TF_DETECT_POLL_MS)) ==
            pdTRUE;
        const jetkvm_tf_detect_action_t action = jetkvm_tf_detect_poll(
            &detect, present, storage_free,
            storage_free && s_ctx.card_present);
        if (!storage_free) {
            continue;
        }
        if (action.remove) {
            remove_storage_locked();
        }
        if (action.insert) {
            ESP_LOGI(TAG, "TF card inserted");
            esp_err_t error = init_storage_locked();
            if (error != ESP_OK) {
                ESP_LOGW(TAG, "TF card setup failed: %s",
                         esp_err_to_name(error));
            }
        }
        unlock_context();
    }
}

/* Without the task the card in the slot at the start stays the card, as
 * before; the rest of virtual media works either way. */
static void start_card_detect_locked(void)
{
    if (s_ctx.card_detect_task != NULL) {
        return;
    }
    if (xTaskCreateWithCaps(tf_card_detect_task, "tf_detect",
                            TF_DETECT_TASK_STACK_BYTES, NULL,
                            TF_DETECT_TASK_PRIORITY, &s_ctx.card_detect_task,
                            MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT) != pdPASS) {
        s_ctx.card_detect_task = NULL;
        ESP_LOGW(TAG, "TF card detection unavailable: no memory for its task");
    }
}

#if JETKVM_TF_CARD_TEST
void jetkvm_virtual_media_test_card_out(bool out)
{
    atomic_store(&s_card_test_removed, out);
    if (out) {
        /* Q1 off: the card loses its supply, as a card that leaves the
         * slot. Setting up the card again powers it through a reset. */
        (void)gpio_set_level(TF_POWER_GPIO, 1);
    }
}
#endif
