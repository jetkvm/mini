#!/usr/bin/env python3
"""Run the production OS Services guest display handover against models.

1. The capture task's request mailbox (jetkvm_video_present_guest and
   friends, guest_display_service, p4_host_display_should_advertise) with a
   model of the bridge's EDID RAM and of the WebRTC transport mode. Guest
   EDIDs come from the real os_services_edid.c builder.
2. tc_program_edid_bytes with a model of the TC358743 registers.
3. Where the capture loop and the encoder consult the mailbox.
"""

from pathlib import Path
import subprocess
import tempfile

component = Path(__file__).resolve().parents[1]
components = component.parent
video = (component / "jetkvm_video.c").read_text()
bridge = (components / "jetkvm_hdmi_bridge" / "jetkvm_hdmi_bridge.c").read_text()
bridge_header = (components / "jetkvm_hdmi_bridge" / "include" /
                 "jetkvm_hdmi_bridge.h").read_text()


def function(source, signature):
    start = source.index(signature)
    pos = source.index("{", start) + 1
    depth = 1
    while depth:
        depth += (source[pos] == "{") - (source[pos] == "}")
        pos += 1
    return source[start:pos]


# The guest request is served right after the cable poll and before every
# branch that could touch the bridge, the capture or the transport.
loop = function(video, "static void hdmi_capture_task(void *arg)")
loop = loop[loop.index("while (1) {"):]
service = loop.index("if (guest_display_service(&ctx)) {")
assert loop.index("video_update_hdmi_connection(") < service
for later in ("requested_preview != ctx.preview_only_active",
              "p4_host_display_sync(&ctx",
              "p4_video_sleep_sync(&ctx",
              "p4_edid_control_process(&ctx.bridge)",
              "video_detect_timings(&ctx.bridge, &detected)"):
    assert service < loop.index(later), later
assert "continue;" in function(loop[service:], "if (guest_display_service(&ctx))")

# The encoder drops a frame before reserving output or taking a sync IDR.
encode = function(video, "static void h264_pipeline_encode_one(")
gate = encode.index("jetkvm_video_guest_owns_display()")
assert gate < encode.index("webrtc_transport_reserve_access_unit(")
assert gate < encode.index("h264_sync_idr_arm(ctx)")

mailbox = video[video.index("typedef enum {\n    GUEST_DISPLAY_REQUEST_NONE"):
                video.index("static bool p4_host_display_should_advertise(void)")]
assert "webrtc_transport_" not in mailbox

common = r'''
#include <assert.h>
#include <inttypes.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "os_services_edid.h"
typedef int esp_err_t;
#define ESP_OK 0
#define ESP_ERR_INVALID_ARG 0x102
#define ESP_ERR_TIMEOUT 0x107
#define ESP_ERR_INVALID_RESPONSE 0x108
#define ESP_ERR_INVALID_CRC 0x109
#define ESP_LOGI(tag, ...) do { (void)(tag); if (0) printf(__VA_ARGS__); } while (0)
#define ESP_LOGE ESP_LOGI
static const char *esp_err_to_name(esp_err_t error) { (void)error; return "error"; }
#define pdMS_TO_TICKS(ms) (ms)
static unsigned s_delay_ms;
static void vTaskDelay(unsigned ms);
typedef enum {
    JETKVM_VIDEO_EDID_PRESET_DEFAULT = 0,
    JETKVM_VIDEO_EDID_PRESET_DELL_D2721H = 1,
} jetkvm_video_edid_preset_t;
typedef struct {
    jetkvm_video_edid_preset_t preset;
    const uint8_t *bytes;
    size_t size;
    const char *label;
} p4_edid_descriptor_t;
'''

