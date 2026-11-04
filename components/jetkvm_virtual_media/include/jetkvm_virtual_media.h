/*
 * SPDX-FileCopyrightText: 2026
 * SPDX-License-Identifier: MIT
 */

#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <time.h>

#include "esp_err.h"
#include "jetkvm_virtual_media_format.h"
#include "jetkvm_virtual_media_upload_identity.h"

#ifdef __cplusplus
extern "C" {
#endif

#define JETKVM_VIRTUAL_MEDIA_FILENAME_CAPACITY 256U
#define JETKVM_VIRTUAL_MEDIA_UPLOAD_ID_CAPACITY 48U
#define JETKVM_VIRTUAL_MEDIA_URL_CAPACITY 2048U
#define JETKVM_VIRTUAL_MEDIA_REASON_CAPACITY 192U
#define JETKVM_VIRTUAL_MEDIA_MAX_LISTED_FILES 64U
#define JETKVM_VIRTUAL_MEDIA_BLOCK_SIZE 512U

typedef enum {
    JETKVM_VIRTUAL_MEDIA_MODE_CDROM = 0,
    JETKVM_VIRTUAL_MEDIA_MODE_DISK,
} jetkvm_virtual_media_mode_t;

typedef enum {
    JETKVM_VIRTUAL_MEDIA_SOURCE_STORAGE = 0,
    JETKVM_VIRTUAL_MEDIA_SOURCE_HTTP,
    /** The installer disc in the firmware (mount_builtin_installer). */
    JETKVM_VIRTUAL_MEDIA_SOURCE_BUILTIN,
} jetkvm_virtual_media_source_t;

/** The filename the state gives for the built-in installer disc. */
#define JETKVM_VIRTUAL_MEDIA_BUILTIN_INSTALLER_FILENAME "Install OS Services.iso"

typedef struct {
    bool initialized;
    bool card_present;
    /** Advances each time a card leaves the slot, so two states with the
     * same card_present tell whether they describe the same card. */
    uint32_t card_generation;
    bool filesystem_mounted;
    bool image_mounted;
    jetkvm_virtual_media_source_t source;
    jetkvm_virtual_media_mode_t mode;
    /** Names the medium: every mount, and every unmount that closes an
     * image, advances it. */
    uint32_t medium_generation;
    char filename[JETKVM_VIRTUAL_MEDIA_FILENAME_CAPACITY];
    char url[JETKVM_VIRTUAL_MEDIA_URL_CAPACITY];
    uint64_t size;
} jetkvm_virtual_media_state_t;

typedef struct {
    bool usable;
    uint64_t size;
    char reason[JETKVM_VIRTUAL_MEDIA_REASON_CAPACITY];
} jetkvm_virtual_media_url_info_t;

typedef struct {
    uint64_t bytes_used;
    uint64_t bytes_free;
} jetkvm_virtual_media_space_t;

typedef struct {
    char filename[JETKVM_VIRTUAL_MEDIA_FILENAME_CAPACITY];
    uint64_t size;
    time_t created_at;
} jetkvm_virtual_media_file_t;

typedef struct {
    size_t count;
    bool truncated;
    jetkvm_virtual_media_file_t files[JETKVM_VIRTUAL_MEDIA_MAX_LISTED_FILES];
} jetkvm_virtual_media_file_list_t;

typedef struct {
    char upload_id[JETKVM_VIRTUAL_MEDIA_UPLOAD_ID_CAPACITY];
    uint64_t already_uploaded_bytes;
} jetkvm_virtual_media_upload_t;

/** Exclusive writer identity for one resumable upload. */
typedef enum {
    JETKVM_VIRTUAL_MEDIA_UPLOAD_OWNER_HTTP = 1,
    JETKVM_VIRTUAL_MEDIA_UPLOAD_OWNER_WEBRTC,
    /** The device's own HTTPS download (jetkvm_storage_download.h). */
    JETKVM_VIRTUAL_MEDIA_UPLOAD_OWNER_DOWNLOAD,
} jetkvm_virtual_media_upload_owner_t;

/**
 * Opaque, non-zero capability returned by claim_upload().  A token is valid
 * only for the upload generation that issued it and is consumed by finish or
 * abort.  Callers that merely stop producing must release it explicitly.
 */
typedef uint64_t jetkvm_virtual_media_upload_token_t;
#define JETKVM_VIRTUAL_MEDIA_UPLOAD_TOKEN_INVALID UINT64_C(0)

/** Cumulative backing-store read telemetry since the latest image mount. */
typedef struct {
    uint64_t calls;
    uint64_t bytes_requested;
    uint64_t bytes_read;
    uint64_t errors;
    uint64_t total_us;
    /* Time spent waiting for the media mutex before a READ(10) can inspect
     * the prefetch state. This separates backing-store serialization from
     * FatFs and memcpy work without logging every endpoint fragment. */
    uint64_t lock_wait_us;
    uint64_t seek_us;
    uint64_t read_us;
    uint32_t max_us;
    uint32_t max_lock_wait_us;
    uint32_t max_seek_us;
    uint32_t max_read_us;
    uint64_t prefetch_jobs;
    uint64_t prefetch_hits;
    uint64_t prefetch_misses;
    uint64_t prefetch_hit_bytes;
    uint64_t prefetch_miss_bytes;
    uint64_t prefetch_bytes;
    uint64_t prefetch_total_us;
    uint64_t prefetch_seek_us;
    uint64_t prefetch_read_us;
    uint64_t prefetch_copy_us;
    /* Notify-to-worker-start latency and ready-to-consumer lead time expose
     * scheduler starvation versus a producer that simply cannot outrun USB. */
    uint64_t prefetch_schedule_delay_us;
    uint64_t prefetch_worker_lock_wait_us;
    uint64_t prefetch_ready_age_us;
    uint64_t prefetch_errors;
    uint32_t prefetch_max_us;
    uint32_t prefetch_max_schedule_delay_us;
    uint32_t prefetch_max_worker_lock_wait_us;
    uint32_t prefetch_max_ready_age_us;
    uint32_t prefetch_stack_high_watermark;
} jetkvm_virtual_media_io_stats_t;

/** Cumulative local-image WRITE(10) telemetry since the latest image mount. */
typedef struct {
    uint64_t calls;
    uint64_t bytes_requested;
    uint64_t bytes_written;
    uint64_t errors;
    uint64_t total_us;
    uint64_t lock_wait_us;
    uint64_t seek_us;
    uint64_t write_us;
    uint64_t sync_calls;
    uint64_t sync_errors;
    uint64_t sync_us;
    uint32_t max_us;
    uint32_t max_lock_wait_us;
    uint32_t max_seek_us;
    uint32_t max_write_us;
    uint32_t max_sync_us;
    bool dirty;
} jetkvm_virtual_media_write_io_stats_t;

/** Cumulative upload-write telemetry since the latest successful start. */
typedef struct {
    uint64_t calls;
    uint64_t bytes_requested;
    uint64_t bytes_written;
    uint64_t errors;
    uint64_t total_us;
    uint64_t direct_calls;
    uint64_t direct_bytes;
    uint64_t direct_us;
    uint64_t bounce_calls;
    uint64_t bounce_bytes;
    uint64_t bounce_copy_us;
    uint64_t bounce_write_us;
    uint64_t staging_allocations;
    uint64_t staging_allocation_failures;
    uint64_t staging_releases;
    uint32_t staging_bytes;
    bool staging_active;
} jetkvm_virtual_media_upload_io_stats_t;

/** Telemetry since storage initialization for the optional SD ACMD23
 * pre-erase (build option JETKVM_TF_SD_ACMD23, off by default). */
typedef struct {
    bool enabled;
    bool unsupported_latched;
    uint32_t attempts;
    uint32_t successes;
    uint32_t unsupported;
    uint32_t failures;
    uint32_t last_block_count;
    esp_err_t last_error;
} jetkvm_virtual_media_sd_write_hint_stats_t;

/**
 * Initialize HTTP virtual media and, when present, mount the exFAT-backed TF
 * card at /sdcard and create /sdcard/jetkvm/images.
 * Existing media is never formatted. The socket uses native 4-bit SDMMC with
 * GPIO43 CLK, GPIO44 CMD and GPIO39..42 DAT0..DAT3; GPIO45 is the active-low
 * card-detect input. A task reads it every 250 ms: a card that is inserted
 * is mounted, and a card that is removed ends the image mounted from it and
 * an upload or download to it, and is unmounted.
 */
esp_err_t jetkvm_virtual_media_init(void);

/**
 * Erase the detected TF card: create a fresh exFAT filesystem with
 * the images folder, mounted at once, and give its space in `space`. A card
 * whose filesystem is mounted is unmounted first; an upload without a writer
 * is closed. Refused, with nothing changed, before the storage service has
 * started, without a card, while an image from the card is mounted and while
 * an upload or a download writes to the card (storage_format_contract.h).
 */
jetkvm_virtual_media_format_result_t jetkvm_virtual_media_format_exfat(
    jetkvm_virtual_media_space_t *space);

/** The card-detect pin now: true with a card in the slot. It can differ
 * from card_present for up to 500 ms, until the card-detect task has
 * counted the change. */
bool jetkvm_virtual_media_card_in_slot(void);

/** Copy one coherent state snapshot. */
esp_err_t jetkvm_virtual_media_get_state(jetkvm_virtual_media_state_t *state);

esp_err_t jetkvm_virtual_media_get_space(jetkvm_virtual_media_space_t *space);
esp_err_t jetkvm_virtual_media_list_files(jetkvm_virtual_media_file_list_t *list);
esp_err_t jetkvm_virtual_media_delete_file(const char *filename);
/**
 * Size of one complete image in the images folder. ESP_ERR_NOT_FOUND when it
 * does not exist, ESP_ERR_INVALID_STATE without a mounted filesystem.
 */
esp_err_t jetkvm_virtual_media_file_size(const char *filename, uint64_t *size);
/** True while an upload writer (browser or device download) holds the lease. */
bool jetkvm_virtual_media_upload_claimed(void);

/**
 * Called with true when a browser upload (HTTP or WebRTC owner) takes the
 * writer lease, and with false when that lease ends: the upload finished,
 * failed, was aborted or timed out, or its request was rejected after the
 * claim. The call is made with this component's lock held, so the calls
 * arrive in the order of the leases; the hook must not block and must not
 * call this component.
 */
typedef void (*jetkvm_virtual_media_upload_writer_fn)(bool active, void *ctx);
/** Set once at startup, before the HTTP server takes uploads. */
void jetkvm_virtual_media_set_upload_writer_hook(
    jetkvm_virtual_media_upload_writer_fn hook, void *ctx);

/**
 * Open one SD-backed image as the virtual LUN. A 512-byte-aligned local Disk
 * image is opened read/write; CDROM, HTTP and non-local media remain read-only.
 * Only a basename is accepted. The image remains open until unmount,
 * preventing rename/delete races while TinyUSB serves block I/O.
 */
esp_err_t jetkvm_virtual_media_mount(const char *filename,
                                    jetkvm_virtual_media_mode_t mode);
/** Validate a remote image with GET Range bytes=0-0. */
esp_err_t jetkvm_virtual_media_check_url(
    const char *url, jetkvm_virtual_media_url_info_t *info);
/** Mount a validated HTTP(S) byte-range object as a read-only LUN. */
esp_err_t jetkvm_virtual_media_mount_http(
    const char *url, jetkvm_virtual_media_mode_t mode);
/**
 * Mount the installer disc that the firmware carries as a read-only CD-ROM.
 * It needs no TF card and no network, and a card's removal leaves it
 * mounted. ESP_ERR_INVALID_STATE while another image is mounted, which
 * stays mounted.
 */
esp_err_t jetkvm_virtual_media_mount_builtin_installer(void);
esp_err_t jetkvm_virtual_media_unmount(void);
/**
 * Unmount for the host's eject of the medium of `medium_generation`. Fails
 * with ESP_ERR_INVALID_STATE, and unmounts nothing, when the medium is not
 * that one any more, so that an eject meant for one image never closes the
 * image that replaced it.
 */
esp_err_t jetkvm_virtual_media_eject(uint32_t medium_generation);

/**
 * Hold the medium lock while the USB stack stops. TinyUSB deletes its task
 * wherever it is; a task deleted inside an MSC callback would keep the lock.
 */
esp_err_t jetkvm_virtual_media_pause_io(void);
void jetkvm_virtual_media_resume_io(void);

/** The medium generation of the state snapshot. Does not wait for the medium lock. */
uint32_t jetkvm_virtual_media_medium_generation(void);

/**
 * Serve a byte range from the mounted image. Reads extending into the final
 * partial 512-byte block are zero padded, as required by MSC block transport.
 * Fails with ESP_ERR_INVALID_STATE when the medium is not the one of
 * `medium_generation`, so that a read meant for one image never returns
 * another image's data.
 */
esp_err_t jetkvm_virtual_media_read(uint32_t medium_generation,
                                   uint64_t offset, void *buffer, size_t size);
/**
 * Called for each video frame handed to the WebRTC peer. On the Wi-Fi Mini,
 * HTTP(S) image reads are paced while video frames are being sent.
 */
void jetkvm_virtual_media_note_video_frame(void);
/** True only for a mounted, local, 512-byte-aligned Disk image. */
bool jetkvm_virtual_media_is_writable(void);
/** True while an HTTP(S) image is mounted. Does not wait for the medium lock. */
bool jetkvm_virtual_media_is_remote(void);
/**
 * Write within the fixed size of a writable local Disk image. Fails with
 * ESP_ERR_INVALID_STATE when the medium is not the one of `medium_generation`.
 */
esp_err_t jetkvm_virtual_media_write(uint32_t medium_generation,
                                    uint64_t offset, const void *buffer,
                                    size_t size);
/** Flush dirty local-image data. Read-only media are a successful no-op. */
esp_err_t jetkvm_virtual_media_sync(void);
esp_err_t jetkvm_virtual_media_capacity(uint32_t *block_count,
                                       uint16_t *block_size);
esp_err_t jetkvm_virtual_media_get_io_stats(
    jetkvm_virtual_media_io_stats_t *stats);
esp_err_t jetkvm_virtual_media_get_write_io_stats(
    jetkvm_virtual_media_write_io_stats_t *stats);
esp_err_t jetkvm_virtual_media_get_upload_io_stats(
    jetkvm_virtual_media_upload_io_stats_t *stats);
esp_err_t jetkvm_virtual_media_get_sd_write_hint_stats(
    jetkvm_virtual_media_sd_write_hint_stats_t *stats);

/**
 * JetKVM-compatible resumable .incomplete upload lifecycle. sha256_hex must
 * be the complete file's SHA-256 encoded as exactly 64 hexadecimal digits;
 * it is persisted with the expected size and must match on every resume.
 * File offsets and sizes stay 64-bit end to end through FatFs' direct f_* API.
 */
esp_err_t jetkvm_virtual_media_start_upload(
    const char *filename, uint64_t size, const char *sha256_hex,
    jetkvm_virtual_media_upload_t *upload);

/**
 * Materialize the bounded internal-DMA staging buffer and atomically publish
 * the sole writer lease for an open upload. This is the ownership boundary
 * between the HTTP and WebRTC producers: after a claim, upload operations
 * without the matching token fail closed. The optional counters are copied
 * from the same locked snapshot as the published claim.
 */
esp_err_t jetkvm_virtual_media_claim_upload(
    const char *upload_id, jetkvm_virtual_media_upload_owner_t owner,
    jetkvm_virtual_media_upload_token_t *token,
    uint64_t *uploaded_bytes, uint64_t *expected_bytes);

/** Release a writer lease without closing or deleting the .incomplete file. */
esp_err_t jetkvm_virtual_media_release_upload(
    const char *upload_id, jetkvm_virtual_media_upload_token_t token);

esp_err_t jetkvm_virtual_media_write_upload_owned(
    const char *upload_id, jetkvm_virtual_media_upload_token_t token,
    const void *data, size_t size,
    uint64_t *uploaded_bytes, uint64_t *expected_bytes);
esp_err_t jetkvm_virtual_media_finish_upload_owned(
    const char *upload_id, jetkvm_virtual_media_upload_token_t token);
esp_err_t jetkvm_virtual_media_abort_upload_owned(
    const char *upload_id, jetkvm_virtual_media_upload_token_t token);

/* Aborts an unclaimed upload; it cannot bypass an HTTP/WebRTC writer lease.
 * Only a claimed writer writes or finishes, so an unclaimed session has no
 * writer and a start, delete or mount may close it. */
esp_err_t jetkvm_virtual_media_abort_upload(const char *upload_id);

#if JETKVM_TF_CARD_TEST
/**
 * Bench builds only: with `out`, the card reads as removed and loses its
 * supply, as a card that leaves the slot; without, it reads as present
 * again and is set up as an inserted card.
 */
void jetkvm_virtual_media_test_card_out(bool out);
#endif

#ifdef __cplusplus
}
#endif
