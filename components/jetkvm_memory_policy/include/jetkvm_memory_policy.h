#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "sdkconfig.h"
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Hard floors are post-allocation red lines.  Admission floors leave room for
 * short DTLS, HTTP, FatFs and driver allocation bursts which are not owned by
 * the caller performing the admission check. */
#define JETKVM_MEMORY_INTERNAL_HARD_FLOOR_BYTES (48U * 1024U)
#define JETKVM_MEMORY_INTERNAL_ADMISSION_FLOOR_BYTES (80U * 1024U)
#define JETKVM_MEMORY_DMA_HARD_FLOOR_BYTES (48U * 1024U)
#define JETKVM_MEMORY_DMA_ADMISSION_FLOOR_BYTES (64U * 1024U)
#define JETKVM_MEMORY_PSRAM_HARD_FLOOR_BYTES (2U * 1024U * 1024U)
#define JETKVM_MEMORY_PSRAM_ADMISSION_FLOOR_BYTES (4U * 1024U * 1024U)
#define JETKVM_MEMORY_LARGE_INTERNAL_LIMIT_BYTES (4U * 1024U)
#define JETKVM_MEMORY_CONTROL_ARENA_BYTES (1280U * 1024U)
#define JETKVM_MEMORY_LVGL_ARENA_BYTES (256U * 1024U)
#define JETKVM_MEMORY_POLICY_ARENAS_TOTAL_BYTES \
    (JETKVM_MEMORY_CONTROL_ARENA_BYTES + \
     JETKVM_MEMORY_LVGL_ARENA_BYTES)

/* The original 1.5 MiB early PSRAM carve-out is split into two physical
 * multi_heaps.  The generated twelve-screen/183-object LVGL graph owns the
 * 256 KiB heap; it can neither fragment the video heap nor consume control
 * headroom.  The remaining 1.25 MiB control heap has three exhaustive logical
 * partitions below. */
#define JETKVM_MEMORY_CONTROL_CLOUD_QUEUE_MAX_BYTES (512U * 1024U)
#define JETKVM_MEMORY_CONTROL_SIGNALING_MAX_BYTES (256U * 1024U)
#define JETKVM_MEMORY_CONTROL_RUNTIME_HEADROOM_BYTES (512U * 1024U)
#if JETKVM_MEMORY_CONTROL_CLOUD_QUEUE_MAX_BYTES + \
        JETKVM_MEMORY_CONTROL_SIGNALING_MAX_BYTES + \
        JETKVM_MEMORY_CONTROL_RUNTIME_HEADROOM_BYTES != \
    JETKVM_MEMORY_CONTROL_ARENA_BYTES
#error "control-plane quotas must exactly partition the control arena"
#endif
#if JETKVM_MEMORY_POLICY_ARENAS_TOTAL_BYTES != (1536U * 1024U)
#error "control plus LVGL arenas must retain the original 1.5 MiB carve-out"
#endif
#if (JETKVM_MEMORY_CONTROL_ARENA_BYTES % 64U) != 0U || \
        (JETKVM_MEMORY_LVGL_ARENA_BYTES % 64U) != 0U
#error "private arena boundaries must remain 64-byte aligned"
#endif

/* TLS reservations: the internal RAM that a connection has not allocated yet.
 * mbedTLS's own allocations are in PSRAM (CONFIG_MBEDTLS_EXTERNAL_MEM_ALLOC)
 * and are not counted here. Every internal allocation of a connection was
 * measured in DMA-capable RAM, so a reservation has the same size in both
 * domains. A size is the largest peak of the connection's own allocations
 * that was measured on either board, times 1.5, rounded up to the next KiB:
 *   - a connection with its handshake: 5,004 B measured;
 *   - a session's open, the peer with its DTLS handshake: 6,336 B;
 *   - a request on a connection that is already open: 896 B.
 * A reservation needs a free block of this size. */
#define JETKVM_MEMORY_TLS_CONNECTION_RESERVE_BYTES (8U * 1024U)
#define JETKVM_MEMORY_TLS_SESSION_RESERVE_BYTES (10U * 1024U)
#define JETKVM_MEMORY_TLS_REQUEST_RESERVE_BYTES (2U * 1024U)
/* A session that is opened with ICE servers, as a session through the cloud
 * is, was not measured. It has a cautious size: 16 KiB, 1.6 times the
 * reservation of a measured session's open. */