mailbox_preamble = common + r'''
#define TAG "test"
#define ESP_ERROR_CHECK(x) do { esp_err_t err_ = (x); assert(err_ == ESP_OK); } while (0)
#define EXT_RAM_BSS_ATTR
typedef struct { int locked; } portMUX_TYPE;
#define portMUX_INITIALIZER_UNLOCKED {0}
static int s_critical;
#define portENTER_CRITICAL(mux) do { assert(!(mux)->locked); (mux)->locked = 1; ++s_critical; } while (0)
#define portEXIT_CRITICAL(mux) do { assert((mux)->locked); (mux)->locked = 0; --s_critical; } while (0)
static void vTaskDelay(unsigned ms) { assert(s_critical == 0); s_delay_ms += ms; }

#define ESP_ERR_INVALID_STATE 0x103
#define VIDEO_POOL_CAPTURE_SLOTS 6
#define VIDEO_POOL_CAPTURE_SLOT_BYTES 3110400U
static uint8_t s_pool_bytes[VIDEO_POOL_CAPTURE_SLOTS][64];
static struct { void *capture[VIDEO_POOL_CAPTURE_SLOTS]; } s_video_pool;
esp_err_t jetkvm_video_lend_guest_buffers(uint8_t *buffers[], size_t count, size_t *bytes);
void jetkvm_video_return_guest_buffers(void);

typedef struct { int unused; } jetkvm_hdmi_bridge_t;
typedef struct {
    jetkvm_hdmi_bridge_t bridge;
    bool cam;
} app_ctx_t;
static app_ctx_t s_ctx = {.cam = true};
static volatile bool s_capture_suspended;
static volatile bool s_host_display_idle_enabled;
#define HOST_DISPLAY_IDLE_GRACE_US (INT64_C(1800) * 1000)
typedef struct { bool browser_session_active; } webrtc_transport_stats_t;
static int64_t s_now_us = 1000000;
static int64_t esp_timer_get_time(void) { return s_now_us; }
static esp_err_t webrtc_transport_get_stats(webrtc_transport_stats_t *stats)
{
    stats->browser_session_active = false;
    return ESP_OK;
}
bool jetkvm_video_guest_owns_display(void);
bool jetkvm_video_guest_display_presented(void);
esp_err_t jetkvm_video_present_guest(const uint8_t edid[256]);
esp_err_t jetkvm_video_restore_physical(void);

/* Bridge model: the EDID the computer would read. */
static uint8_t s_ram[256];
static uint8_t s_preset_bytes[2][256];
static const p4_edid_descriptor_t s_presets[2] = {
    {JETKVM_VIDEO_EDID_PRESET_DEFAULT, s_preset_bytes[0], 256, "default"},
    {JETKVM_VIDEO_EDID_PRESET_DELL_D2721H, s_preset_bytes[1], 256, "dell"},
};
static jetkvm_video_edid_preset_t s_applied = JETKVM_VIDEO_EDID_PRESET_DELL_D2721H;
static unsigned s_mutes, s_destroys, s_guest_writes, s_preset_writes;
static esp_err_t s_guest_error, s_preset_error, s_destroy_error;
/* Runs in the middle of a 2 s EDID write, as another task would. */
static void (*s_during_write)(void);
/* Transport model: OS Services streams once presented; csi_destroy clears
 * whatever mode is set. */
enum { MODE_NONE, MODE_PHYSICAL, MODE_GUEST };
static int s_mode = MODE_PHYSICAL;
static unsigned s_guest_mode_cleared;

static const p4_edid_descriptor_t *p4_edid_descriptor(jetkvm_video_edid_preset_t preset)
{
    return &s_presets[preset];
}
static jetkvm_video_edid_preset_t p4_edid_control_applied_preset(void) { return s_applied; }
static esp_err_t tc_enable_stream(jetkvm_hdmi_bridge_t *bridge, bool enable)
{
    (void)bridge;
    assert(s_critical == 0 && !enable);
    ++s_mutes;
    return ESP_OK;
}
static esp_err_t csi_destroy(app_ctx_t *ctx)
{
    assert(s_critical == 0 && s_mutes > 0);
    ++s_destroys;
    ctx->cam = false;
    s_guest_mode_cleared += s_mode == MODE_GUEST;
    s_mode = MODE_NONE;
    return s_destroy_error;
}
static esp_err_t write_ram(const uint8_t *bytes, esp_err_t error)
{
    assert(s_critical == 0);
    if (s_during_write) {
        void (*during)(void) = s_during_write;
        s_during_write = NULL;
        during();
    }
    /* A failed write leaves part of the new EDID behind. */
    memcpy(s_ram, bytes, error == ESP_OK ? 256 : 100);
    return error;
}
static esp_err_t tc_program_edid_bytes(jetkvm_hdmi_bridge_t *bridge, const uint8_t edid[256])
{
    (void)bridge;
    /* Never a guest EDID while the physical capture or its stream lives. */
    assert(!s_ctx.cam && s_mode != MODE_PHYSICAL);
    ++s_guest_writes;
    return write_ram(edid, s_guest_error);
}
static esp_err_t tc_program_edid(jetkvm_hdmi_bridge_t *bridge, const p4_edid_descriptor_t *descriptor)
{
    (void)bridge;
    ++s_preset_writes;
    return write_ram(descriptor->bytes, s_preset_error);
}
'''

