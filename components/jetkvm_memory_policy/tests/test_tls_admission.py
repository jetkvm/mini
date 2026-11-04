#!/usr/bin/env python3
"""Run the real TLS reservation logic against the measured heap values of both boards."""

from pathlib import Path
import re
import subprocess
import tempfile


ROOT = Path(__file__).resolve().parents[1]
SOURCE = (ROOT / 'jetkvm_memory_policy.c').read_text()
HEADER = (ROOT / 'include/jetkvm_memory_policy.h').read_text()


def function(name):
    match = re.search(r'(?:static )?(?:(?:bool|void|esp_err_t|int32_t|size_t) |jetkvm_memory_policy_tls_admission_t \*)' + name + r'\([^;]*?\)\s*\{', SOURCE, re.S)
    assert match, name
    depth, end = 1, match.end()
    while depth:
        depth += (SOURCE[end] == '{') - (SOURCE[end] == '}')
        end += 1
    return SOURCE[match.start():end]


logic = '\n'.join(function(name) for name in (
    'has_projected_floor', 'projected_headroom', 'record_tls_admission',
    'watch_held_tls', 'watch_sample', 'watch_run_locked', 'clamp_u32',
    'request_shape_valid', 'psram_fits', 'request_fits',
    'video_rebuild_blocks', 'reserve_with_domain', 'jetkvm_memory_policy_reserve',
    'jetkvm_memory_policy_try_reserve_peer',
    'reserve_tls_with_domain', 'wait_reserve_tls_with_domain',
    'jetkvm_memory_policy_reserve_tls',
    'jetkvm_memory_policy_wait_reserve_tls',
    'jetkvm_memory_policy_wait_reserve_download',
    'jetkvm_memory_policy_reserve_tls_session',
    'jetkvm_memory_policy_wait_reserve_tls_open_connection',
    'jetkvm_memory_policy_release', 'jetkvm_memory_policy_get_tls_admissions',
))

# The counts of Wi-Fi link blocks are the sizes that depend on the board: the
# header takes the download's from the queue length of the Wi-Fi build.
link = re.search(r'#if CONFIG_ESP_HOSTED_ENABLED\n#define JETKVM_MEMORY_TLS_LINK_BLOCKS (.*)\n'
                 r'#define JETKVM_MEMORY_DOWNLOAD_LINK_BLOCKS (.*)\n'
                 r'#else\n#define JETKVM_MEMORY_TLS_LINK_BLOCKS (.*)\n'
                 r'#define JETKVM_MEMORY_DOWNLOAD_LINK_BLOCKS (.*)\n#endif', HEADER)
assert link, 'the header defines the link blocks per board'
defaults = ROOT.parents[1]
queue = re.search(r'^CONFIG_ESP_HOSTED_SDIO_TX_Q_SIZE=(\d+)$',
                  (defaults / 'sdkconfig.wifi.defaults').read_text(), re.M)
assert queue and 'CONFIG_ESP_HOSTED_ENABLED=y' in (defaults / 'sdkconfig.wifi.defaults').read_text()
assert 'CONFIG_ESP_HOSTED_ENABLED=n' in (defaults / 'sdkconfig.ethernet.defaults').read_text()
BOARDS = {
    'wifi': ['-DWIFI=1', '-DCONFIG_ESP_HOSTED_SDIO_TX_Q_SIZE=' + queue.group(1),
             '-DJETKVM_MEMORY_TLS_LINK_BLOCKS=' + link.group(1),
             '-DJETKVM_MEMORY_DOWNLOAD_LINK_BLOCKS=' + link.group(2)],
    'ethernet': ['-DWIFI=0', '-DJETKVM_MEMORY_TLS_LINK_BLOCKS=' + link.group(3),
                 '-DJETKVM_MEMORY_DOWNLOAD_LINK_BLOCKS=' + link.group(4)],
}


def define(name):
    match = re.search(r'^#define ' + name + r'\s+((?:[^\n]*\\\n)*[^\n]*)', HEADER, re.M)
    assert match, name
    return match.group(0)


BY_BOARD = ('JETKVM_MEMORY_TLS_LINK_BLOCKS', 'JETKVM_MEMORY_DOWNLOAD_LINK_BLOCKS')
found = {}
pending = set(re.findall(r'JETKVM_MEMORY_\w+', logic))
while pending:
    name = pending.pop()
    if name not in found and name not in BY_BOARD:
        found[name] = define(name)
        pending |= set(re.findall(r'JETKVM_MEMORY_\w+', found[name]))
# A macro's text does not need the macros that it names before it.
macros = [found[name] for name in sorted(found)]
types = [value for value in re.findall(r'typedef struct \{.*?\} \w+;', HEADER, re.S)
         if value.endswith((' jetkvm_memory_policy_snapshot_t;',
                            ' jetkvm_memory_policy_request_t;',
                            ' jetkvm_memory_policy_reservation_t;',
                            ' jetkvm_memory_policy_tls_admission_t;'))]