#define JETKVM_MEMORY_TLS_SESSION_ICE_RESERVE_BYTES (16U * 1024U)
/* The Wi-Fi link takes one block of DMA-capable RAM for each frame that
 * waits for the SDIO bus: 1,600 B and 4 B of heap bookkeeping. This is a
 * cost of the link, not of TLS, and the Ethernet board has none.
 *
 * While a connection was open or being made, 1 to 3 such blocks were
 * allocated at the lowest point, so every TLS reservation of the Wi-Fi
 * board has 3 blocks on top of its size.
 *
 * A firmware download fills the link's queue while flash is written, which
 * is more than the 16 KiB between the DMA floors, so a download reserves
 * the queue. ESP-Hosted queues
 * CONFIG_ESP_HOSTED_SDIO_TX_Q_SIZE station frames and refuses the next, one
 * more is at the bus and one is being built: that part is a bound. The
 * receive path held 1 block at the lowest point of each measured download
 * and can queue more; the reservation has 2 for it. */
#define JETKVM_MEMORY_WIFI_LINK_BLOCK_BYTES 1604U
#if CONFIG_ESP_HOSTED_ENABLED
#define JETKVM_MEMORY_TLS_LINK_BLOCKS 3U
#define JETKVM_MEMORY_DOWNLOAD_LINK_BLOCKS (CONFIG_ESP_HOSTED_SDIO_TX_Q_SIZE + 4U)
#else
#define JETKVM_MEMORY_TLS_LINK_BLOCKS 0U
#define JETKVM_MEMORY_DOWNLOAD_LINK_BLOCKS 0U
#endif
#define JETKVM_MEMORY_TLS_LINK_RESERVE_BYTES \
    (JETKVM_MEMORY_TLS_LINK_BLOCKS * JETKVM_MEMORY_WIFI_LINK_BLOCK_BYTES)
#define JETKVM_MEMORY_DOWNLOAD_LINK_RESERVE_BYTES \
    (JETKVM_MEMORY_DOWNLOAD_LINK_BLOCKS * JETKVM_MEMORY_WIFI_LINK_BLOCK_BYTES)
/* A waiting TLS reservation is tried this often, for at most this long,
 * while other owners' reservations keep it out. A reservation for one HTTPS
 * image request ends in milliseconds; one for a handshake ends when the
 * handshake does, 225 to 390 ms with a server that answers. */
#define JETKVM_MEMORY_TLS_RETRY_MS 20U
#define JETKVM_MEMORY_TLS_WAIT_MS 2000U
/* While a TLS reservation is held, the policy reads the free internal and
 * DMA-capable RAM this often, for what it reports of the hold. */
#define JETKVM_MEMORY_TLS_WATCH_MS 20U
/* wait_reserve_psram() retries this often, for at most this long, while a
 * video rebuild with a PSRAM working set refuses PSRAM tickets. A rebuild
 * ends when its capture
 * candidate locks or fails; a switch from OS Services video to HDMI capture
 * took under 0.5 s on the bench, and failed candidates re-arm for longer. */
#define JETKVM_MEMORY_PSRAM_RETRY_MS 20U
#define JETKVM_MEMORY_PSRAM_WAIT_MS 10000U

/* A full Go-compatible 25x10x10 saved-macro response is just under 100 KiB
 * with the longest canonical key names and 50-code-unit UTF-8 macro names.
 * Keep one 128 KiB PSRAM buffer so the advertised limit is actually usable. */
#define JETKVM_MEMORY_RPC_SCRATCH_BYTES (128U * 1024U)

/* IDF sys_mbox_new() stores one 32-bit pointer per queue entry in a single
 * Internal-RAM allocation. The queue control block is the same size in every
 * A/B build, so only pointer-slot growth over the former 64-entry baseline is
 * recorded for the global queue. Packet buffers and tcpip_msg objects remain
 * demand allocated; a full 256-entry global queue can retain at most
 * 256 * 1522 bytes of frame payload, but it does not reserve that PSRAM at
 * boot. Each live TCP netconn owns its own receive mailbox: 256 entries, one
 * internal-RAM pointer each, so a browser's six keep-alive connections cost
 * 6 KiB rather than the 24 KiB the former 1024-entry setting held while
 * peer setup competed for the same internal RAM. */