mailbox_checks = r'''
static void reset_counts(void)
{
    s_mutes = s_destroys = s_guest_writes = s_preset_writes = s_delay_ms = 0;
}

static bool service(void)
{
    const bool held = guest_display_service(&s_ctx);
    /* The OS Services task starts its guest stream once presented. */
    if (jetkvm_video_guest_display_presented()) s_mode = MODE_GUEST;
    /* Not owned: the computer reads the applied preset. */
    if (!jetkvm_video_guest_owns_display()) {
        assert(memcmp(s_ram, s_preset_bytes[s_applied], 256) == 0);
        assert(!jetkvm_video_guest_display_presented());
    }
    return held;
}

/* OS Services clears its guest stream, then asks for the restore. */
static esp_err_t os_services_restore(void)
{
    s_mode = MODE_NONE;
    return jetkvm_video_restore_physical();
}

/* The capture loop's rebuild after a restore. */
static void rebuild_physical_capture(void)
{
    assert(!jetkvm_video_guest_owns_display());
    s_ctx.cam = true;
    s_mode = MODE_PHYSICAL;
    reset_counts();
}

static void build(osvc_guest_resolution_t resolution, uint32_t serial, uint8_t out[256])
{
    assert(osvc_edid_build(resolution, serial, out));
}

static uint32_t ram_serial(void)
{
    return (uint32_t)s_ram[12] | ((uint32_t)s_ram[13] << 8) |
           ((uint32_t)s_ram[14] << 16) | ((uint32_t)s_ram[15] << 24);
}

static void test_present_and_restore(void)
{
    uint8_t edid[256];
    build(OSVC_GUEST_RES_1440P60, 0x11111111U, edid);
    assert(!service());
    assert(s_destroys == 0 && s_guest_writes == 0 && s_preset_writes == 0);

    /* Accepted at once; nothing on the bridge yet. */
    assert(jetkvm_video_present_guest(edid) == ESP_OK);
    assert(jetkvm_video_guest_owns_display() && !jetkvm_video_guest_display_presented());
    assert(memcmp(s_ram, s_preset_bytes[s_applied], 256) == 0);

    /* Mute, tear capture down, write the guest EDID, then presented. */
    assert(service());
    assert(s_mutes == 1 && s_destroys == 1 && s_guest_writes == 1 && s_preset_writes == 0);
    assert(memcmp(s_ram, edid, 256) == 0);
    assert(jetkvm_video_guest_display_presented() && s_mode == MODE_GUEST);

    /* Held: nothing more happens. */
    for (int i = 0; i < 5; ++i) assert(service());
    assert(s_destroys == 1 && s_guest_writes == 1 && s_preset_writes == 0);

    assert(os_services_restore() == ESP_OK);
    assert(jetkvm_video_guest_owns_display() && !jetkvm_video_guest_display_presented());
    assert(service());
    assert(s_preset_writes == 1 && s_destroys == 1 && !jetkvm_video_guest_owns_display());
    assert(!service() && s_preset_writes == 1);
    rebuild_physical_capture();

    /* A restore with nothing owned writes nothing. */
    assert(jetkvm_video_restore_physical() == ESP_OK);
    assert(!service() && s_preset_writes == 0 && s_guest_writes == 0);
}

static void test_repeated_requests(void)
{
    uint8_t a[256], b[256], c[256];
    build(OSVC_GUEST_RES_1080P60, 0x20000001U, a);
    build(OSVC_GUEST_RES_720P60, 0x20000002U, b);
    build(OSVC_GUEST_RES_1440P60, 0x20000003U, c);

    /* Two presents before the task runs: one write, of the newer. */
    assert(jetkvm_video_present_guest(a) == ESP_OK);
    assert(jetkvm_video_present_guest(b) == ESP_OK);
    assert(service());
    assert(s_guest_writes == 1 && s_destroys == 1 && ram_serial() == 0x20000002U);

    /* Presenting again rewrites the EDID (a new HPD pulse) only. */
    assert(jetkvm_video_present_guest(c) == ESP_OK);
    assert(!jetkvm_video_guest_display_presented());
    assert(service());
    assert(s_guest_writes == 2 && s_destroys == 1 && ram_serial() == 0x20000003U);
    assert(jetkvm_video_guest_display_presented());

    /* Restore then present before the task runs: the newer wins. */
    assert(os_services_restore() == ESP_OK);
    assert(jetkvm_video_present_guest(a) == ESP_OK);
    assert(service());
    assert(s_preset_writes == 0 && s_guest_writes == 3 && ram_serial() == 0x20000001U);

    /* Present then restore, from the guest: the preset goes back. */
    assert(jetkvm_video_present_guest(b) == ESP_OK);
    assert(os_services_restore() == ESP_OK);
    assert(service());
    assert(s_guest_writes == 3 && s_preset_writes == 1);
    rebuild_physical_capture();

    /* And from the physical display: nothing is written, capture stays,
     * and the loop runs on (no stale HPD assumption). */
    assert(jetkvm_video_present_guest(a) == ESP_OK);
    assert(os_services_restore() == ESP_OK);
    assert(!service());
    assert(s_guest_writes == 0 && s_preset_writes == 0 && s_destroys == 0 && s_ctx.cam);
    assert(!jetkvm_video_guest_owns_display());
}

static uint8_t s_late[256];
static void present_late(void) { assert(jetkvm_video_present_guest(s_late) == ESP_OK); }
static void restore_late(void) { assert(os_services_restore() == ESP_OK); }

static void test_requests_during_a_write(void)
{
    uint8_t a[256];
    build(OSVC_GUEST_RES_1080P60, 0x30000001U, a);
    build(OSVC_GUEST_RES_720P60, 0x30000002U, s_late);

    /* A present during the first one's write: the first never reports
     * presented, the second is written next. */
    assert(jetkvm_video_present_guest(a) == ESP_OK);
    s_during_write = present_late;
    assert(service());
    assert(ram_serial() == 0x30000001U && !jetkvm_video_guest_display_presented());
    assert(service());
    assert(ram_serial() == 0x30000002U && jetkvm_video_guest_display_presented());
    assert(s_destroys == 1 && s_guest_writes == 2);

    /* A restore during a restore's write: owned until the second ran. */
    assert(os_services_restore() == ESP_OK);
    s_during_write = restore_late;
    assert(service() && jetkvm_video_guest_owns_display());
    assert(!service() && !jetkvm_video_guest_owns_display() && s_preset_writes == 1);
    rebuild_physical_capture();

    /* A restore during a present's write: not presented, then restored. */
    assert(jetkvm_video_present_guest(a) == ESP_OK);
    s_during_write = restore_late;
    assert(service());
    assert(!jetkvm_video_guest_display_presented() && jetkvm_video_guest_owns_display());
    assert(service() && !jetkvm_video_guest_owns_display() && s_preset_writes == 1);
    rebuild_physical_capture();
}

static void test_write_failures(void)
{
    uint8_t a[256];
    build(OSVC_GUEST_RES_1440P60, 0x40000001U, a);

    /* The guest EDID fails: the preset goes straight back and ownership
     * ends, which tells OS Services the handover failed. */
    s_guest_error = ESP_ERR_TIMEOUT;
    assert(jetkvm_video_present_guest(a) == ESP_OK);
    assert(service());
    s_guest_error = ESP_OK;
    assert(s_guest_writes == 1 && s_preset_writes == 1 && !jetkvm_video_guest_owns_display());
    rebuild_physical_capture();

    /* The preset fails too: still owned, retried once a second. */
    s_guest_error = s_preset_error = ESP_ERR_TIMEOUT;
    assert(jetkvm_video_present_guest(a) == ESP_OK);
    assert(service());
    s_guest_error = ESP_OK;
    assert(service() && service());
    assert(s_preset_writes == 3 && s_delay_ms == 3000 && jetkvm_video_guest_owns_display());
    s_preset_error = ESP_OK;
    assert(service() && !jetkvm_video_guest_owns_display() && s_preset_writes == 4);
    assert(!service());
    rebuild_physical_capture();

    /* A present while retrying is written at once, capture already gone. */
    s_guest_error = s_preset_error = ESP_ERR_TIMEOUT;
    assert(jetkvm_video_present_guest(a) == ESP_OK);
    assert(service());
    s_guest_error = s_preset_error = ESP_OK;
    assert(jetkvm_video_present_guest(a) == ESP_OK);
    assert(service());
    assert(s_destroys == 1 && jetkvm_video_guest_display_presented());
    assert(os_services_restore() == ESP_OK);
    assert(service());
    rebuild_physical_capture();
}

static void test_capture_teardown_failure(void)
{
    uint8_t a[256];
    build(OSVC_GUEST_RES_1080P60, 0x90000001U, a);
    /* csi_destroy fails, as when the 3 s WebRTC mode drain times out: the
     * handover fails with the preset left in place, and nothing aborts. */
    s_destroy_error = ESP_ERR_TIMEOUT;
    assert(jetkvm_video_present_guest(a) == ESP_OK);
    assert(service());
    s_destroy_error = ESP_OK;
    assert(s_destroys == 1 && s_guest_writes == 0 && s_preset_writes == 0);
    assert(!jetkvm_video_guest_owns_display());
    assert(!service());
    /* After the capture loop's rebuild the next present goes through. */
    rebuild_physical_capture();
    assert(jetkvm_video_present_guest(a) == ESP_OK);
    assert(service() && jetkvm_video_guest_display_presented());
    assert(os_services_restore() == ESP_OK);
    assert(service());
    rebuild_physical_capture();
}

static void test_ota_suspension_defers_requests(void)
{
    uint8_t a[256];
    build(OSVC_GUEST_RES_1080P60, 0x50000001U, a);
    s_capture_suspended = true;
    assert(jetkvm_video_present_guest(a) == ESP_OK);
    assert(!service() && s_mutes == 0 && s_guest_writes == 0);
    s_capture_suspended = false;
    assert(service() && jetkvm_video_guest_display_presented());

    s_capture_suspended = true;
    assert(os_services_restore() == ESP_OK);
    assert(service() && s_preset_writes == 0 && jetkvm_video_guest_owns_display());
    s_capture_suspended = false;
    assert(service() && !jetkvm_video_guest_owns_display());
    rebuild_physical_capture();
}

static void test_restore_writes_the_applied_preset(void)
{
    uint8_t a[256];
    build(OSVC_GUEST_RES_720P60, 0x60000001U, a);
    /* The EDID mailbox applied the other preset earlier. */
    s_applied = JETKVM_VIDEO_EDID_PRESET_DEFAULT;
    memcpy(s_ram, s_preset_bytes[0], 256);
    assert(jetkvm_video_present_guest(a) == ESP_OK);
    assert(service());
    assert(os_services_restore() == ESP_OK);
    assert(service() && memcmp(s_ram, s_preset_bytes[0], 256) == 0);
    rebuild_physical_capture();
}

static void test_rejected_edids(void)
{
    uint8_t a[256];
    build(OSVC_GUEST_RES_1080P60, 0x70000001U, a);
    assert(jetkvm_video_present_guest(NULL) == ESP_ERR_INVALID_ARG);
    a[0] = 0x01;
    assert(jetkvm_video_present_guest(a) == ESP_ERR_INVALID_ARG);
    a[0] = 0x00;
    a[200] ^= 0x40;
    assert(jetkvm_video_present_guest(a) == ESP_ERR_INVALID_CRC);
    a[200] ^= 0x40;
    a[30] ^= 0x01;
    assert(jetkvm_video_present_guest(a) == ESP_ERR_INVALID_CRC);
    assert(!jetkvm_video_guest_owns_display());
    assert(!service() && s_guest_writes == 0 && s_destroys == 0);
}

static void test_host_display_stays_advertised(void)
{
    uint8_t a[256];
    build(OSVC_GUEST_RES_1080P60, 0x80000001U, a);
    s_host_display_idle_enabled = true;
    assert(p4_host_display_should_advertise());   /* idle grace starts */
    s_now_us += HOST_DISPLAY_IDLE_GRACE_US + 1;
    assert(!p4_host_display_should_advertise());  /* idle: hide */

    assert(jetkvm_video_present_guest(a) == ESP_OK);
    assert(p4_host_display_should_advertise());
    assert(service());
    s_now_us += 10 * HOST_DISPLAY_IDLE_GRACE_US;
    assert(p4_host_display_should_advertise());

    /* After the restore the grace starts over. */
    assert(os_services_restore() == ESP_OK);
    assert(service() && p4_host_display_should_advertise());
    s_now_us += HOST_DISPLAY_IDLE_GRACE_US - 1;
    assert(p4_host_display_should_advertise());
    s_now_us += 2;
    assert(!p4_host_display_should_advertise());
    s_host_display_idle_enabled = false;
    rebuild_physical_capture();
}

/* Service video borrows capture buffers only while the guest display is
 * presented, and hardware capture stays down until they are back, also
 * after the restore. */
static void test_lent_buffers_hold_capture(void)
{
    uint8_t a[256];
    uint8_t *buffers[2] = {NULL, NULL};
    size_t bytes = 0U;
    build(OSVC_GUEST_RES_2160P60, 0xA0000001U, a);
    assert(jetkvm_video_lend_guest_buffers(buffers, 2U, &bytes) == ESP_ERR_INVALID_STATE);
    assert(jetkvm_video_present_guest(a) == ESP_OK);
    assert(jetkvm_video_lend_guest_buffers(buffers, 2U, &bytes) == ESP_ERR_INVALID_STATE);
    assert(service() && jetkvm_video_guest_display_presented());
    assert(jetkvm_video_lend_guest_buffers(buffers, VIDEO_POOL_CAPTURE_SLOTS + 1U,
                                           &bytes) == ESP_ERR_INVALID_ARG);
    assert(jetkvm_video_lend_guest_buffers(buffers, 2U, &bytes) == ESP_OK);
    assert(buffers[0] == s_video_pool.capture[0] && buffers[1] == s_video_pool.capture[1]);
    assert(bytes == VIDEO_POOL_CAPTURE_SLOT_BYTES);
    assert(jetkvm_video_lend_guest_buffers(buffers, 2U, &bytes) == ESP_ERR_INVALID_STATE);

    /* A new guest EDID keeps the loan; capture is down either way. */
    assert(jetkvm_video_present_guest(a) == ESP_OK);
    assert(service() && s_destroys == 1);

    /* Restored, but the buffers are still out: the loop holds capture. */
    assert(os_services_restore() == ESP_OK);
    assert(service() && !jetkvm_video_guest_owns_display() && s_preset_writes == 1);
    for (int i = 0; i < 3; ++i) assert(service());
    jetkvm_video_return_guest_buffers();
    assert(!service());
    rebuild_physical_capture();
}

int main(void)
{
    for (int i = 0; i < VIDEO_POOL_CAPTURE_SLOTS; ++i) s_video_pool.capture[i] = s_pool_bytes[i];
    for (int i = 0; i < 512; ++i) s_preset_bytes[i / 256][i % 256] = (uint8_t)(i * 7 + i / 256);
    memcpy(s_ram, s_preset_bytes[s_applied], 256);
    test_present_and_restore();
    test_repeated_requests();
    test_requests_during_a_write();
    test_write_failures();
    test_capture_teardown_failure();
    test_ota_suspension_defers_requests();
    test_restore_writes_the_applied_preset();
    test_rejected_edids();
    test_host_display_stays_advertised();
    test_lent_buffers_hold_capture();
    assert(s_critical == 0 && s_guest_mode_cleared == 0);
    puts("guest display mailbox: present, restore, coalescing, races, failures, OTA, HPD policy, lent buffers: PASS");
    return 0;
}
'''