assert len(types) == 4
slot = re.search(r'typedef struct \{.*?\} reservation_slot_t;', SOURCE, re.S).group(0)
prefix = r"""
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#include <stdatomic.h>
#include <stdio.h>
#include <string.h>
"""
mock = r"""
typedef int esp_err_t;
#define ESP_OK 0
#define ESP_ERR_NO_MEM 0x101
#define ESP_ERR_NOT_FINISHED 0x10c
#define MALLOC_CAP_INTERNAL 1U
#define MALLOC_CAP_8BIT 2U
#define MALLOC_CAP_DMA 4U
#define MALLOC_CAP_SPIRAM 8U
#define portMAX_DELAY UINT32_MAX
#define pdTRUE 1
#define ESP_LOGW(...) ((void)0)
static jetkvm_memory_policy_snapshot_t memory;
static int mutex_storage;
static void *s_reservation_mutex = &mutex_storage;
static bool held, deny_mutex;
static unsigned heap_reads;
static reservation_slot_t s_reservations[JETKVM_MEMORY_RESERVATION_SLOTS];
static uint32_t s_next_reservation_generation = 1;
static struct { bool active; size_t working_set_bytes; } s_video_rebuild;
static jetkvm_memory_policy_tls_admission_t
    tls_admission_storage[JETKVM_MEMORY_TLS_ADMISSION_OWNERS];
static jetkvm_memory_policy_tls_admission_t *s_tls_admissions = tls_admission_storage;
static atomic_uint s_admission_denials, s_video_rebuild_blocked_materializations;
/* The timer's callback does not wait for the mutex. */
static int xSemaphoreTake(void *mutex, uint32_t ticks) {
    assert(mutex == &mutex_storage && (ticks == 0 || (ticks == portMAX_DELAY && !held)));
    if (deny_mutex || held) return 0;
    held = true; return pdTRUE;
}
static void xSemaphoreGive(void *mutex) {
    assert(mutex == &mutex_storage && held); held = false;
}
/* The timer that reads free memory while a TLS reservation is held: the
 * test calls watch_sample() where the timer would. */
typedef int *esp_timer_handle_t;
static int timer_storage;
static esp_timer_handle_t s_watch_timer = &timer_storage;
static bool s_watch_running;
static bool timer_runs;
static esp_err_t esp_timer_start_periodic(esp_timer_handle_t timer, uint64_t period_us) {
    assert(held && timer == &timer_storage && !timer_runs && period_us == 20000);
    timer_runs = true; return ESP_OK;
}
static esp_err_t esp_timer_stop(esp_timer_handle_t timer) {
    assert(held && timer == &timer_storage && timer_runs);
    timer_runs = false; return ESP_OK;
}
static size_t heap_caps_get_free_size(unsigned caps) {
    assert(held);
    heap_reads++;
    if (caps == (MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT)) return memory.internal_free;
    if (caps == (MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA)) return memory.dma_free;
    assert(caps == (MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT)); return memory.psram_free;
}
void jetkvm_memory_policy_release(jetkvm_memory_policy_reservation_t *reservation);
/* vTaskDelay() counts the waits and can end another owner's reservation at one. */
#define pdMS_TO_TICKS(ms) (ms)
static unsigned delays, release_at_delay;
static jetkvm_memory_policy_reservation_t *release_at;
static void vTaskDelay(uint32_t ticks) {
    assert(!held && ticks == JETKVM_MEMORY_TLS_RETRY_MS);
    if (++delays == release_at_delay) jetkvm_memory_policy_release(release_at);
}
static size_t heap_caps_get_largest_free_block(unsigned caps) {
    assert(held);
    heap_reads++;
    if (caps == (MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT)) return memory.internal_largest;
    if (caps == (MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA)) return memory.dma_largest;
    assert(caps == (MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT)); return memory.psram_largest;
}
"""
cases = r"""
#define INTERNAL_FLOOR 81920U
#define DMA_FLOOR 65536U
/* The part of a reservation that must be free as one block. */
#define CONNECTION 8192U
#define SESSION 10240U
#define SESSION_ICE 16384U
#define REQUEST 2048U
/* On the Wi-Fi board every TLS reservation has 3 link blocks of 1,604 B on
 * top, and a download the link's queue: 8 queued frames, one at the bus, one
 * being built, two of the receive path. */
#define TLS_LINK (WIFI ? 4812U : 0U)
#define QUEUE (WIFI ? 19248U : 0U)
#define CONN (CONNECTION + TLS_LINK)
#define SESS (SESSION + TLS_LINK)
#define REQ (REQUEST + TLS_LINK)
/* What a running handshake has allocated at most. */
#define HANDSHAKE_PEAK 5004U

typedef jetkvm_memory_policy_reservation_t reservation_t;

/* Free internal and DMA-capable RAM, with nothing in the way of a block. */
static void heap(uint32_t internal, uint32_t dma) {
    assert(!held);
    memory = (jetkvm_memory_policy_snapshot_t) {
        .internal_free=internal, .dma_free=dma, .psram_free=14658044,
        .internal_largest=47104, .dma_largest=47104, .psram_largest=3080192,
    };
}
/* The timer fires. */
static void timer_reads(void) {
    assert(timer_runs && !held);
    watch_sample(NULL);
    assert(!held);
}
static void reset(void) {
    assert(!held);
    memset(s_reservations, 0, sizeof(s_reservations));
    memset(tls_admission_storage, 0, sizeof(tls_admission_storage));
    s_video_rebuild.active = false;
    deny_mutex = false;
    heap_reads = 0;
    release_at = NULL; release_at_delay = 0; delays = 0;
    s_admission_denials = s_video_rebuild_blocked_materializations = 0;
    timer_runs = s_watch_running = false;
    heap(154600, 123700);
}
/* The owner allocates: free memory falls. */
static void allocate(uint32_t bytes) { memory.internal_free -= bytes; memory.dma_free -= bytes; }
static void give_back(uint32_t bytes) { memory.internal_free += bytes; memory.dma_free += bytes; }
static const reservation_slot_t *slot_of(const reservation_t *reservation) {
    assert(reservation->active && s_reservations[reservation->slot].active);
    return &s_reservations[reservation->slot];
}
/* The same bytes in both domains, as TLS memory with no PSRAM. */
static void expect_shape(const reservation_t *reservation, size_t bytes, size_t largest) {
    const jetkvm_memory_policy_request_t *request = &slot_of(reservation)->request;
    assert(slot_of(reservation)->tls);
    assert(request->internal_bytes == bytes && request->dma_bytes == bytes);
    assert(request->internal_largest_block == largest && request->dma_largest_block == largest);
    assert(request->psram_bytes == 0 && request->psram_largest_block == 0);
    assert(request->storage_ingress_bytes == 0);
}
static const jetkvm_memory_policy_tls_admission_t *record_of(const char *owner) {
    for (size_t i = 0; i < JETKVM_MEMORY_TLS_ADMISSION_OWNERS; i++)
        if (s_tls_admissions[i].owner && strcmp(s_tls_admissions[i].owner, owner) == 0)
            return &s_tls_admissions[i];
    return NULL;
}
static unsigned active_slots(void) {
    unsigned count = 0;
    for (size_t i = 0; i < JETKVM_MEMORY_RESERVATION_SLOTS; i++) count += s_reservations[i].active;
    return count;
}
/* A handshake that is admitted and has allocated its peak. */
static bool handshake(const char *owner, reservation_t *reservation) {
    if (!jetkvm_memory_policy_reserve_tls(owner, reservation)) return false;
    allocate(HANDSHAKE_PEAK);
    return true;
}
static bool local_session(const char *owner, reservation_t *reservation) {
    return jetkvm_memory_policy_reserve_tls_session(owner, false, reservation);
}
static bool cloud_session(const char *owner, reservation_t *reservation) {
    return jetkvm_memory_policy_reserve_tls_session(owner, true, reservation);
}

/* Each kind of reservation, and the floor to the byte in each domain. */
static void sizes_and_floors(void) {
    reset();
    reservation_t connection = {0}, session = {0}, request = {0}, download = {0};
    assert(jetkvm_memory_policy_reserve_tls("cloud-wss", &connection));
    expect_shape(&connection, CONN, CONNECTION);
    jetkvm_memory_policy_release(&connection);
    assert(jetkvm_memory_policy_wait_reserve_tls("ota-download", &connection));
    expect_shape(&connection, CONN, CONNECTION);
    jetkvm_memory_policy_release(&connection);
    assert(local_session("webrtc-dtls", &session));
    expect_shape(&session, SESS, SESSION);
    jetkvm_memory_policy_release(&session);
    /* A session with ICE servers was not measured and keeps 16 KiB. */
    assert(cloud_session("webrtc-dtls", &session));
    expect_shape(&session, SESSION_ICE + TLS_LINK, SESSION_ICE);
    jetkvm_memory_policy_release(&session);
    assert(jetkvm_memory_policy_wait_reserve_tls_open_connection("https-media-range", &request));
    expect_shape(&request, REQ, REQUEST);
    jetkvm_memory_policy_release(&request);
    /* A download: the connection and the link's queue, which has the
     * connection's 3 blocks in it. The largest block is the connection's
     * part, because the link's part is single blocks. */
    assert(jetkvm_memory_policy_wait_reserve_download("ota-download", &download));
    expect_shape(&download, CONNECTION + QUEUE, CONNECTION);
    jetkvm_memory_policy_release(&download);
    assert(!held && active_slots() == 0);

    struct { size_t bytes; bool (*reserve)(const char *, reservation_t *); } kinds[] = {
        { CONN, jetkvm_memory_policy_reserve_tls },
        { SESS, local_session },
        { SESSION_ICE + TLS_LINK, cloud_session },
        { REQ, jetkvm_memory_policy_wait_reserve_tls_open_connection },
    };
    for (size_t i = 0; i < sizeof(kinds) / sizeof(kinds[0]); i++) {
        reset();
        reservation_t one = {0};
        const uint32_t internal = INTERNAL_FLOOR + kinds[i].bytes, dma = DMA_FLOOR + kinds[i].bytes;
        heap(internal - 1, dma);
        assert(!kinds[i].reserve("owner", &one) && !one.active && delays == 0);
        assert(record_of("owner")->denied == 1 && record_of("owner")->last_internal_headroom == -1);
        heap(internal, dma - 1);
        assert(!kinds[i].reserve("owner", &one) && !one.active && delays == 0);
        assert(record_of("owner")->denied == 2 && record_of("owner")->last_dma_headroom == -1);
        heap(internal, dma);
        assert(kinds[i].reserve("owner", &one) && one.active);
        assert(record_of("owner")->admitted == 1 && record_of("owner")->last_internal_headroom == 0);
        assert(record_of("owner")->last_dma_headroom == 0 && record_of("owner")->min_dma_headroom == -1);
        assert(s_admission_denials == 2);
        jetkvm_memory_policy_release(&one);
    }
}

/* Free bytes are not enough when no free block holds the reservation's
 * block. The link's blocks are no part of it. */
static void largest_block(void) {
    reset();
    reservation_t connection = {0}, session = {0}, request = {0};
    memory.dma_largest = CONNECTION - 1;
    assert(!jetkvm_memory_policy_reserve_tls("cloud-wss", &connection));
    memory.dma_largest = CONNECTION;
    assert(!local_session("webrtc-dtls", &session));
    assert(jetkvm_memory_policy_reserve_tls("cloud-wss", &connection));
    /* The owner's record keeps the smallest largest free block of its requests. */
    assert(record_of("cloud-wss")->min_dma_largest == CONNECTION - 1);
    assert(record_of("cloud-wss")->min_internal_largest == 47104);
    /* The heap fragments while the connection's reservation is held: a
     * request whose own block fits is not admitted, because the held
     * reservation's block does not fit any more. It waits for that
     * reservation to end, as for one that takes its bytes. */
    memory.dma_largest = REQUEST;
    assert(!jetkvm_memory_policy_wait_reserve_tls_open_connection("https-media-range", &request));
    assert(delays == JETKVM_MEMORY_TLS_WAIT_MS / JETKVM_MEMORY_TLS_RETRY_MS);
    release_at = &connection; release_at_delay = 1; delays = 0;
    assert(jetkvm_memory_policy_wait_reserve_tls_open_connection("https-media-range", &request));
    assert(delays == 1 && !connection.active);
    jetkvm_memory_policy_release(&request);
    release_at = NULL; release_at_delay = 0;
    memory.dma_largest = 47104;
    memory.internal_largest = SESSION - 1;
    assert(!local_session("webrtc-dtls", &session));
    assert(jetkvm_memory_policy_reserve_tls("cloud-wss", &connection));
    jetkvm_memory_policy_release(&connection);
    memory.internal_largest = SESSION;
    assert(local_session("webrtc-dtls", &session));
    jetkvm_memory_policy_release(&session);
    memory.internal_largest = SESSION_ICE - 1;
    assert(!cloud_session("webrtc-dtls", &session));
    memory.internal_largest = SESSION_ICE;
    assert(cloud_session("webrtc-dtls", &session));
    jetkvm_memory_policy_release(&session);
    /* A download needs the block of its connection, not one of the size
     * of its whole reservation. */
    reservation_t download = {0};
    memory.internal_largest = memory.dma_largest = CONNECTION - 1;
    assert(!jetkvm_memory_policy_wait_reserve_download("ota-download", &download));
    memory.internal_largest = memory.dma_largest = CONNECTION;
    assert(jetkvm_memory_policy_wait_reserve_download("ota-download", &download));
    jetkvm_memory_policy_release(&download);
}

/* How many are admitted at once, from the free memory that was measured on
 * each board. Each handshake allocates its peak
 * while its reservation is held, so that part counts twice. */
static void admitted_at_once(void) {
#if WIFI
    {   /* Session, cloud socket and MQTT over TLS, at the least free memory
         * that was measured: three handshakes, no fourth. */
        reservation_t a = {0}, b = {0}, c = {0}, d = {0};
        reset(); heap(154600, 123700);
        assert(handshake("ota-download", &a) && handshake("https-media-check", &b));
        assert(handshake("mqtt-test", &c));
        assert(!jetkvm_memory_policy_reserve_tls("cloud-registration-tls", &d));
    }
    {   /* Two handshakes, a read of a mounted image, and a third handshake. */
        reservation_t a = {0}, b = {0}, c = {0}, read = {0};
        reset(); heap(154600, 123700);
        assert(handshake("ota-download", &a) && handshake("cloud-registration-tls", &b));
        assert(jetkvm_memory_policy_wait_reserve_tls_open_connection("https-media-range", &read));
        assert(delays == 0);
        assert(jetkvm_memory_policy_reserve_tls("mqtt-test", &c));
    }
    {   /* A session through the cloud and one handshake, no second. */
        reservation_t session = {0}, a = {0}, b = {0};
        reset(); heap(154600, 123700);
        assert(cloud_session("webrtc-dtls", &session));
        allocate(6336);
        assert(handshake("ota-download", &a));
        assert(!jetkvm_memory_policy_reserve_tls("https-media-check", &b));
    }
    {   /* A firmware download holds its connection and the link's queue.
         * Its connection is open (4,628 B at most): one more handshake. */
        reservation_t download = {0}, a = {0}, b = {0};
        reset(); heap(154600, 123700);
        assert(jetkvm_memory_policy_wait_reserve_download("ota-download", &download));
        allocate(4628);
        assert(handshake("cloud-wss-connect", &a));
        assert(!jetkvm_memory_policy_reserve_tls("mqtt-reconnect", &b));
    }
    {   /* At the download's lowest measured point, with the link's queue
         * full (22,268 B under the free memory before it), a handshake does
         * not fit and a waiting owner waits. */
        reservation_t download = {0}, a = {0};
        reset(); heap(154600, 123700);
        assert(jetkvm_memory_policy_wait_reserve_download("ota-download", &download));
        allocate(22268);
        assert(!jetkvm_memory_policy_reserve_tls("cloud-wss-connect", &a));
        assert(!jetkvm_memory_policy_wait_reserve_tls("https-media-check", &a));
        assert(delays == JETKVM_MEMORY_TLS_WAIT_MS / JETKVM_MEMORY_TLS_RETRY_MS);
        /* What the queue's reservation is for: with the queue full and the
         * download at its peak, free memory is above the admission floor. */
        assert(memory.dma_free >= DMA_FLOOR + CONN);
    }
#else
    {   /* Session, cloud socket and MQTT over TLS, at the least free memory
         * that was measured: two handshakes, and no third reservation. */
        reservation_t a = {0}, b = {0}, c = {0}, session = {0};
        reset(); heap(129400, 90400);
        assert(handshake("ota-download", &a) && handshake("cloud-registration-tls", &b));
        assert(!jetkvm_memory_policy_reserve_tls("mqtt-test", &c));
        assert(!local_session("webrtc-dtls", &session));
        /* An owner that can wait waits for the handshakes to end. */
        assert(!jetkvm_memory_policy_wait_reserve_tls("https-media-check", &c));
        assert(delays == JETKVM_MEMORY_TLS_WAIT_MS / JETKVM_MEMORY_TLS_RETRY_MS);
    }
    {   /* At the most: a read of a mounted image beside the two. */
        reservation_t a = {0}, b = {0}, c = {0}, read = {0};
        reset(); heap(133400, 94300);
        assert(handshake("ota-download", &a) && handshake("cloud-registration-tls", &b));
        assert(jetkvm_memory_policy_wait_reserve_tls_open_connection("https-media-range", &read));
        assert(delays == 0);
        assert(!jetkvm_memory_policy_reserve_tls("mqtt-test", &c));
    }
    {   /* A session's open and one handshake. */
        reservation_t session = {0}, a = {0}, b = {0};
        reset(); heap(129400, 90400);
        assert(local_session("webrtc-dtls", &session));
        allocate(6336);
        assert(handshake("cloud-session-oidc", &a));
        assert(!jetkvm_memory_policy_reserve_tls("ota-download", &b));
    }
    {   /* A session through the cloud has 16 KiB: no handshake fits beside
         * its open. */
        reservation_t session = {0}, a = {0};
        reset(); heap(129400, 90400);
        assert(cloud_session("webrtc-dtls", &session));
        allocate(6336);
        assert(!jetkvm_memory_policy_reserve_tls("ota-download", &a));
    }
    {   /* 13 s after a restart with MQTT over TLS: the registration beside
         * the MQTT reconnect. */
        reservation_t a = {0}, b = {0};
        reset(); heap(127439, 88359);
        assert(jetkvm_memory_policy_reserve_tls("mqtt-reconnect", &a));
        assert(jetkvm_memory_policy_reserve_tls("cloud-registration-tls", &b));
        assert(record_of("cloud-registration-tls")->last_dma_headroom == 88359 - 2 * 8192 - 65536);
        assert(record_of("cloud-registration-tls")->denied == 0);
    }
    {   /* A session alone: three handshakes at the most free memory that
         * was measured, two at the least. */
        reservation_t a = {0}, b = {0}, c = {0};
        reset(); heap(141000, 101900);
        assert(handshake("ota-download", &a) && handshake("https-media-check", &b));
        assert(handshake("mqtt-test", &c));
    }
    {
        reservation_t a = {0}, b = {0}, c = {0};
        reset(); heap(138500, 99400);
        assert(handshake("ota-download", &a) && handshake("https-media-check", &b));
        assert(!jetkvm_memory_policy_reserve_tls("mqtt-test", &c));
    }
    {   /* A firmware download holds a connection's reservation and no more.
         * One handshake fits during it: the connect of an HTTPS image, or
         * the cloud socket's reconnect. Two at the same time do not. */
        reservation_t download = {0}, a = {0}, b = {0};
        reset(); heap(129400, 90400);
        assert(jetkvm_memory_policy_wait_reserve_download("ota-download", &download));
        expect_shape(&download, CONNECTION, CONNECTION);
        allocate(4496);
        assert(handshake("https-media-connect", &a));
        assert(!jetkvm_memory_policy_reserve_tls("cloud-wss-connect", &b));
        /* The mounted image keeps 3,060 B of the handshake's 5,004 B. */
        jetkvm_memory_policy_release(&a);
        give_back(HANDSHAKE_PEAK - 3060);
        assert(jetkvm_memory_policy_reserve_tls("cloud-wss-connect", &b));
    }
#endif
    reset();
}

/* An owner that waits is admitted when another owner's reservation ends, and
 * is refused at once when free memory alone is short. */
static void waiting(void) {
    reservation_t a = {0}, b = {0}, waiter = {0};
    /* Room for two connections, and two hold it; a third waits. */
    reset(); heap(INTERNAL_FLOOR + 2 * CONN, DMA_FLOOR + 2 * CONN);
    assert(jetkvm_memory_policy_reserve_tls("osvc-update", &a));
    assert(jetkvm_memory_policy_reserve_tls("ota-download", &b));
    /* The tries while it waits are no denials. */
    release_at = &a; release_at_delay = 3;
    assert(jetkvm_memory_policy_wait_reserve_tls("https-media-check", &waiter) && waiter.active);
    assert(delays == 3 && !a.active && s_admission_denials == 0);
    assert(record_of("https-media-check")->admitted == 1 && record_of("https-media-check")->denied == 0);
    jetkvm_memory_policy_release(&waiter);
    /* A read on an open connection waits in the same way. */
    assert(jetkvm_memory_policy_reserve_tls("osvc-update", &a));
    release_at = &a; release_at_delay = 2; delays = 0;
    assert(jetkvm_memory_policy_wait_reserve_tls_open_connection("https-media-range", &waiter));
    assert(delays == 2 && !a.active && s_admission_denials == 0);
    jetkvm_memory_policy_release(&waiter);
    /* So does a download, which needs the link's queue too. */
    memory.internal_free += QUEUE; memory.dma_free += QUEUE;
    assert(jetkvm_memory_policy_reserve_tls("osvc-update", &a));
    memory.internal_free -= TLS_LINK; memory.dma_free -= TLS_LINK;
    release_at = &a; release_at_delay = 4; delays = 0;
    assert(jetkvm_memory_policy_wait_reserve_download("storage-download", &waiter));
    assert(delays == 4 && !a.active && s_admission_denials == 0);
    jetkvm_memory_policy_release(&waiter);
    memory.internal_free -= QUEUE - TLS_LINK; memory.dma_free -= QUEUE - TLS_LINK;
    /* Reservations that outlast the wait: one denial, after the whole wait. */
    assert(jetkvm_memory_policy_reserve_tls("osvc-update", &a));
    release_at = NULL; release_at_delay = 0; delays = 0;
    assert(!jetkvm_memory_policy_wait_reserve_tls("https-media-check", &waiter) && !waiter.active);
    assert(delays == JETKVM_MEMORY_TLS_WAIT_MS / JETKVM_MEMORY_TLS_RETRY_MS);
    assert(s_admission_denials == 1 && record_of("https-media-check")->denied == 1);
    /* An owner that does not wait is refused at once beside them. */
    reservation_t refused = {0};
    delays = 0;
    assert(!jetkvm_memory_policy_reserve_tls("cloud-registration-tls", &refused));
    assert(!local_session("webrtc-dtls", &refused));
    assert(delays == 0 && s_admission_denials == 3);
    /* Free memory alone is short: refused at once, without a wait. */
    reset(); heap(INTERNAL_FLOOR + CONN, DMA_FLOOR + CONN - 1);
    reservation_t alone = {0};
    assert(!jetkvm_memory_policy_wait_reserve_tls("https-media-check", &alone));
    assert(delays == 0 && s_admission_denials == 1);
    assert(record_of("https-media-check")->denied == 1);
    heap(INTERNAL_FLOOR + CONN, DMA_FLOOR + REQ - 1);
    assert(!jetkvm_memory_policy_wait_reserve_tls_open_connection("https-media-range", &alone));
    assert(delays == 0);
}

/* What the policy reports of a hold, per owner. */
static void held_values(void) {
    reservation_t ota = {0}, other = {0};
    jetkvm_memory_policy_tls_admission_t copy[JETKVM_MEMORY_TLS_ADMISSION_OWNERS];
    /* The timer runs while a TLS reservation is held, and for no other. */
    reset(); heap(129400, 90400);
    jetkvm_memory_policy_request_t psram = {.psram_bytes=1024};
    assert(jetkvm_memory_policy_reserve("psram", &psram, &other) && !timer_runs);
    jetkvm_memory_policy_release(&other);
    assert(jetkvm_memory_policy_reserve_tls("ota-download", &ota) && timer_runs);
    const jetkvm_memory_policy_tls_admission_t *record = record_of("ota-download");
    assert(record->held_internal_min == 129400 && record->held_dma_min == 90400);
    assert(record->held_internal_fall_max == 0 && record->held_dma_fall_max == 0);
    assert(jetkvm_memory_policy_get_tls_admissions(copy, JETKVM_MEMORY_TLS_ADMISSION_OWNERS) == 1);
    assert(copy[0].reserved_internal == CONN && copy[0].reserved_dma == CONN);
    /* The handshake takes 4,812 B, which the timer reads, and gives 1,364 B
     * back before its end: the hold's values are those of the peak. */
    allocate(4812); timer_reads(); give_back(1364);
    jetkvm_memory_policy_release(&ota);
    assert(!timer_runs);
    assert(record->held_internal_min == 129400 - 4812 && record->held_dma_min == 90400 - 4812);
    assert(record->held_internal_fall_max == 4812 && record->held_dma_fall_max == 4812);
    assert(record->held_under_floor == 0);
    assert(jetkvm_memory_policy_get_tls_admissions(copy, JETKVM_MEMORY_TLS_ADMISSION_OWNERS) == 1);
    assert(copy[0].reserved_internal == 0 && copy[0].reserved_dma == 0);
    /* A peak that falls between two readings is not seen: the hold has the
     * values of its release. */
    reset(); heap(129400, 90400);
    assert(jetkvm_memory_policy_reserve_tls("ota-download", &ota));
    allocate(4812); give_back(1364);
    jetkvm_memory_policy_release(&ota);
    assert(record_of("ota-download")->held_dma_fall_max == 3448);

    /* A reading under the admission floor of 65,536 B, although free memory
     * is back at the release: one hold under the floor, however many
     * readings it has. */
    reset(); heap(129400, 90400);
    assert(jetkvm_memory_policy_reserve_tls("ota-download", &ota));
    record = record_of("ota-download");
    allocate(30000); timer_reads(); timer_reads(); give_back(30000); timer_reads();
    jetkvm_memory_policy_release(&ota);
    assert(record->held_internal_min == 99400 && record->held_dma_min == 60400);
    assert(record->held_internal_fall_max == 30000 && record->held_dma_fall_max == 30000);
    assert(record->held_under_floor == 1);
    /* The next hold is not charged for it. */
    assert(jetkvm_memory_policy_reserve_tls("ota-download", &ota));
    jetkvm_memory_policy_release(&ota);
    assert(record->held_under_floor == 1 && record->held_dma_fall_max == 30000);

    /* A reading belongs to the reservations that are held when it is made:
     * one from before an owner's admission is not that owner's. */
    reset(); heap(INTERNAL_FLOOR + 60000, DMA_FLOOR + 60000);
    assert(jetkvm_memory_policy_reserve_tls("ota-download", &ota));
    memory.dma_free = DMA_FLOOR - 1; timer_reads();
    memory.dma_free = DMA_FLOOR + CONN + REQ;
    assert(jetkvm_memory_policy_wait_reserve_tls_open_connection("https-media-range", &other));
    assert(record_of("ota-download")->held_dma_min == DMA_FLOOR - 1);
    assert(record_of("ota-download")->held_under_floor == 1);
    assert(record_of("https-media-range")->held_dma_min == DMA_FLOOR + CONN + REQ);
    assert(record_of("https-media-range")->held_under_floor == 0);
    /* Another owner's release is a look at every hold, and one hold under
     * the floor counts once for each holder. */
    memory.dma_free = DMA_FLOOR - 2;
    jetkvm_memory_policy_release(&other);
    assert(timer_runs);
    assert(record_of("https-media-range")->held_dma_min == DMA_FLOOR - 2);
    assert(record_of("https-media-range")->held_under_floor == 1);
    assert(record_of("ota-download")->held_dma_min == DMA_FLOOR - 2);
    assert(record_of("ota-download")->held_under_floor == 1);
    memory.internal_free = INTERNAL_FLOOR - 1;
    jetkvm_memory_policy_release(&ota);
    assert(!timer_runs);
    assert(record_of("ota-download")->held_under_floor == 1);
    assert(record_of("ota-download")->held_internal_min == INTERNAL_FLOOR - 1);

    /* A callback of the timer that runs after the last release, as one that
     * was on its way when the timer was stopped, and a low reading then: the
     * next hold is not charged for it. */
    reset(); heap(129400, 90400);
    assert(jetkvm_memory_policy_reserve_tls("ota-download", &ota));
    jetkvm_memory_policy_release(&ota);
    memory.dma_free = DMA_FLOOR - 1;
    watch_sample(NULL);
    memory.dma_free = 90400;
    assert(jetkvm_memory_policy_reserve_tls("mqtt-test", &ota));
    assert(record_of("mqtt-test")->held_dma_min == 90400 && record_of("mqtt-test")->held_under_floor == 0);
    assert(record_of("ota-download")->held_dma_min == 90400 && record_of("ota-download")->held_under_floor == 0);
    /* A callback that finds the mutex taken does not wait and notes nothing. */
    memory.dma_free = DMA_FLOOR - 1;
    held = true; watch_sample(NULL); held = false;
    memory.dma_free = 90400;
    jetkvm_memory_policy_release(&ota);
    assert(record_of("mqtt-test")->held_dma_min == 90400);

    /* A fall that is larger than the reservation, as after an upload on the
     * Wi-Fi Mini, when a refresh of the OS Services release list was
     * admitted at 82,715 B of free DMA-capable RAM and saw it fall by
     * 21,124 B: the room between the floors (16,384 B) was in use, which
     * counts as a hold under the floor, and the fall is inside the
     * reservation and that room together, which is no overrun. */
    reset(); heap(113599, 82715);
    assert(jetkvm_memory_policy_reserve_tls("osvc-update", &ota));
    allocate(21124); timer_reads();
    assert(record_of("osvc-update")->held_under_floor == 1);
    assert(record_of("osvc-update")->held_overruns == 0);
    /* One byte more than the reservation and the room between the floors
     * of DMA-capable RAM is an overrun, once for the hold. */
    give_back(21124);
    allocate(CONN + 16384); timer_reads();
    assert(record_of("osvc-update")->held_overruns == 0);
    allocate(1); timer_reads(); allocate(100); timer_reads();
    assert(record_of("osvc-update")->held_overruns == 1);
    jetkvm_memory_policy_release(&ota);
    assert(record_of("osvc-update")->held_overruns == 1);

    /* What is reserved during a hold is no overrun of it: a reservation
     * that was held at the admission, and one that is admitted later, may
     * each allocate what they reserved. */
    reset(); heap(INTERNAL_FLOOR + 120000, DMA_FLOOR + 120000);
    assert(local_session("webrtc-dtls", &other));
    assert(jetkvm_memory_policy_reserve_tls("ota-download", &ota));
    reservation_t upload = {0};
    jetkvm_memory_policy_request_t setup = {
        .internal_bytes=52224, .internal_largest_block=1024, .dma_bytes=52224, .dma_largest_block=1024,
    };
    assert(jetkvm_memory_policy_reserve("http-storage-upload", &setup, &upload));
    allocate(SESS + CONN + 52224 + 16384); timer_reads();
    jetkvm_memory_policy_release(&upload);
    timer_reads();
    assert(record_of("ota-download")->held_overruns == 0);
    assert(record_of("ota-download")->held_dma_fall_max == SESS + CONN + 52224 + 16384);
    /* The session's hold began before the connection's: the connection's
     * and the upload's reservations were admitted during it. */
    assert(record_of("webrtc-dtls")->held_overruns == 0);
    allocate(1); timer_reads();
    assert(record_of("ota-download")->held_overruns == 1);
    assert(record_of("webrtc-dtls")->held_overruns == 1);
    jetkvm_memory_policy_release(&ota);
    jetkvm_memory_policy_release(&other);
    /* The room between the floors of internal RAM is 32,768 B. */
    reset(); heap(INTERNAL_FLOOR + 120000, DMA_FLOOR + 120000);
    assert(jetkvm_memory_policy_reserve_tls("ota-download", &ota));
    memory.internal_free -= CONN + 32768; timer_reads();
    assert(record_of("ota-download")->held_overruns == 0);
    memory.internal_free -= 1; timer_reads();
    assert(record_of("ota-download")->held_overruns == 1);
    jetkvm_memory_policy_release(&ota);

    /* An owner that was only refused has held nothing. */
    reset(); heap(INTERNAL_FLOOR, DMA_FLOOR);
    assert(!jetkvm_memory_policy_reserve_tls("public-ip", &ota));
    assert(record_of("public-ip")->held_internal_min == UINT32_MAX);
    assert(record_of("public-ip")->held_dma_min == UINT32_MAX);
    assert(!timer_runs);
}

/* TLS reservations beside the others. */
static void beside_other_reservations(void) {
    reset();
    /* The peer's PSRAM and its ingress share stay reserved beside TLS. */
    jetkvm_memory_policy_request_t peer = {
        .psram_bytes=1835008, .psram_largest_block=1048576, .storage_ingress_bytes=262144,
    };
    reservation_t peer_memory = {0}, tls = {0}, other = {0};
    assert(jetkvm_memory_policy_reserve("webrtc-peer", &peer, &peer_memory));
    assert(!slot_of(&peer_memory)->tls && slot_of(&peer_memory)->record == NULL);
    assert(local_session("webrtc-dtls", &tls));
    assert(active_slots() == 2);
    jetkvm_memory_policy_release(&tls);
    /* A video rebuild refuses PSRAM and no TLS reservation. */
    s_video_rebuild.active = true;
    jetkvm_memory_policy_request_t psram = {.psram_bytes=1024};
    assert(!jetkvm_memory_policy_reserve("psram", &psram, &other));
    assert(s_video_rebuild_blocked_materializations == 1);
    assert(jetkvm_memory_policy_reserve_tls("ota-download", &tls));
    jetkvm_memory_policy_release(&tls);
    s_video_rebuild.active = false;
    jetkvm_memory_policy_release(&peer_memory);
#if !WIFI
    /* An upload's setup of 52,224 B beside a connection:
     * one connection is admitted, a second is not. */
    heap(170463, 131347);
    memory.internal_largest = memory.dma_largest = 65536;
    jetkvm_memory_policy_request_t upload = {
        .internal_bytes=52224, .internal_largest_block=49152, .dma_bytes=52224, .dma_largest_block=49152,
    };
    assert(jetkvm_memory_policy_reserve("http-storage-upload", &upload, &other));
    assert(jetkvm_memory_policy_reserve_tls("ota-download", &tls));
    assert(record_of("ota-download")->last_dma_headroom == 131347 - 52224 - 8192 - 65536);
    reservation_t second = {0};
    assert(!jetkvm_memory_policy_reserve_tls("cloud-wss-connect", &second));
    jetkvm_memory_policy_release(&tls);
    jetkvm_memory_policy_release(&other);
#endif
    /* Without the mutex or without a slot nothing is reserved. */
    deny_mutex = true;
    assert(!jetkvm_memory_policy_reserve_tls("ota-download", &tls));
    deny_mutex = false;
    for (size_t i = 0; i < JETKVM_MEMORY_RESERVATION_SLOTS; i++) s_reservations[i].active = true;
    assert(!jetkvm_memory_policy_reserve_tls("ota-download", &tls));
}

int main(void) {
    sizes_and_floors();
    largest_block();
    admitted_at_once();
    waiting();
    held_values();
    beside_other_reservations();
    reset();
    printf("TLS admission on the %s board: sizes, floors, largest block, count at once, waits, held values: PASS\n",
           WIFI ? "Wi-Fi" : "Ethernet");
}
"""
with tempfile.TemporaryDirectory(prefix='p4-tls-admission-') as temporary:
    directory = Path(temporary)
    source = directory / 'test.c'
    source.write_text('\n'.join([prefix, *macros, *types, slot, mock, logic, cases]))
    for board, defines in BOARDS.items():
        executable = directory / board
        subprocess.run(['cc', '-std=c11', '-Wall', '-Wextra', '-Werror',
                        '-fsanitize=address,undefined', '-fno-omit-frame-pointer',
                        *defines, str(source), '-o', str(executable)], check=True)
        subprocess.run([str(executable)], check=True)