#define JETKVM_MEMORY_LWIP_MBOX_POINTER_BYTES 4U
#define JETKVM_MEMORY_LWIP_TCPIP_MBOX_BASELINE_ENTRIES 64U
#define JETKVM_MEMORY_LWIP_MBOX_AB_ENTRIES 256U
#define JETKVM_MEMORY_LWIP_TCP_MBOX_ENTRIES 256U
#define JETKVM_MEMORY_LWIP_MAX_TCP_SOCKETS 16U
#define JETKVM_MEMORY_ETHERNET_MAX_FRAME_BYTES 1522U
#define JETKVM_MEMORY_LWIP_TCPIP_MBOX_AB_GROWTH_BYTES \
    ((JETKVM_MEMORY_LWIP_MBOX_AB_ENTRIES - \
      JETKVM_MEMORY_LWIP_TCPIP_MBOX_BASELINE_ENTRIES) * \
     JETKVM_MEMORY_LWIP_MBOX_POINTER_BYTES)
#define JETKVM_MEMORY_LWIP_TCP_MBOX_PER_SOCKET_BYTES \
    (JETKVM_MEMORY_LWIP_TCP_MBOX_ENTRIES * \
     JETKVM_MEMORY_LWIP_MBOX_POINTER_BYTES)
#define JETKVM_MEMORY_LWIP_TCP_MBOX_ALL_SOCKETS_BYTES \
    (JETKVM_MEMORY_LWIP_TCP_MBOX_PER_SOCKET_BYTES * \
     JETKVM_MEMORY_LWIP_MAX_TCP_SOCKETS)
#define JETKVM_MEMORY_LWIP_TCPIP_MBOX_AB_PAYLOAD_MAX_BYTES \
    (JETKVM_MEMORY_LWIP_MBOX_AB_ENTRIES * \
     JETKVM_MEMORY_ETHERNET_MAX_FRAME_BYTES)

/* Four MiB is the total active storage-ingress budget. File payloads use the
 * HTTP path, whose configured scaled TCP window is charged directly below.
 * DataChannel carries only bounded RPC/HID control traffic, so its SCTP
 * receive window stays at 256 KiB instead of reserving a second
 * bandwidth-delay product for an unused bulk path. */
#define JETKVM_MEMORY_STORAGE_INGRESS_TOTAL_MAX_BYTES (4U * 1024U * 1024U)
#define JETKVM_MEMORY_RTC_SHARED_POOL_BYTES (2U * 1024U * 1024U)
#define JETKVM_MEMORY_RTC_PROTOCOL_METADATA_MAX_BYTES (4U * 1024U)
#define JETKVM_MEMORY_HTTP_WRITER_POOL_BYTES (512U * 1024U)
#define JETKVM_MEMORY_HTTP_TCP_RECEIVE_MAX_BYTES \
    CONFIG_LWIP_TCP_WND_DEFAULT
#define JETKVM_MEMORY_HTTP_INGRESS_MAX_BYTES \
    (JETKVM_MEMORY_HTTP_WRITER_POOL_BYTES + \
     JETKVM_MEMORY_HTTP_TCP_RECEIVE_MAX_BYTES)
#define JETKVM_MEMORY_SCTP_RECEIVE_MAX_BYTES (256U * 1024U)
#define JETKVM_MEMORY_RTC_INGRESS_HEADROOM_BYTES \
    (JETKVM_MEMORY_STORAGE_INGRESS_TOTAL_MAX_BYTES - \
     JETKVM_MEMORY_RTC_SHARED_POOL_BYTES - \
     JETKVM_MEMORY_SCTP_RECEIVE_MAX_BYTES - \
     JETKVM_MEMORY_RTC_PROTOCOL_METADATA_MAX_BYTES)
#if JETKVM_MEMORY_RTC_SHARED_POOL_BYTES + \
        JETKVM_MEMORY_SCTP_RECEIVE_MAX_BYTES + \
        JETKVM_MEMORY_RTC_PROTOCOL_METADATA_MAX_BYTES > \
    JETKVM_MEMORY_STORAGE_INGRESS_TOTAL_MAX_BYTES
#error "RTC shared pool, SCTP receive, and metadata exceed storage ingress"
#endif
#if JETKVM_MEMORY_HTTP_INGRESS_MAX_BYTES > \
    JETKVM_MEMORY_STORAGE_INGRESS_TOTAL_MAX_BYTES
#error "HTTP TCP window and writer exceed storage-ingress total"
#endif

#define JETKVM_MEMORY_RESERVATION_SLOTS 12U