mailbox_production = "\n".join([
    mailbox,
    function(video, "static bool p4_host_display_should_advertise(void)"),
    function(video, "esp_err_t jetkvm_video_present_guest("),
    function(video, "esp_err_t jetkvm_video_restore_physical(void)"),
    function(video, "bool jetkvm_video_guest_owns_display(void)"),
    function(video, "bool jetkvm_video_guest_display_presented(void)"),
    function(video, "esp_err_t jetkvm_video_lend_guest_buffers("),
    function(video, "void jetkvm_video_return_guest_buffers(void)"),
])

writer_preamble = common + r'''
#include "tc358743_regs.h"
#define ESP_RETURN_ON_FALSE(cond, err, tag, ...) do { (void)(tag); if (!(cond)) return (err); } while (0)
typedef struct { const char *log_tag; } jetkvm_hdmi_bridge_t;

/* TC358743 model: HPD output, EDID length and RAM. */
static uint32_t s_hpd_ctl = MASK_HPD_OUT0;
static uint32_t s_len1, s_len2;
static uint8_t s_ram[256];
static unsigned s_accesses, s_writes_with_hpd_high, s_low_pulses;
static int s_fail_write_at = -1;

static void vTaskDelay(unsigned ms) { if (!(s_hpd_ctl & MASK_HPD_OUT0)) s_delay_ms += ms; }
static esp_err_t tc_update_bits(jetkvm_hdmi_bridge_t *ctx, uint16_t reg, uint32_t mask, uint32_t value)
{
    (void)ctx;
    ++s_accesses;
    assert(reg == HPD_CTL && mask == (uint32_t)~MASK_HPD_OUT0);
    s_low_pulses += (s_hpd_ctl & MASK_HPD_OUT0) && !(value & MASK_HPD_OUT0);
    s_hpd_ctl = (s_hpd_ctl & mask) | value;
    return ESP_OK;
}
static esp_err_t tc_write_reg(jetkvm_hdmi_bridge_t *ctx, uint16_t reg, int size, uint32_t value)
{
    (void)ctx;
    ++s_accesses;
    assert(size == 1 && (reg == EDID_LEN1 || reg == EDID_LEN2));
    *(reg == EDID_LEN1 ? &s_len1 : &s_len2) = value;
    return ESP_OK;
}
static esp_err_t tc_read_reg(jetkvm_hdmi_bridge_t *ctx, uint16_t reg, int size, uint32_t *value)
{
    (void)ctx;
    ++s_accesses;
    assert(reg == HPD_CTL && size == 1);
    *value = s_hpd_ctl;
    return ESP_OK;
}
static esp_err_t tc_write_bytes(jetkvm_hdmi_bridge_t *ctx, uint16_t reg, const uint8_t *data, size_t len)
{
    (void)ctx;
    ++s_accesses;
    assert(reg >= EDID_RAM && reg + len <= EDID_RAM + 256U);
    s_writes_with_hpd_high += (s_hpd_ctl & MASK_HPD_OUT0) != 0;
    if ((int)(reg - EDID_RAM) == s_fail_write_at) return ESP_ERR_TIMEOUT;
    memcpy(s_ram + (reg - EDID_RAM), data, len);
    return ESP_OK;
}
static esp_err_t tc_read_bytes(jetkvm_hdmi_bridge_t *ctx, uint16_t reg, uint8_t *data, size_t len)
{
    (void)ctx;
    ++s_accesses;
    memcpy(data, s_ram + (reg - EDID_RAM), len);
    return ESP_OK;
}
'''