typedef struct {
    /* Zero means expensive largest/block-count details have not been sampled. */
    uint64_t detailed_sample_timestamp_us;
    uint32_t internal_free;
    uint32_t internal_minimum;
    uint32_t internal_largest;
    uint32_t dma_free;
    uint32_t dma_minimum;
    uint32_t dma_largest;
    uint32_t psram_free;
    uint32_t psram_minimum;
    uint32_t psram_largest;
    uint32_t control_arena_free;
    uint32_t control_arena_minimum;
    uint32_t control_arena_largest;
    uint32_t control_allocation_failures;
    uint32_t lvgl_arena_capacity;
    uint32_t lvgl_arena_free;
    uint32_t lvgl_arena_minimum;
    uint32_t lvgl_arena_largest;
    uint32_t lvgl_arena_used;
    uint32_t lvgl_arena_peak_used;
    uint32_t lvgl_allocated_blocks;
    uint32_t lvgl_free_blocks;
    uint32_t lvgl_allocation_failures;
    uint32_t json_allocation_failures;
    uint32_t admission_denials;
    uint32_t reservation_internal_bytes;
    uint32_t reservation_dma_bytes;
    uint32_t reservation_psram_bytes;
    uint32_t reservation_storage_ingress_bytes;
    uint32_t active_reservations;
    uint32_t active_storage_ingress_reservations;
    uint32_t tls_reservation_internal_bytes;
    uint32_t tls_reservation_dma_bytes;
    uint32_t active_tls_reservations;
    bool video_rebuild_active;
    uint32_t video_rebuild_working_set_bytes;
    uint32_t video_rebuild_begin_denials;
    uint32_t video_rebuild_failures;
    uint32_t video_rebuild_blocked_materializations;
} jetkvm_memory_policy_snapshot_t;

typedef struct {
    /* DMA-capable internal RAM is a subset of internal RAM. internal_bytes is
     * the total internal decrease including dma_bytes, never an independent
     * bucket; requests with dma_bytes > internal_bytes are rejected. */
    size_t internal_bytes;
    size_t internal_largest_block;
    size_t dma_bytes;
    size_t dma_largest_block;
    size_t psram_bytes;
    size_t psram_largest_block;
    /* Logical data already resident or allowed to materialize in the shared
     * storage ingress window. This remains reserved after physical PSRAM
     * allocation so RTC/SCTP/future reorder owners cannot exceed 4 MiB. */
    size_t storage_ingress_bytes;
} jetkvm_memory_policy_request_t;

typedef struct {
    uint32_t generation;
    uint8_t slot;
    bool active;
} jetkvm_memory_policy_reservation_t;

typedef struct {
    uint32_t generation;
    bool active;
} jetkvm_memory_policy_video_rebuild_t;

/* TLS admissions since boot, per owner. Headroom is what the free bytes keep
 * above the admission floor after the held reservations and this request; a
 * refused request shows how far it was short as a negative value.
 *
 * The held_* fields are what the policy saw while the owner held a
 * reservation. The policy reads the free bytes every
 * JETKVM_MEMORY_TLS_WATCH_MS while a TLS reservation is held, and at each
 * reservation and release of any owner and at each snapshot. Each reading
 * is the free bytes of all regions of the heap at one moment. A dip between
 * two readings is not seen. A fall is the free bytes at the owner's
 * admission less the lowest value seen.
 *
 * held_overruns counts the holds in which a fall was larger than everything
 * that was reserved during the hold (the reservations that were held at its
 * admission, its own, and every one admitted before its end) and the room
 * between the admission floor and the hard floor of that domain together.
 * Such a fall takes free memory under the hard floor when the admission was
 * at its limit, so a reservation is too small for its owner, or another
 * task took more than the room between the floors.
 *
 * held_under_floor counts the holds in which free internal or DMA-capable
 * RAM was seen under its admission floor: the room between the floors was
 * in use while the reservation was held. */
/* Fifteen owners use TLS today; a sixteenth still fits. Owner names must be
 * string literals: the records keep the pointer until reboot. */
#define JETKVM_MEMORY_TLS_ADMISSION_OWNERS 16U
typedef struct {
    const char *owner;
    uint32_t admitted;
    uint32_t denied;
    int32_t last_internal_headroom;
    int32_t min_internal_headroom;
    int32_t last_dma_headroom;
    int32_t min_dma_headroom;
    /* The smallest largest free block at one of the owner's requests. */
    uint32_t min_internal_largest;
    uint32_t min_dma_largest;
    /* What the owner holds now. */
    uint32_t reserved_internal;
    uint32_t reserved_dma;
    /* UINT32_MAX until the owner's first admission. */
    uint32_t held_internal_min;
    uint32_t held_dma_min;
    uint32_t held_internal_fall_max;
    uint32_t held_dma_fall_max;
    uint32_t held_overruns;
    uint32_t held_under_floor;
} jetkvm_memory_policy_tls_admission_t;

/** Create the control/LVGL PSRAM heaps and install cJSON hooks before workers. */
esp_err_t jetkvm_memory_policy_init(void);

/**
 * Check a projected transient allocation against the global admission floors.
 * This is a fail-closed preflight, not an allocator or a lifetime reservation.
 */
bool jetkvm_memory_policy_admit(
    const char *owner, const jetkvm_memory_policy_request_t *request);

/**
 * Serialize admission for a bounded request. Short allocation tickets are
 * released after allocation; owners that can reconnect or grow retain only
 * their explicitly documented future headroom until teardown.
 */
bool jetkvm_memory_policy_reserve(
    const char *owner, const jetkvm_memory_policy_request_t *request,
    jetkvm_memory_policy_reservation_t *reservation);

/**
 * Reserve one PSRAM allocation that takes no internal or DMA RAM, such as a
 * heap_caps_malloc(MALLOC_CAP_SPIRAM) buffer. Only the PSRAM floor, pending
 * PSRAM tickets and the largest PSRAM block decide: the allocation cannot
 * take the internal or DMA heap under its floor, so other owners' internal
 * or DMA tickets do not refuse it.
 */
bool jetkvm_memory_policy_reserve_psram(
    const char *owner, size_t bytes,
    jetkvm_memory_policy_reservation_t *reservation);

/**
 * reserve_psram() that waits out a video rebuild with a PSRAM working set
 * (a debug capture's), which refuses every PSRAM ticket while
 * it runs: it retries every JETKVM_MEMORY_PSRAM_RETRY_MS for up
 * to JETKVM_MEMORY_PSRAM_WAIT_MS, without recording a denial, then calls
 * reserve_psram() once, which records the outcome. The request is judged
 * against memory only when no such rebuild is active, because that rebuild
 * changes PSRAM: a refusal for memory (the floor, the largest block, a full slot
 * table) returns at once when no rebuild runs, and as soon as the rebuild
 * ends when one does. It blocks the calling task, so it is not for the
 * TinyUSB task.
 */
bool jetkvm_memory_policy_wait_reserve_psram(
    const char *owner, size_t bytes,
    jetkvm_memory_policy_reservation_t *reservation);

/**
 * Peer admission. ESP_ERR_NOT_FINISHED means an active video transaction
 * deferred this attempt: no ticket, heap snapshot or admission failure is
 * produced; retry after yielding. Real budget failures return ESP_ERR_NO_MEM
 * and increment denials.
 */
esp_err_t jetkvm_memory_policy_try_reserve_peer(
    const char *owner, const jetkvm_memory_policy_request_t *request,
    jetkvm_memory_policy_reservation_t *reservation);

/**
 * Reserve JETKVM_MEMORY_TLS_CONNECTION_RESERVE_BYTES for one TLS connection
 * with its handshake, and JETKVM_MEMORY_TLS_LINK_RESERVE_BYTES on the Wi-Fi
 * board, as every TLS reservation. It fails at once.
 */
bool jetkvm_memory_policy_reserve_tls(
    const char *owner, jetkvm_memory_policy_reservation_t *reservation);

/**
 * reserve_tls() that waits for other owners' reservations. While only they
 * keep this one out (free memory would admit it), it tries again every
 * JETKVM_MEMORY_TLS_RETRY_MS for up to JETKVM_MEMORY_TLS_WAIT_MS and records
 * no denial for these tries; then it tries once more, which records the
 * outcome. When free memory alone is short, it is refused at once. It blocks
 * the calling task, so it is not for the TinyUSB task.
 */
bool jetkvm_memory_policy_wait_reserve_tls(
    const char *owner, jetkvm_memory_policy_reservation_t *reservation);