writer_checks = r'''
int main(void)
{
    jetkvm_hdmi_bridge_t ctx = {"test"};
    uint8_t edid[256];
    assert(osvc_edid_build(OSVC_GUEST_RES_1440P60, 0xCAFEF00DU, edid));

    /* The guest EDID lands byte for byte as two blocks, every RAM write
     * inside one HPD-low pulse of at least 2 s, HPD high at the end. */
    assert(tc_program_edid_bytes(&ctx, edid) == ESP_OK);
    assert(memcmp(s_ram, edid, 256) == 0 && s_len1 == 0x00 && s_len2 == 0x01);
    assert(s_writes_with_hpd_high == 0 && s_low_pulses == 1 && s_delay_ms >= 2000);
    assert(s_hpd_ctl & MASK_HPD_OUT0);

    /* A damaged checksum in either block never reaches the chip. */
    s_accesses = 0;
    for (size_t at = 20; at < 256; at += 180) {
        edid[at] ^= 0x10;
        assert(tc_program_edid_bytes(&ctx, edid) == ESP_ERR_INVALID_CRC);
        edid[at] ^= 0x10;
    }
    assert(s_accesses == 0);

    /* A failed write still ends with HPD high, which the capture loop
     * assumes after every EDID write. */
    s_fail_write_at = 128;
    assert(tc_program_edid_bytes(&ctx, edid) == ESP_ERR_TIMEOUT);
    assert(s_hpd_ctl & MASK_HPD_OUT0);
    puts("tc_program_edid_bytes: guest bytes, HPD pulse, checksums, failure: PASS");
    return 0;
}
'''

writer_production = "\n".join([
    next(line for line in bridge_header.splitlines()
         if line.startswith("#define TC_HPD_LOW_MIN_MS")),
    function(bridge, "esp_err_t tc_program_edid(jetkvm_hdmi_bridge_t *ctx,"),
    function(bridge, "esp_err_t tc_program_edid_bytes("),
])

with tempfile.TemporaryDirectory(prefix="video-guest-display-") as directory:
    path = Path(directory)
    cc = ["cc", "-std=c11", "-Wall", "-Wextra", "-Werror", "-O1", "-g",
          "-fsanitize=address,undefined", "-fno-sanitize-recover=all",
          "-I", str(components / "jetkvm_os_services"),
          "-I", str(components / "jetkvm_hdmi_bridge"),
          str(components / "jetkvm_os_services" / "os_services_edid.c")]
    for name, text in (
            ("mailbox", mailbox_preamble + mailbox_production + mailbox_checks),
            ("writer", writer_preamble + writer_production + writer_checks)):
        (path / f"{name}.c").write_text(text)
        subprocess.run(cc + [str(path / f"{name}.c"), "-o", str(path / name)],
                       check=True)
        subprocess.run([str(path / name)], check=True)