/**
 * Reserve for a connection that downloads a file, for as long as the
 * download runs: JETKVM_MEMORY_TLS_CONNECTION_RESERVE_BYTES for the
 * connection and JETKVM_MEMORY_DOWNLOAD_LINK_RESERVE_BYTES for the Wi-Fi
 * link's queue, which has the 3 blocks of a connection in it. A download
 * over plain HTTP reserves the same: its HTTP client, its socket and the
 * link take what they take with TLS. It waits as wait_reserve_tls() does,
 * and its admissions are in the owner's TLS record.
 */
bool jetkvm_memory_policy_wait_reserve_download(
    const char *owner, jetkvm_memory_policy_reservation_t *reservation);

/**
 * Reserve for a session's open, the peer's objects and its DTLS handshake:
 * JETKVM_MEMORY_TLS_SESSION_RESERVE_BYTES, or
 * JETKVM_MEMORY_TLS_SESSION_ICE_RESERVE_BYTES for a peer that is opened with
 * ICE servers. It fails at once.
 */
bool jetkvm_memory_policy_reserve_tls_session(
    const char *owner, bool ice_servers,
    jetkvm_memory_policy_reservation_t *reservation);

/**
 * Reserve JETKVM_MEMORY_TLS_REQUEST_RESERVE_BYTES for one request on a TLS
 * connection that is already open, and wait for other owners' reservations
 * as wait_reserve_tls() does: a reservation counts in full until it ends,
 * also for the part that its owner has already allocated, so a request
 * beside two handshakes can miss the floor although free memory would admit
 * it. A request that may connect or handshake uses wait_reserve_tls()
 * instead. It blocks the calling task, so it is not for the TinyUSB task.
 */
bool jetkvm_memory_policy_wait_reserve_tls_open_connection(
    const char *owner, jetkvm_memory_policy_reservation_t *reservation);

void jetkvm_memory_policy_release(
    jetkvm_memory_policy_reservation_t *reservation);

/** Reduce an active reservation to what its owner has not allocated yet. */
bool jetkvm_memory_policy_shrink(
    jetkvm_memory_policy_reservation_t *reservation,
    const jetkvm_memory_policy_request_t *remaining_request);

/**
 * Start an exclusive video allocation transaction after the old H.264/CSI
 * mode has been completely destroyed. The capture slots and the encoder DB
 * are allocated once at boot, so the working set is 0 except for the debug
 * captures: the BGR888 diagnostic's output and the CSI driver's backup
 * buffer. The working set is checked against free
 * PSRAM above the admission floor and against the largest block.
 *
 * While active, new reservations which would materialize PSRAM are denied,
 * except a PSRAM-only reservation (reserve_psram()) beside a rebuild whose
 * working set is 0: that rebuild allocates no PSRAM, so the reservation is
 * judged against memory as at any other time. Internal-only/TLS operations
 * and already-materialized logical ingress leases remain valid.
 */
esp_err_t jetkvm_memory_policy_video_rebuild_begin(
    const char *owner, size_t new_working_set_bytes,
    jetkvm_memory_policy_video_rebuild_t *transaction);

/** End a video transaction and record whether the complete rebuild worked. */
esp_err_t jetkvm_memory_policy_video_rebuild_finish(
    jetkvm_memory_policy_video_rebuild_t *transaction, bool succeeded);

/** Bounded control-plane heap shared by cJSON and cloud signaling payloads. */
void *jetkvm_memory_policy_control_alloc(size_t size);
void *jetkvm_memory_policy_control_realloc(void *memory, size_t size);
void jetkvm_memory_policy_control_free(void *memory);

/** Dedicated fail-closed LVGL heap; never falls back to control or general. */
void *jetkvm_memory_policy_lvgl_alloc(size_t size);
void *jetkvm_memory_policy_lvgl_realloc(void *memory, size_t size);
void jetkvm_memory_policy_lvgl_free(void *memory);
bool jetkvm_memory_policy_lvgl_check(void);

void jetkvm_memory_policy_get_snapshot(
    jetkvm_memory_policy_snapshot_t *snapshot);

/** Copy the per-owner TLS admission records; returns how many were copied. */
size_t jetkvm_memory_policy_get_tls_admissions(
    jetkvm_memory_policy_tls_admission_t *admissions, size_t capacity);

/** Live telemetry: fresh free/minimum/counters, cached largest/block counts.
 * Exact admission checks and explicit diagnostics retain get_snapshot(). */
void jetkvm_memory_policy_get_snapshot_cached(
    jetkvm_memory_policy_snapshot_t *snapshot);

#ifdef __cplusplus
}
#endif
