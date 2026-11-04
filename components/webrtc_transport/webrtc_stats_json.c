/* WebRTC stats snapshot collection and JSON serialization. */
#include "jetkvm_screen.h"
#include "webrtc_stats_json.h"
#include "jetkvm_audio.h"

#include <inttypes.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/lock.h>

#include "esp_app_desc.h"
#include "esp_attr.h"
#include "esp_heap_caps.h"
#include "esp_memory_utils.h"
#include "esp_private/freertos_idf_additions_priv.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/freertos_debug.h"
#include "freertos/task.h"
#include "jetkvm_ethernet_stats.h"
#include "jetkvm_flash_read_mode.h"
#include "jetkvm_hid_input.h"
#include "jetkvm_memory_policy.h"
#include "jetkvm_usb_service.h"
#include "jetkvm_virtual_media.h"
#include "playout_delay.h"
#include "webrtc_transport.h"
#include "webrtc_transport_private.h"

static void add_u64(cJSON *root, const char *name, uint64_t value)
{
    cJSON_AddNumberToObject(root, name, (double)value);
}

/* The sources that hold a stream off, under `name`; empty while its frames
 * are delivered. */
static void add_paused_by(cJSON *root, const char *name, uint8_t sources)
{
    static const struct {
        webrtc_video_pause_source_t source;
        const char *name;
    } names[] = {
        { WEBRTC_VIDEO_PAUSE_UI, "ui" },
        { WEBRTC_VIDEO_PAUSE_OTA, "ota" },
        { WEBRTC_VIDEO_PAUSE_STORAGE_DOWNLOAD, "download" },
        { WEBRTC_VIDEO_PAUSE_STORAGE_UPLOAD, "upload" },
    };
    cJSON *by = cJSON_AddArrayToObject(root, name);
    for (size_t i = 0; by && i < sizeof(names) / sizeof(names[0]); ++i) {
        if (sources & names[i].source) {
            cJSON_AddItemToArray(by, cJSON_CreateString(names[i].name));
        }
    }
}

/* "videoPausedBy" has every source that holds video off, and "audioPausedBy"
 * those of them that also stop the host's sound, so that a client can tell
 * sound that the Mini holds off from sound that is missing. */
static void add_paused_streams(cJSON *root, uint8_t sources)
{
    add_paused_by(root, "videoPausedBy", sources);
    add_paused_by(root, "audioPausedBy", sources & WEBRTC_AUDIO_PAUSE_SOURCES);
}

/* [count, >=10 ms, >=25 ms, >=50 ms, max us] */
static void add_duration_hist(cJSON *parent, const char *name,
                              const webrtc_transport_duration_hist_t *h)
{
    const double values[] = { h->count, h->over_10ms, h->over_25ms, h->over_50ms, h->max_us };
    cJSON_AddItemToObject(parent, name, cJSON_CreateDoubleArray(values, 5));
}

static void add_peer_pass(cJSON *root, const webrtc_transport_peer_pass_t *p)
{
    cJSON *pass = cJSON_AddObjectToObject(root, "peerPass");
    if (!pass) {
        return;
    }
    add_u64(pass, "count", p->count);
    add_u64(pass, "sumUs", p->sum_us);
    add_u64(pass, "over16ms", p->over_16ms);
    cJSON_AddNumberToObject(pass, "maxUs", p->max_us);
}

/* One count per core; null for a core without the idle hook. */
static void add_idle_runs(cJSON *root, const uint64_t *counts, const bool *counted)
{
    cJSON *runs = cJSON_AddArrayToObject(root, "idleRuns");
    for (int core = 0; runs && core < WEBRTC_TRANSPORT_IDLE_CORES; ++core) {
        cJSON_AddItemToArray(runs, counted[core] ? cJSON_CreateNumber((double)counts[core]) :
                                                   cJSON_CreateNull());
    }
}

/* Largest-block and allocated-block counts need an IRQ-masked heap walk.
 * Make cached/unknown detail explicit while free/minimum remain live. */
static void add_heap_detail_source(cJSON *root, const char *name,
                                   bool cached, uint64_t sampled_us)
{
    cJSON *detail = cJSON_AddObjectToObject(root, name);
    if (detail == NULL) return;
    cJSON_AddBoolToObject(detail, "cached", cached);
    cJSON_AddBoolToObject(detail, "available", sampled_us != 0);
    cJSON_AddStringToObject(detail, "scope", "largestBlocksAndBlockCounts");
    if (sampled_us == 0) {
        cJSON_AddNullToObject(detail, "sampledAtUs");
        cJSON_AddNullToObject(detail, "ageMs");
    } else {
        uint64_t now_us = (uint64_t)esp_timer_get_time();
        add_u64(detail, "sampledAtUs", sampled_us);
        add_u64(detail, "ageMs", now_us >= sampled_us ?
                (now_us - sampled_us) / 1000U : 0U);
    }
}

static bool (*s_cloud_link)(webrtc_transport_cloud_link_t *link);

void webrtc_transport_set_cloud_link_source(
    bool (*source)(webrtc_transport_cloud_link_t *link))
{
    s_cloud_link = source;
}

static void add_cloud_link(cJSON *root)
{
    webrtc_transport_cloud_link_t link = {0};
    if (s_cloud_link == NULL || !s_cloud_link(&link)) {
        cJSON_AddNullToObject(root, "cloudConnected");
        cJSON_AddNullToObject(root, "cloudConnectionEpoch");
        cJSON_AddNullToObject(root, "cloudClientRebuilds");
        return;
    }
    cJSON_AddBoolToObject(root, "cloudConnected", link.connected);
    cJSON_AddNumberToObject(root, "cloudConnectionEpoch", link.connection_epoch);
    cJSON_AddNumberToObject(root, "cloudClientRebuilds",
                            (double)link.client_rebuilds);
}

static bool (*s_guest_video)(cJSON *object);

void webrtc_transport_set_guest_video_source(bool (*source)(cJSON *object))
{
    s_guest_video = source;
}

static void add_guest_video(cJSON *root)
{
    cJSON *object = cJSON_CreateObject();
    if (object != NULL && s_guest_video != NULL && s_guest_video(object)) {
        cJSON_AddItemToObject(root, "guestVideo", object);
        return;
    }
    cJSON_Delete(object);
    cJSON_AddNullToObject(root, "guestVideo");
}

static void (*s_video_encode)(webrtc_transport_video_encode_t *encode);

void webrtc_transport_set_video_encode_source(
    void (*source)(webrtc_transport_video_encode_t *encode))
{
    s_video_encode = source;
}

/* "audio": the USB audio from the host and its Opus frames, in the full view
 * and in the compact one, which a poll once per second reads. */
static void add_audio(cJSON *root)
{
    jetkvm_audio_stats_t audio_stats;
    jetkvm_audio_get_stats(&audio_stats);
    cJSON *audio = cJSON_AddObjectToObject(root, "audio");
    if (audio) {
        cJSON_AddBoolToObject(audio, "usbReady", audio_stats.usb_ready);
        cJSON_AddBoolToObject(audio, "streaming", audio_stats.streaming);
        cJSON_AddBoolToObject(audio, "enabled", audio_stats.enabled);
        cJSON_AddNumberToObject(audio, "receivedPackets", audio_stats.received_packets);
        cJSON_AddNumberToObject(audio, "receivedBytes", audio_stats.received_bytes);
        cJSON_AddNumberToObject(audio, "pcmBytes", audio_stats.pcm_bytes);
        cJSON_AddNumberToObject(audio, "encodedFrames", audio_stats.encoded_frames);
        cJSON_AddNumberToObject(audio, "encodeErrors", audio_stats.encode_errors);
        cJSON_AddNumberToObject(audio, "droppedFrames", audio_stats.dropped_frames);
    }
}

static void add_video_encode(cJSON *root)
{
    cJSON *object = s_video_encode != NULL ? cJSON_CreateObject() : NULL;
    if (object == NULL) {
        cJSON_AddNullToObject(root, "videoEncode");
        return;
    }
    webrtc_transport_video_encode_t encode = {0};
    s_video_encode(&encode);
    cJSON_AddNumberToObject(object, "pFrames", encode.p_frames);
    add_u64(object, "pEncodeUs", encode.p_encode_us);
    cJSON_AddNumberToObject(object, "pEncodeMaxUs", encode.p_encode_max_us);
    cJSON_AddNumberToObject(object, "idrFrames", encode.idr_frames);
    add_u64(object, "idrEncodeUs", encode.idr_encode_us);
    cJSON_AddNumberToObject(object, "idrEncodeMaxUs", encode.idr_encode_max_us);
    add_u64(object, "handoffUs", encode.handoff_us);
    cJSON_AddNumberToObject(object, "handoffMaxUs", encode.handoff_max_us);
    cJSON_AddItemToObject(root, "videoEncode", object);
}

static void (*s_hdmi_receiver)(webrtc_transport_hdmi_receiver_t *receiver);

void webrtc_transport_set_hdmi_receiver_source(
    void (*source)(webrtc_transport_hdmi_receiver_t *receiver))
{
    s_hdmi_receiver = source;
}

/* "hdmiReceiver": a part that the capture task has not read yet has null as
 * its age and as the values that would come from it. */
static void add_hdmi_receiver(cJSON *root)
{
    cJSON *object = s_hdmi_receiver != NULL ? cJSON_CreateObject() : NULL;
    if (object == NULL) {
        cJSON_AddNullToObject(root, "hdmiReceiver");
        return;
    }
    webrtc_transport_hdmi_receiver_t receiver = {0};
    s_hdmi_receiver(&receiver);
    if (receiver.status_read) {
        cJSON_AddNumberToObject(object, "sysStatus", receiver.sys_status);
        cJSON_AddBoolToObject(object, "cable5v", receiver.cable_5v);
        cJSON_AddBoolToObject(object, "tmdsClock", receiver.tmds_clock);
        cJSON_AddBoolToObject(object, "phyPll", receiver.phy_pll);
        cJSON_AddBoolToObject(object, "phyScdt", receiver.phy_scdt);
        cJSON_AddBoolToObject(object, "sync", receiver.sync);
        add_u64(object, "statusAgeMs", receiver.status_age_ms);
    } else {
        cJSON_AddNullToObject(object, "sysStatus");
        cJSON_AddNullToObject(object, "statusAgeMs");
    }
    if (receiver.detect_ran) {
        cJSON_AddStringToObject(object, "detect", receiver.detect_result);
        cJSON_AddNumberToObject(object, "detectedWidth", receiver.detected_width);
        cJSON_AddNumberToObject(object, "detectedHeight", receiver.detected_height);
        cJSON_AddNumberToObject(object, "detectedFps", receiver.detected_fps);
        add_u64(object, "detectAgeMs", receiver.detect_age_ms);
    } else {
        cJSON_AddNullToObject(object, "detect");
        cJSON_AddNullToObject(object, "detectAgeMs");
    }
    cJSON_AddItemToObject(root, "hdmiReceiver", object);
}

static void add_tls_admissions(cJSON *root)
{
    jetkvm_memory_policy_tls_admission_t admissions[
        JETKVM_MEMORY_TLS_ADMISSION_OWNERS];
    const size_t count = jetkvm_memory_policy_get_tls_admissions(
        admissions, JETKVM_MEMORY_TLS_ADMISSION_OWNERS);
    cJSON *list = cJSON_AddArrayToObject(root, "tlsAdmissions");
    for (size_t i = 0U; list != NULL && i < count; ++i) {
        cJSON *item = cJSON_CreateObject();
        if (item == NULL || !cJSON_AddItemToArray(list, item)) {
            cJSON_Delete(item);
            return;
        }
        cJSON_AddStringToObject(item, "owner", admissions[i].owner);
        cJSON_AddNumberToObject(item, "admitted", admissions[i].admitted);
        cJSON_AddNumberToObject(item, "denied", admissions[i].denied);
        cJSON_AddNumberToObject(item, "lastInternalHeadroom",
                                admissions[i].last_internal_headroom);
        cJSON_AddNumberToObject(item, "minInternalHeadroom",
                                admissions[i].min_internal_headroom);
        cJSON_AddNumberToObject(item, "lastDmaHeadroom",
                                admissions[i].last_dma_headroom);
        cJSON_AddNumberToObject(item, "minDmaHeadroom",
                                admissions[i].min_dma_headroom);
        cJSON_AddNumberToObject(item, "minInternalLargest",
                                admissions[i].min_internal_largest);
        cJSON_AddNumberToObject(item, "minDmaLargest",
                                admissions[i].min_dma_largest);
        cJSON_AddNumberToObject(item, "reservedInternal",
                                admissions[i].reserved_internal);
        cJSON_AddNumberToObject(item, "reservedDma",
                                admissions[i].reserved_dma);
        /* An owner that was only refused has held nothing. */
        if (admissions[i].admitted == 0U) {
            continue;
        }
        cJSON_AddNumberToObject(item, "heldInternalMin",
                                admissions[i].held_internal_min);
        cJSON_AddNumberToObject(item, "heldDmaMin",
                                admissions[i].held_dma_min);
        cJSON_AddNumberToObject(item, "heldInternalFallMax",
                                admissions[i].held_internal_fall_max);
        cJSON_AddNumberToObject(item, "heldDmaFallMax",
                                admissions[i].held_dma_fall_max);
        cJSON_AddNumberToObject(item, "heldOverruns",
                                admissions[i].held_overruns);
        cJSON_AddNumberToObject(item, "heldUnderFloor",
                                admissions[i].held_under_floor);
    }
}

static void add_stage_age(cJSON *root, const char *name,
                          const webrtc_transport_stage_age_stats_t *stats)
{
    cJSON *stage = cJSON_AddObjectToObject(root, name);
    if (!stage) {
        return;
    }
    add_u64(stage, "count", stats->count);
    add_u64(stage, "avgUs", stats->count ? stats->total_us / stats->count : 0);
    cJSON_AddNumberToObject(stage, "maxUs", stats->max_us);
}

static void add_packet_cost(cJSON *root, const char *name,
                            const webrtc_transport_packet_cost_stats_t *stats)
{
    cJSON *cost = cJSON_AddObjectToObject(root, name);
    if (!cost) {
        return;
    }
    add_u64(cost, "count", stats->count);
    add_u64(cost, "avgUs", stats->count ? stats->total_us / stats->count : 0);
    cJSON_AddNumberToObject(cost, "maxUs", stats->max_us);
    add_u64(cost, "bytes", stats->bytes);
    add_u64(cost, "errors", stats->errors);
}

static void add_stage_age_summary(cJSON *root,
                                  const webrtc_transport_stage_age_summary_t *summary)
{
    cJSON *stage_age = cJSON_AddObjectToObject(root, "stageAge");
    if (!stage_age) {
        return;
    }
    cJSON_AddStringToObject(stage_age, "unit", "us");
    cJSON_AddStringToObject(stage_age, "clock", "esp_timer");
    cJSON_AddStringToObject(stage_age, "origin", "csi-eof");
    cJSON_AddStringToObject(stage_age, "cohort",
                            "successful-esp-peer-send-video");
    cJSON_AddStringToObject(stage_age, "rtpObservationPoint",
                            "sender-rtp-transformer+srtp+lwip+emac-handoff");
    cJSON_AddBoolToObject(stage_age, "networkEgressMeasured", true);
    cJSON_AddBoolToObject(stage_age, "wireEgressMeasured", false);
    cJSON_AddNumberToObject(stage_age, "epoch", summary->epoch);

    add_stage_age(stage_age, "captureToPpaDone", &summary->capture_to_ppa_done);
    add_stage_age(stage_age, "captureToEncodeStart", &summary->capture_to_encode_start);
    add_stage_age(stage_age, "captureToEncodeDone", &summary->capture_to_encode_done);
    add_stage_age(stage_age, "captureToAuCopyReady", &summary->capture_to_au_copy_ready);
    add_stage_age(stage_age, "captureToPeerTake", &summary->capture_to_peer_take);
    add_stage_age(stage_age, "captureToSendVideoReturn",
                  &summary->capture_to_send_video_return);
    add_stage_age(stage_age, "captureToFirstRtp", &summary->capture_to_first_rtp);
    add_stage_age(stage_age, "captureToRtpMarker", &summary->capture_to_rtp_marker);
    add_stage_age(stage_age, "captureToSrtpFirst",
                  &summary->capture_to_srtp_first);
    add_stage_age(stage_age, "captureToSrtpMarker",
                  &summary->capture_to_srtp_marker);
    add_stage_age(stage_age, "captureToUdpSendFirst",
                  &summary->capture_to_udp_send_first);
    add_stage_age(stage_age, "captureToUdpSendMarker",
                  &summary->capture_to_udp_send_marker);
    add_stage_age(stage_age, "captureToEmacHandoffFirst",
                  &summary->capture_to_emac_handoff_first);
    add_stage_age(stage_age, "captureToEmacHandoffMarker",
                  &summary->capture_to_emac_handoff_marker);
    add_u64(stage_age, "invalidTimestamps", summary->invalid_timestamps);
    add_u64(stage_age, "orderErrors", summary->order_errors);
    add_u64(stage_age, "accumulatorOverflows", summary->accumulator_overflows);
    add_u64(stage_age, "epochMismatches", summary->epoch_mismatches);
    add_u64(stage_age, "missingFirstRtp", summary->missing_first_rtp);
    add_u64(stage_age, "missingRtpMarker", summary->missing_rtp_marker);
}

static void add_network_timing(
    cJSON *root, const webrtc_transport_network_timing_stats_t *stats)
{
    cJSON *network = cJSON_AddObjectToObject(root, "networkTiming");
    if (!network) {
        return;
    }
    cJSON_AddStringToObject(network, "unit", "us");
    cJSON_AddStringToObject(network, "clock", "esp_timer");
    cJSON_AddStringToObject(network, "emacObservationPoint",
                            "DMA-descriptor-handoff-return");
    cJSON_AddBoolToObject(network, "wireCompletionMeasured", false);
    add_packet_cost(network, "srtpProtect", &stats->srtp_protect);
    add_packet_cost(network, "srtpCipher", &stats->srtp_cipher);
    add_packet_cost(network, "srtpAuthAndFramework",
                    &stats->srtp_auth_and_framework);
    add_packet_cost(network, "lwipSendto", &stats->lwip_sendto);
    add_packet_cost(network, "emacHandoff", &stats->emac_handoff);
    add_u64(network, "traceOverwrites", stats->trace_overwrites);
    add_u64(network, "unmatchedSrtpPackets", stats->unmatched_srtp_packets);
    add_u64(network, "unmatchedUdpPackets", stats->unmatched_udp_packets);
    add_u64(network, "unmatchedEmacPackets", stats->unmatched_emac_packets);
    add_u64(network, "rtpRetransmits", stats->rtp_retransmits);
    cJSON *srtp_errors =
        cJSON_AddObjectToObject(network, "srtpProtectErrorDetails");
    if (srtp_errors) {
        add_u64(srtp_errors, "replayFail", stats->srtp_error_replay_fail);
        add_u64(srtp_errors, "replayOld", stats->srtp_error_replay_old);
        add_u64(srtp_errors, "bufferSmall", stats->srtp_error_buffer_small);
        add_u64(srtp_errors, "authFail", stats->srtp_error_auth_fail);
        add_u64(srtp_errors, "cipherFail", stats->srtp_error_cipher_fail);
        add_u64(srtp_errors, "other", stats->srtp_error_other);
        cJSON_AddNumberToObject(srtp_errors, "lastStatus",
                                stats->srtp_last_error_status);
        cJSON_AddNumberToObject(srtp_errors, "lastSequence",
                                stats->srtp_last_error_sequence);
        cJSON_AddNumberToObject(srtp_errors, "lastTimestamp",
                                stats->srtp_last_error_timestamp);
        cJSON_AddNumberToObject(srtp_errors, "lastSsrc",
                                stats->srtp_last_error_ssrc);
        cJSON_AddNumberToObject(srtp_errors, "lastInputBytes",
                                stats->srtp_last_error_input_bytes);
        cJSON_AddNumberToObject(srtp_errors, "lastOutputCapacity",
                                stats->srtp_last_error_output_capacity);
        cJSON_AddNumberToObject(srtp_errors, "lastOutputBytes",
                                stats->srtp_last_error_output_bytes);
        cJSON_AddNumberToObject(srtp_errors, "lastFingerprint",
                                stats->srtp_last_error_fingerprint);
    }
}

static void add_firmware_identity(cJSON *root)
{
    const esp_app_desc_t *description = esp_app_get_description();
    cJSON *firmware = cJSON_AddObjectToObject(root, "firmware");
    if (!description || !firmware) {
        return;
    }
    char elf_sha256[sizeof(description->app_elf_sha256) * 2 + 1];
    for (size_t i = 0; i < sizeof(description->app_elf_sha256); ++i) {
        snprintf(&elf_sha256[i * 2], 3, "%02x", description->app_elf_sha256[i]);
    }
    cJSON_AddStringToObject(firmware, "projectName", description->project_name);
    cJSON_AddStringToObject(firmware, "version", description->version);
    cJSON_AddStringToObject(firmware, "buildDate", description->date);
    cJSON_AddStringToObject(firmware, "buildTime", description->time);
    cJSON_AddStringToObject(firmware, "idfVersion", description->idf_ver);
    cJSON_AddStringToObject(firmware, "appElfSha256", elf_sha256);
    cJSON_AddNumberToObject(firmware, "secureVersion", description->secure_version);
}

static void add_flash_read_mode(cJSON *root)
{
    jetkvm_flash_read_mode_t mode;
    jetkvm_flash_read_mode_get(&mode);
    cJSON *flash = cJSON_AddObjectToObject(root, "flash");
    if (flash) {
        cJSON_AddStringToObject(flash, "cacheReadMode", mode.cache_read_mode);
        cJSON_AddStringToObject(flash, "quadRead", mode.quad_read);
    }
}

/* The fill byte of an unused task stack (tskSTACK_FILL_BYTE of tasks.c). */
#define TASK_STACK_FILL_BYTE 0xa5U
#define TASK_STACK_MAX_TASKS 96U

typedef struct {
    TaskHandle_t handle;
    uint32_t size;
    uint32_t high_watermark;
    char name[configMAX_TASK_NAME_LEN];
    int8_t core_id;
    bool psram;
    bool measured;
} task_stack_sample_t;

typedef struct {
    uint64_t sampled_us;
    uint32_t scan_us;
    uint32_t max_masked_us;
    uint32_t count;
    bool truncated;
    task_stack_sample_t tasks[TASK_STACK_MAX_TASKS];
} task_stack_snapshot_t;

/* In PSRAM: the list must not take internal RAM from DTLS admission. */
static EXT_RAM_BSS_ATTR task_stack_snapshot_t s_task_stack_snapshot;
static _lock_t s_task_stack_snapshot_lock;

/* The bytes from the stack's lowest address that the task never wrote. */
static uint32_t task_stack_unused_bytes(const uint8_t *start, const uint8_t *end)
{
    const uint8_t *at = start;
    while (at < end && ((uintptr_t)at & 3U) != 0 && *at == TASK_STACK_FILL_BYTE) {
        ++at;
    }
    if (((uintptr_t)at & 3U) == 0) {
        typedef uint32_t __attribute__((may_alias)) stack_word_t;
        const uint32_t fill = TASK_STACK_FILL_BYTE * 0x01010101U;
        while ((size_t)(end - at) >= sizeof(uint32_t) && *(const stack_word_t *)at == fill) {
            at += sizeof(uint32_t);
        }
        while (at < end && *at == TASK_STACK_FILL_BYTE) {
            ++at;
        }
    }
    return (uint32_t)(at - start);
}

/* What the kernel records of one task, and the unused bytes of its stack.
 * Call with the kernel lock held, for a task that is in the kernel's lists.
 *
 * The kernel records the stack's lowest address and its highest, which is 1
 * to 16 bytes under the end of the stack's memory (16-byte alignment). The
 * size from the two is the size that the task was created with when that
 * size is a multiple of 16 bytes, and a multiple of 16 bytes within 15 bytes
 * of it otherwise. At a task's first use of the FPU or of the vector unit
 * the port takes the save area for it from the stack's low end and moves the
 * recorded lowest address up, so the size is then smaller by that area: it
 * is what the task has. */
static void task_stack_measure(task_stack_sample_t *task)
{
    TaskSnapshot_t tcb;
    if (vTaskGetSnapshot(task->handle, &tcb) != pdTRUE) {
        return;
    }
    const uint8_t *start = (const uint8_t *)xTaskGetStackStart(task->handle);
    const uint8_t *end = (const uint8_t *)tcb.pxEndOfStack;
    if (start == NULL || end <= start) {
        return;
    }
    memcpy(task->name, pcTaskGetName(task->handle), sizeof(task->name) - 1);
    /* FPU/PIE use can pin a task after creation. IDF also maps
     * SMP affinity masks to a single core or tskNO_AFFINITY. */
    BaseType_t core_id = xTaskGetCoreID(task->handle);
    task->core_id = core_id == tskNO_AFFINITY ? -1 : (int8_t)core_id;
    task->size = ((uint32_t)(end - start) + 16U) & ~15U;
    task->psram = esp_ptr_external_ram(start);
    task->high_watermark = task_stack_unused_bytes(start, end);
    task->measured = true;
}

static void task_stack_masked_since(task_stack_snapshot_t *snapshot, int64_t from_us)
{
    uint32_t masked_us = (uint32_t)(esp_timer_get_time() - from_us);
    if (masked_us > snapshot->max_masked_us) {
        snapshot->max_masked_us = masked_us;
    }
}

/* A task can end, and the kernel can free its control block and stack, at
 * any moment in which the kernel lock is not held. The lock masks interrupts
 * and stops the scheduler of both cores, so the scan does not hold it for
 * all tasks at once: it holds it once to list the tasks' handles, and then
 * once per task, in which it looks the handle up again and reads everything
 * of that task. A task that ended in between is left out. A new task that
 * got the handle of one that ended is in the list as itself. One stack is
 * read in one hold: between two holds its task could write under the bytes
 * that were already counted as unused. The longest hold, with the wait
 * for the lock, is in max_masked_us. A task that runs on the other core
 * during the hold for it is not stopped, as in the kernel's own
 * uxTaskGetStackHighWaterMark: what it writes under the read position in
 * that time is in the next scan. */
static void task_stack_scan(task_stack_snapshot_t *snapshot)
{
    memset(snapshot, 0, sizeof(*snapshot));
    snapshot->sampled_us = (uint64_t)esp_timer_get_time();

    TaskIterator_t iterator = {0};
    /* The time from before the take: the wait for the lock is masked too. */
    int64_t from_us = esp_timer_get_time();
    prvTakeKernelLock();
    while (xTaskGetNext(&iterator) != -1) {
        TaskHandle_t handle = iterator.pxTaskHandle;
        bool listed = handle == NULL;
        /* A task that waits for the scheduler of the other core to resume is
         * in two of the kernel's lists. */
        for (uint32_t i = 0; !listed && i < snapshot->count; ++i) {
            listed = snapshot->tasks[i].handle == handle;
        }
        if (listed) {
            continue;
        }
        if (snapshot->count == TASK_STACK_MAX_TASKS) {
            snapshot->truncated = true;
            break;
        }
        snapshot->tasks[snapshot->count++].handle = handle;
    }
    task_stack_masked_since(snapshot, from_us);
    prvReleaseKernelLock();

    for (uint32_t i = 0; i < snapshot->count; ++i) {
        task_stack_sample_t *task = &snapshot->tasks[i];
        memset(&iterator, 0, sizeof(iterator));
        from_us = esp_timer_get_time();
        prvTakeKernelLock();
        while (xTaskGetNext(&iterator) != -1) {
            if (iterator.pxTaskHandle == task->handle) {
                task_stack_measure(task);
                break;
            }
        }
        task_stack_masked_since(snapshot, from_us);
        prvReleaseKernelLock();
    }
    snapshot->scan_us = (uint32_t)((uint64_t)esp_timer_get_time() - snapshot->sampled_us);
}

/* The longest entry of the list as JSON text: 98 characters of names and
 * punctuation, a name of 15, a core of 2, three numbers of 10 and a memory
 * of 8, and the comma before it. */
#define TASK_STACK_ENTRY_JSON_BYTES 160U

/* `taskStacks` as one raw item of JSON text. A cJSON object per task takes
 * about 0.5 KB of internal RAM per task while the answer is built, because
 * allocations under 1 KiB are internal; the text is one allocation, which
 * is in PSRAM from 1 KiB up. */
static void add_task_stack_list(cJSON *root, const task_stack_snapshot_t *snapshot)
{
    size_t capacity = 3U + snapshot->count * TASK_STACK_ENTRY_JSON_BYTES;
    char *text = heap_caps_malloc(capacity, MALLOC_CAP_SPIRAM);
    if (text == NULL) {
        cJSON_AddArrayToObject(root, "taskStacks");
        return;
    }
    size_t used = 0;
    text[used++] = '[';
    for (uint32_t i = 0; i < snapshot->count; ++i) {
        const task_stack_sample_t *task = &snapshot->tasks[i];
        if (!task->measured) {
            continue;
        }
        /* A task's name is any bytes: keep what needs no escape in JSON. */
        char name[sizeof(task->name)];
        for (size_t at = 0; at < sizeof(name); ++at) {
            unsigned char byte = (unsigned char)task->name[at];
            bool plain = byte == 0 || (byte >= 0x20 && byte < 0x7f && byte != '"' && byte != '\\');
            name[at] = plain ? (char)byte : '?';
        }
        size_t room = capacity - used - 1U;
        int written = snprintf(text + used, room,
            "%s{\"name\":\"%s\",\"coreId\":%d,\"configuredBytes\":%" PRIu32
            ",\"highWatermarkBytes\":%" PRIu32 ",\"peakUsedBytes\":%" PRIu32
            ",\"stackMemory\":\"%s\"}",
            used > 1U ? "," : "", name, (int)task->core_id, task->size,
            task->high_watermark, task->size - task->high_watermark,
            task->psram ? "psram" : "internal");
        if (written < 0 || (size_t)written >= room) {
            break;
        }
        used += (size_t)written;
    }
    text[used++] = ']';
    text[used] = '\0';
    cJSON_AddRawToObject(root, "taskStacks", text);
    heap_caps_free(text);
}

static void add_task_stack_budgets(cJSON *root, bool detailed)
{
    task_stack_snapshot_t *snapshot = &s_task_stack_snapshot;
    _lock_acquire(&s_task_stack_snapshot_lock);
    if (detailed) {
        /* The scan holds the kernel lock, which masks interrupts. Keep it
         * out of routine live HTTP polling, just like heap details. Only an
         * idle snapshot or explicit details=1 refreshes this cache. */
        task_stack_scan(snapshot);
    }

    cJSON *detail = cJSON_AddObjectToObject(root, "taskStackDetails");
    if (detail != NULL) {
        cJSON_AddBoolToObject(detail, "cached", !detailed);
        cJSON_AddBoolToObject(detail, "available", snapshot->sampled_us != 0);
        cJSON_AddStringToObject(detail, "scope",
                                "taskPresenceHighWatermarksAndCoreAffinity");
        if (snapshot->sampled_us == 0) {
            cJSON_AddNullToObject(detail, "sampledAtUs");
            cJSON_AddNullToObject(detail, "ageMs");
        } else {
            uint64_t now_us = (uint64_t)esp_timer_get_time();
            add_u64(detail, "sampledAtUs", snapshot->sampled_us);
            add_u64(detail, "ageMs", now_us >= snapshot->sampled_us ?
                    (now_us - snapshot->sampled_us) / 1000U : 0U);
            cJSON_AddNumberToObject(detail, "scanUs", snapshot->scan_us);
            cJSON_AddNumberToObject(detail, "maxMaskedUs", snapshot->max_masked_us);
            cJSON_AddBoolToObject(detail, "truncated", snapshot->truncated);
        }
    }
    add_task_stack_list(root, snapshot);
    _lock_release(&s_task_stack_snapshot_lock);
}

static esp_err_t stats_json_create(cJSON **root_out, bool detailed)
{
    if (root_out == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    *root_out = NULL;
    webrtc_transport_stats_t stats = { 0 };
    esp_err_t error = webrtc_transport_get_stats(&stats);
    if (error != ESP_OK) {
        return error;
    }
    /* Refresh while the source is idle, or only on explicit diagnostics.
     * A normal poll during a live stream never requests a full heap walk. */
    detailed = detailed || (!stats.mode_valid && !stats.browser_session_active);
    if (detailed) {
        error = webrtc_transport_get_stats_detailed(&stats);
        if (error != ESP_OK) return error;
    }
    cJSON *root = cJSON_CreateObject();
    if (root == NULL) {
        return ESP_ERR_NO_MEM;
    }
    add_heap_detail_source(root, "memoryDetails", stats.memory_details_cached,
                            stats.memory_details_sample_timestamp_us);
    add_heap_detail_source(root, "peerMemoryDetails", stats.memory_details_cached,
                            stats.peer_memory_details_sample_timestamp_us);
    cJSON_AddBoolToObject(root, "started", stats.started);
    cJSON_AddBoolToObject(root, "modeValid", stats.mode_valid);
    cJSON_AddBoolToObject(root, "sessionActive", stats.browser_session_active);
    cJSON_AddBoolToObject(root, "peerConnected", stats.peer_connected);
    add_paused_streams(root, stats.video_pause_sources);
    cJSON_AddNumberToObject(root, "videoTargetBitrate", stats.video_target_bitrate);
    cJSON_AddNumberToObject(root, "videoBitrateCeiling", stats.video_bitrate_ceiling);
    cJSON_AddNumberToObject(root, "videoRembBitrate", stats.video_remb_bitrate);
    cJSON_AddNumberToObject(root, "videoRembReports", stats.video_remb_reports);
    cJSON_AddNumberToObject(root, "videoRembDecreases", stats.video_remb_decreases);
    cJSON_AddNumberToObject(root, "videoAchievedBitrate", stats.video_achieved_bitrate);
    cJSON_AddNumberToObject(root, "videoBitrateLossPermille", stats.video_bitrate_loss_permille);
    cJSON_AddNumberToObject(root, "accessUnitsNoSlot", stats.access_units_no_slot);
    cJSON_AddNumberToObject(root, "videoRembCuts", stats.video_remb_cuts);
    cJSON_AddNumberToObject(root, "videoRembIgnored", stats.video_remb_ignored);
    cJSON_AddNumberToObject(root, "videoBitrateRestores", stats.video_bitrate_restores);
    cJSON_AddStringToObject(root, "videoBitrateReason",
                            stats.video_bitrate_reason ? stats.video_bitrate_reason : "hold");
    cJSON_AddBoolToObject(root, "waitingForSync", stats.waiting_for_sync_frame);
    cJSON_AddNumberToObject(root, "width", stats.width);
    cJSON_AddNumberToObject(root, "height", stats.height);
    cJSON_AddNumberToObject(root, "fps", stats.fps);
    cJSON_AddNumberToObject(root, "generation", stats.generation);
    cJSON_AddNumberToObject(root, "videoEpoch", stats.video_epoch);
    cJSON_AddStringToObject(root, "peerState",
                            webrtc_transport_peer_state_name(stats.peer_state));
    add_u64(root, "accessUnitsOffered", stats.access_units_offered);
    add_u64(root, "accessUnitsEnqueued", stats.access_units_enqueued);
    add_u64(root, "accessUnitsSent", stats.access_units_sent);
    add_u64(root, "accessUnitBytesSent", stats.access_unit_bytes_sent);
    add_u64(root, "accessUnitsReplaced", stats.access_units_replaced);
    add_u64(root, "accessUnitsOversize", stats.access_units_oversize);
    add_u64(root, "accessUnitsBadAnnexB", stats.access_units_bad_annexb);
    add_u64(root, "accessUnitsNoSession", stats.access_units_no_session);
    add_u64(root, "accessUnitsModeMismatch", stats.access_units_mode_mismatch);
    add_u64(root, "accessUnitsWaitingForSync", stats.access_units_waiting_for_sync);
    add_u64(root, "accessUnitsQueueFull", stats.access_units_queue_full);
    add_u64(root, "accessUnitsSendErrors", stats.access_units_send_errors);
    add_u64(root, "accessUnitsWouldBlock", stats.access_units_would_block);
    add_u64(root, "ptsRegressions", stats.pts_regressions);
    cJSON_AddNumberToObject(root, "lastInputPts90k", stats.last_input_pts_90khz);
    add_u64(root, "rtpPacketsSent", stats.rtp_packets_sent);
    add_u64(root, "rtpBytesSent", stats.rtp_bytes_sent);
    add_u64(root, "rtpMarkerPackets", stats.rtp_marker_packets);
    cJSON_AddNumberToObject(root, "lastRtpTimestamp90k", stats.last_rtp_timestamp_90khz);
    cJSON *playout_delay = cJSON_AddObjectToObject(root, "rtpPlayoutDelay");
    if (playout_delay) {
        cJSON_AddStringToObject(playout_delay, "uri", WEBRTC_PLAYOUT_DELAY_URI);
        cJSON_AddStringToObject(playout_delay, "wireFormat",
                                "RFC8285-one-byte-min0-max0");
        cJSON_AddNumberToObject(playout_delay, "rtpOverheadBytes",
                                WEBRTC_PLAYOUT_DELAY_RTP_OVERHEAD);
        cJSON_AddBoolToObject(playout_delay, "requested",
                             stats.rtp_playout_delay_requested);
        cJSON_AddBoolToObject(playout_delay, "offered",
                             stats.rtp_playout_delay_offered);
        cJSON_AddBoolToObject(playout_delay, "negotiated",
                             stats.rtp_playout_delay_negotiated);
        cJSON_AddNumberToObject(playout_delay, "extensionId",
                                stats.rtp_playout_delay_extension_id);
        add_u64(playout_delay, "offerFailures",
                stats.rtp_playout_delay_offer_failures);
        add_u64(playout_delay, "answerRejections",
                stats.rtp_playout_delay_answer_rejections);
        add_u64(playout_delay, "packetsStamped",
                stats.rtp_playout_delay_packets_stamped);
        add_u64(playout_delay, "packetsBypassed",
                stats.rtp_playout_delay_packets_bypassed);
        add_u64(playout_delay, "transformErrors",
                stats.rtp_playout_delay_transform_errors);
        add_u64(playout_delay, "bytesAdded",
                stats.rtp_playout_delay_bytes_added);
    }
    add_u64(root, "peerRebuilds", stats.peer_rebuilds);
    add_u64(root, "peerConnects", stats.peer_connects);
    add_u64(root, "videoPeerReleases", stats.video_peer_releases);
    cJSON *peer_memory = cJSON_AddObjectToObject(root, "peerMemoryArena");
    if (peer_memory) {
        cJSON_AddBoolToObject(peer_memory, "active",
                             stats.peer_memory_arena_active);
        cJSON_AddNumberToObject(peer_memory, "capacity",
                                stats.peer_memory_arena_capacity);
        cJSON_AddNumberToObject(peer_memory, "free",
                                stats.peer_memory_arena_free);
        cJSON_AddNumberToObject(peer_memory, "minimum",
                                stats.peer_memory_arena_minimum);
        cJSON_AddNumberToObject(peer_memory, "largest",
                                stats.peer_memory_arena_largest);
        cJSON_AddNumberToObject(peer_memory, "allocatedBlocks",
                                stats.peer_memory_arena_allocated_blocks);
        cJSON_AddNumberToObject(
            peer_memory, "allocationFailures",
            stats.peer_memory_arena_allocation_failures);
        cJSON_AddNumberToObject(peer_memory, "beginFailures",
                                stats.peer_memory_arena_begin_failures);
        cJSON_AddNumberToObject(peer_memory, "leakEvents",
                                stats.peer_memory_arena_leak_events);
    }
    add_u64(root, "syncIdrRequests", stats.sync_idr_requests);
    add_u64(root, "syncIdrIssued", stats.sync_idr_issued);
    add_u64(root, "syncIdrCommitted", stats.sync_idr_committed);
    add_u64(root, "syncIdrSuccesses", stats.sync_idr_successes);
    add_u64(root, "syncIdrFailures", stats.sync_idr_failures);
    cJSON_AddNumberToObject(root, "syncIdrPendingGeneration",
                            stats.sync_idr_pending_generation);
    cJSON_AddNumberToObject(root, "syncIdrInflightGeneration",
                            stats.sync_idr_inflight_generation);
    cJSON_AddNumberToObject(root, "syncIdrCommittedGeneration",
                            stats.sync_idr_committed_generation);
    add_u64(root, "signalingMessagesTx", stats.signaling_messages_tx);
    add_u64(root, "signalingMessagesRx", stats.signaling_messages_rx);
    add_u64(root, "signalingMessagesDropped", stats.signaling_messages_dropped);
    cJSON_AddNumberToObject(root, "signalingResidentBytes",
                            stats.signaling_resident_bytes);
    cJSON_AddNumberToObject(root, "signalingResidentHighWatermark",
                            stats.signaling_resident_high_watermark);
    cJSON *data_channel = cJSON_AddObjectToObject(root, "jetkvmDataChannels");
    if (data_channel) {
        cJSON_AddStringToObject(data_channel, "protocolScope",
                                "JetKVM rpc plus generation-gated HIDRPC v1; USB output uses P4 HS/UTMI when initialized");
        cJSON_AddBoolToObject(data_channel, "rpcOpen",
                             stats.rpc_data_channel_open);
        cJSON_AddBoolToObject(data_channel, "hidrpcOpen",
                             stats.hidrpc_data_channel_open);
        cJSON_AddBoolToObject(
            data_channel, "hidrpcUnreliableOrderedOpen",
            stats.hidrpc_unreliable_ordered_data_channel_open);
        cJSON_AddBoolToObject(
            data_channel, "hidrpcUnreliableUnorderedOpen",
            stats.hidrpc_unreliable_unordered_data_channel_open);
        cJSON_AddNumberToObject(data_channel, "rpcStreamId",
                                stats.rpc_data_channel_stream_id);
        cJSON_AddNumberToObject(data_channel, "hidrpcStreamId",
                                stats.hidrpc_data_channel_stream_id);
        cJSON_AddNumberToObject(
            data_channel, "hidrpcUnreliableOrderedStreamId",
            stats.hidrpc_unreliable_ordered_data_channel_stream_id);
        cJSON_AddNumberToObject(
            data_channel, "hidrpcUnreliableUnorderedStreamId",
            stats.hidrpc_unreliable_unordered_data_channel_stream_id);
        cJSON_AddNumberToObject(data_channel, "hidrpcActiveGeneration",
                                stats.hidrpc_active_generation);
        add_u64(data_channel, "opens", stats.data_channel_opens);
        add_u64(data_channel, "closes", stats.data_channel_closes);
        add_u64(data_channel, "messagesRx", stats.data_channel_messages_rx);
        add_u64(data_channel, "messagesTx", stats.data_channel_messages_tx);
        add_u64(data_channel, "bytesRx", stats.data_channel_bytes_rx);
        add_u64(data_channel, "bytesTx", stats.data_channel_bytes_tx);
        add_u64(data_channel, "sendErrors",
                stats.data_channel_send_errors);
        add_u64(data_channel, "sendQueued", stats.data_channel_send_queued);
        cJSON_AddNumberToObject(data_channel, "sendQueueBytes",
                                stats.data_channel_send_queue_bytes);
        add_u64(data_channel, "protocolErrors",
                stats.data_channel_protocol_errors);
        add_u64(data_channel, "rpcResponses", stats.rpc_responses);
        cJSON *reboot = cJSON_AddObjectToObject(data_channel, "reboot");
        if (reboot) {
            add_u64(reboot, "requests", stats.reboot_requests);
            add_u64(reboot, "accepted", stats.reboot_accepted);
            add_u64(reboot, "busy", stats.reboot_busy);
            add_u64(reboot, "responseSent", stats.reboot_response_sent);
            add_u64(reboot, "willRebootSent",
                    stats.reboot_will_reboot_sent);
            add_u64(reboot, "sendFailures", stats.reboot_send_failures);
            add_u64(reboot, "executions", stats.reboot_executions);
        }
        cJSON *rpc_events =
            cJSON_AddObjectToObject(data_channel, "rpcEvents");
        if (rpc_events) {
            add_u64(rpc_events, "queued",
                    stats.rpc_event_notifications_queued);
            add_u64(rpc_events, "coalesced",
                    stats.rpc_event_notifications_coalesced);
            add_u64(rpc_events, "sent",
                    stats.rpc_event_notifications_sent);
            add_u64(rpc_events, "dropped",
                    stats.rpc_event_notifications_dropped);
            add_u64(rpc_events, "renderErrors",
                    stats.rpc_event_render_errors);
            add_u64(rpc_events, "pollErrors",
                    stats.rpc_event_poll_errors);
            cJSON_AddNumberToObject(rpc_events, "pendingMask",
                                    stats.rpc_event_pending_mask);
        }
        add_u64(data_channel, "hidrpcHandshakes",
                stats.hidrpc_handshakes);
        cJSON_AddBoolToObject(data_channel, "hidrpcHandshakeComplete",
                             stats.hidrpc_handshake_complete);
        cJSON *hidrpc_state =
            cJSON_AddObjectToObject(data_channel, "hidrpcState");
        if (hidrpc_state) {
            add_u64(hidrpc_state, "queued",
                    stats.hidrpc_state_notifications_queued);
            add_u64(hidrpc_state, "coalesced",
                    stats.hidrpc_state_notifications_coalesced);
            add_u64(hidrpc_state, "sent",
                    stats.hidrpc_state_notifications_sent);
            add_u64(hidrpc_state, "dropped",
                    stats.hidrpc_state_notifications_dropped);
            cJSON_AddNumberToObject(hidrpc_state, "pendingMask",
                                    stats.hidrpc_state_pending_mask);
        }
        add_u64(data_channel, "hidrpcUnsupportedMessages",
                stats.hidrpc_unsupported_messages);
        add_u64(data_channel, "hidrpcEventsAccepted",
                stats.hidrpc_events_accepted);
        add_u64(data_channel, "hidrpcEventsDropped",
                stats.hidrpc_events_dropped);
        add_u64(data_channel, "hidrpcEventsBackpressured",
                stats.hidrpc_events_backpressured);
        add_u64(data_channel, "hidrpcEventsUnsupported",
                stats.hidrpc_events_unsupported);
        add_u64(data_channel, "hidrpcInactiveMessages",
                stats.hidrpc_inactive_messages);
    }
    jetkvm_hid_input_snapshot_t hid_snapshot = {0};
    jetkvm_hid_input_stats_t hid_stats = {0};
    bool have_hid_snapshot =
        jetkvm_hid_input_get_snapshot(&hid_snapshot) == ESP_OK;
    bool have_hid_stats = jetkvm_hid_input_get_stats(&hid_stats) == ESP_OK;
    cJSON *usb_hid = cJSON_AddObjectToObject(root, "usbHid");
    if (usb_hid) {
        cJSON_AddBoolToObject(usb_hid, "initialized",
                             have_hid_snapshot && hid_snapshot.initialized);
        cJSON_AddBoolToObject(usb_hid, "usbAvailable",
                             have_hid_snapshot &&
                                 hid_snapshot.usb_available);
        cJSON_AddBoolToObject(usb_hid, "acceptingInput",
                             have_hid_snapshot &&
                                 hid_snapshot.accepting_input);
        cJSON_AddNumberToObject(
            usb_hid, "activeGeneration",
            have_hid_snapshot ? hid_snapshot.active_generation : 0);
        add_u64(usb_hid, "lastUserInputUs",
                have_hid_snapshot && hid_snapshot.last_user_input_us > 0
                    ? (uint64_t)hid_snapshot.last_user_input_us
                    : 0U);
        cJSON_AddBoolToObject(usb_hid, "connected",
                             have_hid_snapshot &&
                                 hid_snapshot.usb.connected);
        cJSON_AddBoolToObject(usb_hid, "mounted",
                             have_hid_snapshot && hid_snapshot.usb.mounted);
        cJSON_AddBoolToObject(usb_hid, "suspended",
                             have_hid_snapshot && hid_snapshot.usb.suspended);
        cJSON_AddBoolToObject(
            usb_hid, "remoteWakeEnabled",
            have_hid_snapshot &&
                hid_snapshot.usb.remote_wakeup_enabled);
        cJSON_AddBoolToObject(
            usb_hid, "highSpeed",
            have_hid_snapshot && hid_snapshot.usb.link_high_speed);
        cJSON_AddNumberToObject(
            usb_hid, "keyboardLeds",
            have_hid_snapshot ? hid_snapshot.keyboard_leds : 0);
        cJSON_AddBoolToObject(
            usb_hid, "macroActive",
            have_hid_snapshot && hid_snapshot.macro_active);
        cJSON_AddBoolToObject(
            usb_hid, "macroIsPaste",
            have_hid_snapshot && hid_snapshot.macro_is_paste);
        cJSON_AddNumberToObject(
            usb_hid, "macroStepIndex",
            have_hid_snapshot ? hid_snapshot.macro_step_index : 0);
        cJSON_AddNumberToObject(
            usb_hid, "macroStepCount",
            have_hid_snapshot ? hid_snapshot.macro_step_count : 0);
        add_u64(usb_hid, "macroStateRevision",
                have_hid_snapshot ? hid_snapshot.macro_state_revision : 0);
        cJSON_AddNumberToObject(
            usb_hid, "reportsSent",
            have_hid_snapshot ? hid_snapshot.usb.reports_sent : 0);
        cJSON_AddNumberToObject(
            usb_hid, "reportsBusy",
            have_hid_snapshot ? hid_snapshot.usb.reports_busy : 0);
        cJSON_AddNumberToObject(
            usb_hid, "mountCount",
            have_hid_snapshot ? hid_snapshot.usb.mount_count : 0);
        cJSON_AddNumberToObject(
            usb_hid, "unmountCount",
            have_hid_snapshot ? hid_snapshot.usb.unmount_count : 0);
        cJSON_AddNumberToObject(
            usb_hid, "suspendCount",
            have_hid_snapshot ? hid_snapshot.usb.suspend_count : 0);
        cJSON_AddNumberToObject(
            usb_hid, "resumeCount",
            have_hid_snapshot ? hid_snapshot.usb.resume_count : 0);
        cJSON_AddNumberToObject(
            usb_hid, "wakeAttempts",
            have_hid_snapshot ? hid_snapshot.usb.wake_attempts : 0);
        cJSON_AddNumberToObject(
            usb_hid, "wakeSuccess",
            have_hid_snapshot ? hid_snapshot.usb.wake_success : 0);
        cJSON_AddNumberToObject(
            usb_hid, "wakeFailures",
            have_hid_snapshot ? hid_snapshot.usb.wake_failures : 0);
        cJSON *msc_read = cJSON_AddObjectToObject(usb_hid, "mscRead");
        if (msc_read) {
            add_u64(msc_read, "calls",
                    have_hid_snapshot ? hid_snapshot.usb.msc_read_calls : 0);
            add_u64(msc_read, "bytes",
                    have_hid_snapshot ? hid_snapshot.usb.msc_read_bytes : 0);
            add_u64(msc_read, "errors",
                    have_hid_snapshot ? hid_snapshot.usb.msc_read_errors : 0);
            add_u64(msc_read, "totalUs",
                    have_hid_snapshot ? hid_snapshot.usb.msc_read_total_us : 0);
            add_u64(msc_read, "commands",
                    have_hid_snapshot ? hid_snapshot.usb.msc_read_commands : 0);
            add_u64(
                msc_read, "commandTotalUs",
                have_hid_snapshot ?
                    hid_snapshot.usb.msc_read_command_total_us : 0);
            add_u64(
                msc_read, "intraTransferGapSamples",
                have_hid_snapshot ?
                    hid_snapshot.usb.msc_read_intra_gap_samples : 0);
            add_u64(
                msc_read, "intraTransferGapUs",
                have_hid_snapshot ?
                    hid_snapshot.usb.msc_read_intra_gap_total_us : 0);
            add_u64(
                msc_read, "sequentialCommandGapSamples",
                have_hid_snapshot ?
                    hid_snapshot.usb.msc_read_sequential_command_gap_samples :
                    0);
            add_u64(
                msc_read, "sequentialCommandGapUs",
                have_hid_snapshot ?
                    hid_snapshot.usb.msc_read_sequential_command_gap_total_us :
                    0);
            add_u64(
                msc_read, "nonReadCommands",
                have_hid_snapshot ?
                    hid_snapshot.usb.msc_read_non_read_commands : 0);
            add_u64(
                msc_read, "timingResets",
                have_hid_snapshot ?
                    hid_snapshot.usb.msc_read_timing_resets : 0);
            cJSON_AddNumberToObject(
                msc_read, "maxUs",
                have_hid_snapshot ? hid_snapshot.usb.msc_read_max_us : 0);
            cJSON_AddNumberToObject(
                msc_read, "maxCommandUs",
                have_hid_snapshot ?
                    hid_snapshot.usb.msc_read_max_command_us : 0);
            cJSON_AddNumberToObject(
                msc_read, "maxIntraTransferGapUs",
                have_hid_snapshot ?
                    hid_snapshot.usb.msc_read_max_intra_gap_us : 0);
            cJSON_AddNumberToObject(
                msc_read, "maxSequentialCommandGapUs",
                have_hid_snapshot ?
                    hid_snapshot.usb.msc_read_max_sequential_command_gap_us :
                    0);
            cJSON_AddNumberToObject(
                msc_read, "minRequestBytes",
                have_hid_snapshot ? hid_snapshot.usb.msc_read_min_size : 0);
            cJSON_AddNumberToObject(
                msc_read, "maxRequestBytes",
                have_hid_snapshot ? hid_snapshot.usb.msc_read_max_size : 0);
        }
        cJSON *msc_write = cJSON_AddObjectToObject(usb_hid, "mscWrite");
        if (msc_write) {
            add_u64(msc_write, "calls",
                    have_hid_snapshot ? hid_snapshot.usb.msc_write_calls : 0);
            add_u64(msc_write, "bytes",
                    have_hid_snapshot ? hid_snapshot.usb.msc_write_bytes : 0);
            add_u64(msc_write, "errors",
                    have_hid_snapshot ? hid_snapshot.usb.msc_write_errors : 0);
            add_u64(msc_write, "totalUs",
                    have_hid_snapshot ? hid_snapshot.usb.msc_write_total_us : 0);
            add_u64(msc_write, "commands",
                    have_hid_snapshot ? hid_snapshot.usb.msc_write_commands : 0);
            add_u64(
                msc_write, "commandTotalUs",
                have_hid_snapshot ?
                    hid_snapshot.usb.msc_write_command_total_us : 0);
            cJSON_AddNumberToObject(
                msc_write, "maxUs",
                have_hid_snapshot ? hid_snapshot.usb.msc_write_max_us : 0);
            cJSON_AddNumberToObject(
                msc_write, "maxCommandUs",
                have_hid_snapshot ?
                    hid_snapshot.usb.msc_write_max_command_us : 0);
            cJSON_AddNumberToObject(
                msc_write, "minRequestBytes",
                have_hid_snapshot ? hid_snapshot.usb.msc_write_min_size : 0);
            cJSON_AddNumberToObject(
                msc_write, "maxRequestBytes",
                have_hid_snapshot ? hid_snapshot.usb.msc_write_max_size : 0);
        }
        cJSON_AddNumberToObject(
            usb_hid, "lastError",
            have_hid_snapshot ? hid_snapshot.usb.last_error :
                                ESP_ERR_INVALID_STATE);
        jetkvm_virtual_media_io_stats_t media_io = {0};
        bool have_media_io =
            jetkvm_virtual_media_get_io_stats(&media_io) == ESP_OK;
        jetkvm_virtual_media_write_io_stats_t media_write_io = {0};
        bool have_media_write_io =
            jetkvm_virtual_media_get_write_io_stats(&media_write_io) == ESP_OK;
        jetkvm_virtual_media_upload_io_stats_t upload_io = {0};
        bool have_upload_io =
            jetkvm_virtual_media_get_upload_io_stats(&upload_io) == ESP_OK;
        jetkvm_virtual_media_sd_write_hint_stats_t sd_write_hint = {0};
        bool have_sd_write_hint =
            jetkvm_virtual_media_get_sd_write_hint_stats(&sd_write_hint) ==
            ESP_OK;
        cJSON *media_read = cJSON_AddObjectToObject(usb_hid, "mediaRead");
        if (media_read) {
            add_u64(media_read, "calls", have_media_io ? media_io.calls : 0);
            add_u64(media_read, "bytesRequested",
                    have_media_io ? media_io.bytes_requested : 0);
            add_u64(media_read, "bytesRead",
                    have_media_io ? media_io.bytes_read : 0);
            add_u64(media_read, "errors", have_media_io ? media_io.errors : 0);
            add_u64(media_read, "totalUs", have_media_io ? media_io.total_us : 0);
            add_u64(media_read, "lockWaitUs",
                    have_media_io ? media_io.lock_wait_us : 0);
            add_u64(media_read, "seekUs", have_media_io ? media_io.seek_us : 0);
            add_u64(media_read, "readUs", have_media_io ? media_io.read_us : 0);
            cJSON_AddNumberToObject(media_read, "maxUs",
                                    have_media_io ? media_io.max_us : 0);
            cJSON_AddNumberToObject(
                media_read, "maxLockWaitUs",
                have_media_io ? media_io.max_lock_wait_us : 0);
            cJSON_AddNumberToObject(media_read, "maxSeekUs",
                                    have_media_io ? media_io.max_seek_us : 0);
            cJSON_AddNumberToObject(media_read, "maxReadUs",
                                    have_media_io ? media_io.max_read_us : 0);
            cJSON *prefetch = cJSON_AddObjectToObject(media_read, "prefetch");
            if (prefetch) {
                add_u64(prefetch, "jobs",
                        have_media_io ? media_io.prefetch_jobs : 0);
                add_u64(prefetch, "hits",
                        have_media_io ? media_io.prefetch_hits : 0);
                add_u64(prefetch, "misses",
                        have_media_io ? media_io.prefetch_misses : 0);
                add_u64(prefetch, "hitBytes",
                        have_media_io ? media_io.prefetch_hit_bytes : 0);
                add_u64(prefetch, "missBytes",
                        have_media_io ? media_io.prefetch_miss_bytes : 0);
                add_u64(prefetch, "bytes",
                        have_media_io ? media_io.prefetch_bytes : 0);
                add_u64(prefetch, "totalUs",
                        have_media_io ? media_io.prefetch_total_us : 0);
                add_u64(prefetch, "seekUs",
                        have_media_io ? media_io.prefetch_seek_us : 0);
                add_u64(prefetch, "readUs",
                        have_media_io ? media_io.prefetch_read_us : 0);
                add_u64(prefetch, "copyUs",
                        have_media_io ? media_io.prefetch_copy_us : 0);
                add_u64(
                    prefetch, "scheduleDelayUs",
                    have_media_io ? media_io.prefetch_schedule_delay_us : 0);
                add_u64(
                    prefetch, "workerLockWaitUs",
                    have_media_io ?
                        media_io.prefetch_worker_lock_wait_us : 0);
                add_u64(prefetch, "readyAgeUs",
                        have_media_io ? media_io.prefetch_ready_age_us : 0);
                add_u64(prefetch, "errors",
                        have_media_io ? media_io.prefetch_errors : 0);
                cJSON_AddNumberToObject(
                    prefetch, "maxUs",
                    have_media_io ? media_io.prefetch_max_us : 0);
                cJSON_AddNumberToObject(
                    prefetch, "maxScheduleDelayUs",
                    have_media_io ?
                        media_io.prefetch_max_schedule_delay_us : 0);
                cJSON_AddNumberToObject(
                    prefetch, "maxWorkerLockWaitUs",
                    have_media_io ?
                        media_io.prefetch_max_worker_lock_wait_us : 0);
                cJSON_AddNumberToObject(
                    prefetch, "maxReadyAgeUs",
                    have_media_io ? media_io.prefetch_max_ready_age_us : 0);
                cJSON_AddNumberToObject(
                    prefetch, "stackHighWatermark",
                    have_media_io ? media_io.prefetch_stack_high_watermark : 0);
            }
        }
        cJSON *media_write =
            cJSON_AddObjectToObject(usb_hid, "mediaWrite");
        if (media_write) {
            add_u64(media_write, "calls",
                    have_media_write_io ? media_write_io.calls : 0);
            add_u64(media_write, "bytesRequested",
                    have_media_write_io ? media_write_io.bytes_requested : 0);
            add_u64(media_write, "bytesWritten",
                    have_media_write_io ? media_write_io.bytes_written : 0);
            add_u64(media_write, "errors",
                    have_media_write_io ? media_write_io.errors : 0);
            add_u64(media_write, "totalUs",
                    have_media_write_io ? media_write_io.total_us : 0);
            add_u64(media_write, "lockWaitUs",
                    have_media_write_io ? media_write_io.lock_wait_us : 0);
            add_u64(media_write, "seekUs",
                    have_media_write_io ? media_write_io.seek_us : 0);
            add_u64(media_write, "writeUs",
                    have_media_write_io ? media_write_io.write_us : 0);
            add_u64(media_write, "syncCalls",
                    have_media_write_io ? media_write_io.sync_calls : 0);
            add_u64(media_write, "syncErrors",
                    have_media_write_io ? media_write_io.sync_errors : 0);
            add_u64(media_write, "syncUs",
                    have_media_write_io ? media_write_io.sync_us : 0);
            cJSON_AddNumberToObject(
                media_write, "maxUs",
                have_media_write_io ? media_write_io.max_us : 0);
            cJSON_AddNumberToObject(
                media_write, "maxLockWaitUs",
                have_media_write_io ? media_write_io.max_lock_wait_us : 0);
            cJSON_AddNumberToObject(
                media_write, "maxSeekUs",
                have_media_write_io ? media_write_io.max_seek_us : 0);
            cJSON_AddNumberToObject(
                media_write, "maxWriteUs",
                have_media_write_io ? media_write_io.max_write_us : 0);
            cJSON_AddNumberToObject(
                media_write, "maxSyncUs",
                have_media_write_io ? media_write_io.max_sync_us : 0);
            cJSON_AddBoolToObject(
                media_write, "dirty",
                have_media_write_io && media_write_io.dirty);
        }
        cJSON *upload_write =
            cJSON_AddObjectToObject(usb_hid, "uploadWrite");
        if (upload_write) {
            add_u64(upload_write, "calls",
                    have_upload_io ? upload_io.calls : 0);
            add_u64(upload_write, "bytesRequested",
                    have_upload_io ? upload_io.bytes_requested : 0);
            add_u64(upload_write, "bytesWritten",
                    have_upload_io ? upload_io.bytes_written : 0);
            add_u64(upload_write, "errors",
                    have_upload_io ? upload_io.errors : 0);
            add_u64(upload_write, "totalUs",
                    have_upload_io ? upload_io.total_us : 0);
            add_u64(upload_write, "directCalls",
                    have_upload_io ? upload_io.direct_calls : 0);
            add_u64(upload_write, "directBytes",
                    have_upload_io ? upload_io.direct_bytes : 0);
            add_u64(upload_write, "directUs",
                    have_upload_io ? upload_io.direct_us : 0);
            add_u64(upload_write, "bounceCalls",
                    have_upload_io ? upload_io.bounce_calls : 0);
            add_u64(upload_write, "bounceBytes",
                    have_upload_io ? upload_io.bounce_bytes : 0);
            add_u64(upload_write, "bounceCopyUs",
                    have_upload_io ? upload_io.bounce_copy_us : 0);
            add_u64(upload_write, "bounceWriteUs",
                    have_upload_io ? upload_io.bounce_write_us : 0);
            add_u64(upload_write, "stagingAllocations",
                    have_upload_io ? upload_io.staging_allocations : 0);
            add_u64(
                upload_write, "stagingAllocationFailures",
                have_upload_io ? upload_io.staging_allocation_failures : 0);
            add_u64(upload_write, "stagingReleases",
                    have_upload_io ? upload_io.staging_releases : 0);
            cJSON_AddNumberToObject(
                upload_write, "stagingBytes",
                have_upload_io ? upload_io.staging_bytes : 0);
            cJSON_AddBoolToObject(
                upload_write, "stagingActive",
                have_upload_io && upload_io.staging_active);
        }
        cJSON *sd_write_hint_json =
            cJSON_AddObjectToObject(usb_hid, "sdWriteHint");
        if (sd_write_hint_json) {
            cJSON_AddBoolToObject(
                sd_write_hint_json, "enabled",
                have_sd_write_hint && sd_write_hint.enabled);
            cJSON_AddBoolToObject(
                sd_write_hint_json, "unsupportedLatched",
                have_sd_write_hint && sd_write_hint.unsupported_latched);
            add_u64(sd_write_hint_json, "attempts",
                    have_sd_write_hint ? sd_write_hint.attempts : 0);
            add_u64(sd_write_hint_json, "successes",
                    have_sd_write_hint ? sd_write_hint.successes : 0);
            add_u64(sd_write_hint_json, "unsupported",
                    have_sd_write_hint ? sd_write_hint.unsupported : 0);
            add_u64(sd_write_hint_json, "failures",
                    have_sd_write_hint ? sd_write_hint.failures : 0);
            cJSON_AddNumberToObject(
                sd_write_hint_json, "lastBlockCount",
                have_sd_write_hint ? sd_write_hint.last_block_count : 0);
            cJSON_AddNumberToObject(
                sd_write_hint_json, "lastError",
                have_sd_write_hint ? sd_write_hint.last_error :
                                     ESP_ERR_INVALID_STATE);
        }
        if (have_hid_stats) {
            add_u64(usb_hid, "submitted", hid_stats.submitted);
            add_u64(usb_hid, "enqueued", hid_stats.enqueued);
            add_u64(usb_hid, "processed", hid_stats.processed);
            add_u64(usb_hid, "staleGenerationDropped",
                    hid_stats.stale_generation_dropped);
            add_u64(usb_hid, "queueFullDropped",
                    hid_stats.queue_full_dropped);
            add_u64(usb_hid, "absoluteMoveDropped",
                    hid_stats.absolute_move_dropped);
            add_u64(usb_hid, "absoluteMoveBackpressureDropped",
                    hid_stats.absolute_move_backpressure_dropped);
            add_u64(usb_hid, "macroAccepted", hid_stats.macro_accepted);
            add_u64(usb_hid, "macroRejected", hid_stats.macro_rejected);
            add_u64(usb_hid, "macroCancelRequests",
                    hid_stats.macro_cancel_requests);
            add_u64(usb_hid, "macroCancelled", hid_stats.macro_cancelled);
            add_u64(usb_hid, "macroReplaced", hid_stats.macro_replaced);
            add_u64(usb_hid, "macroCompleted", hid_stats.macro_completed);
            add_u64(usb_hid, "macroFailed", hid_stats.macro_failed);
            add_u64(usb_hid, "usbErrors", hid_stats.usb_errors);
            add_u64(usb_hid, "usbReportRetries",
                    hid_stats.usb_report_retries);
            cJSON_AddNumberToObject(usb_hid, "keyboardWriteLastUs",
                                    hid_stats.keyboard_write_last_us);
            cJSON_AddNumberToObject(usb_hid, "keyboardWriteMaxUs",
                                    hid_stats.keyboard_write_max_us);
            add_u64(usb_hid, "keyboardWriteSlow", hid_stats.keyboard_write_slow);
            add_u64(usb_hid, "localRelativeMouseReports",
                    hid_stats.local_relative_mouse_reports);
            add_u64(usb_hid, "keepalives", hid_stats.keepalives);
            add_u64(usb_hid, "keepaliveRejectedLate",
                    hid_stats.keepalives_rejected_late);
            add_u64(usb_hid, "keepaliveRejectedStale",
                    hid_stats.keepalives_rejected_stale);
            add_u64(usb_hid, "keepaliveJitterAdjusted",
                    hid_stats.keepalives_jitter_adjusted);
            add_u64(usb_hid, "releaseRequests",
                    hid_stats.release_requests);
            add_u64(usb_hid, "releasesCompleted",
                    hid_stats.releases_completed);
            add_u64(usb_hid, "releaseRetries",
                    hid_stats.release_retries);
            add_u64(usb_hid, "releaseFailures",
                    hid_stats.release_failures);
            add_u64(usb_hid, "releaseBlockedDropped",
                    hid_stats.release_blocked_dropped);
            cJSON_AddBoolToObject(usb_hid, "releasePending",
                                  hid_stats.release_pending);
            cJSON_AddNumberToObject(usb_hid, "queueHighWatermark",
                                    hid_stats.queue_high_watermark);
        }
    }
    jetkvm_usb_service_snapshot_t usb_service_snapshot = {0};
    bool have_usb_service =
        jetkvm_usb_service_get_snapshot(&usb_service_snapshot) == ESP_OK;
    cJSON *usb_service = cJSON_AddObjectToObject(root, "usbService");
    if (usb_service) {
        cJSON_AddBoolToObject(
            usb_service, "initialized",
            have_usb_service && usb_service_snapshot.initialized);
        cJSON_AddBoolToObject(
            usb_service, "desiredEnabled",
            have_usb_service && usb_service_snapshot.desired_enabled);
        cJSON_AddBoolToObject(
            usb_service, "driverEnabled",
            have_usb_service && usb_service_snapshot.driver_enabled);
        cJSON_AddBoolToObject(
            usb_service, "hidInputReady",
            have_usb_service && usb_service_snapshot.hid_input_ready);
        cJSON_AddBoolToObject(
            usb_service, "transitioning",
            have_usb_service && usb_service_snapshot.transitioning);
        cJSON_AddBoolToObject(
            usb_service, "commandPending",
            have_usb_service && usb_service_snapshot.command_pending);
        cJSON_AddBoolToObject(
            usb_service, "configurationReady",
            have_usb_service && usb_service_snapshot.configuration_ready);
        cJSON_AddBoolToObject(
            usb_service, "configurationPending",
            have_usb_service && usb_service_snapshot.configuration_pending);
        cJSON_AddBoolToObject(
            usb_service, "configurationTransitioning",
            have_usb_service &&
                usb_service_snapshot.configuration_transitioning);
        cJSON_AddBoolToObject(
            usb_service, "configurationStoreDegraded",
            have_usb_service &&
                usb_service_snapshot.configuration_store_degraded);
        cJSON_AddNumberToObject(
            usb_service, "lifecycleGeneration",
            have_usb_service ? usb_service_snapshot.lifecycle_generation : 0);
        cJSON_AddNumberToObject(
            usb_service, "commandsAccepted",
            have_usb_service ? usb_service_snapshot.commands_accepted : 0);
        cJSON_AddNumberToObject(
            usb_service, "commandsReplaced",
            have_usb_service ? usb_service_snapshot.commands_replaced : 0);
        cJSON_AddNumberToObject(
            usb_service, "transitionsCompleted",
            have_usb_service ? usb_service_snapshot.transitions_completed : 0);
        cJSON_AddNumberToObject(
            usb_service, "transitionFailures",
            have_usb_service ? usb_service_snapshot.transition_failures : 0);
        cJSON_AddNumberToObject(
            usb_service, "configurationGeneration",
            have_usb_service ? usb_service_snapshot.configuration_generation : 0);
        cJSON_AddNumberToObject(
            usb_service, "configurationRequests",
            have_usb_service ? usb_service_snapshot.configuration_requests : 0);
        cJSON_AddNumberToObject(
            usb_service, "configurationTransactionsCompleted",
            have_usb_service ?
                usb_service_snapshot.configuration_transactions_completed : 0);
        cJSON_AddNumberToObject(
            usb_service, "configurationTransactionFailures",
            have_usb_service ?
                usb_service_snapshot.configuration_transaction_failures : 0);
        cJSON_AddNumberToObject(
            usb_service, "configurationRollbacks",
            have_usb_service ? usb_service_snapshot.configuration_rollbacks : 0);
        cJSON_AddNumberToObject(
            usb_service, "configurationRollbackFailures",
            have_usb_service ?
                usb_service_snapshot.configuration_rollback_failures : 0);
        cJSON_AddNumberToObject(
            usb_service, "configurationWaitTimeouts",
            have_usb_service ?
                usb_service_snapshot.configuration_wait_timeouts : 0);
        cJSON_AddNumberToObject(
            usb_service, "lastError",
            have_usb_service ? usb_service_snapshot.last_error :
                               ESP_ERR_INVALID_STATE);
        cJSON_AddNumberToObject(
            usb_service, "configurationLoadError",
            have_usb_service ? usb_service_snapshot.configuration_load_error :
                               ESP_ERR_INVALID_STATE);
        cJSON_AddNumberToObject(
            usb_service, "lastConfigurationError",
            have_usb_service ?
                usb_service_snapshot.last_configuration_error :
                ESP_ERR_INVALID_STATE);
    }
    add_stage_age_summary(root, &stats.stage_age);
    add_network_timing(root, &stats.network_timing);
    jetkvm_ethernet_rx_stats_t ethernet_rx_stats = {0};
    jetkvm_ethernet_rx_stats_snapshot(&ethernet_rx_stats);
    cJSON *ethernet_rx = cJSON_AddObjectToObject(root, "ethernetRx");
    if (ethernet_rx) {
        cJSON_AddStringToObject(ethernet_rx, "counterScope", "sinceBoot");
        cJSON_AddNumberToObject(ethernet_rx, "counterWidthBits", 32);
        cJSON_AddNumberToObject(ethernet_rx, "emacDmaRxBuffers",
                                CONFIG_ETH_DMA_RX_BUFFER_NUM);
        cJSON_AddNumberToObject(ethernet_rx, "tcpipMboxCapacity",
                                CONFIG_LWIP_TCPIP_RECVMBOX_SIZE);
        cJSON_AddNumberToObject(ethernet_rx, "tcpMboxCapacityPerSocket",
                                CONFIG_LWIP_TCP_RECVMBOX_SIZE);
        cJSON_AddNumberToObject(ethernet_rx, "tcpReceiveWindowBytes",
                                CONFIG_LWIP_TCP_WND_DEFAULT);
        cJSON_AddNumberToObject(ethernet_rx, "emacRxTaskPriority",
                                ethernet_rx_stats.emac_rx_task_priority);
        cJSON_AddNumberToObject(ethernet_rx, "tcpipTaskPriority",
                                CONFIG_LWIP_TCPIP_TASK_PRIO);
        cJSON_AddNumberToObject(ethernet_rx, "netifInputCalls",
                                ethernet_rx_stats.netif_input_calls);
        cJSON_AddNumberToObject(ethernet_rx, "netifInputAccepted",
                                ethernet_rx_stats.netif_input_accepted);
        cJSON_AddNumberToObject(
            ethernet_rx, "pbufAllocationFailures",
            ethernet_rx_stats.pbuf_allocation_failures);
        cJSON_AddNumberToObject(
            ethernet_rx, "tcpipEnqueueFailures",
            ethernet_rx_stats.tcpip_enqueue_failures);
        cJSON_AddNumberToObject(ethernet_rx, "netifDownDrops",
                                ethernet_rx_stats.netif_down_drops);
        cJSON_AddNumberToObject(ethernet_rx, "invalidInputDrops",
                                ethernet_rx_stats.invalid_input_drops);
        cJSON_AddNumberToObject(ethernet_rx, "otherInputErrors",
                                ethernet_rx_stats.other_input_errors);
    }
    cJSON_AddNumberToObject(root, "internalHeapFree", stats.internal_heap_free);
    cJSON_AddNumberToObject(root, "internalHeapMinimum", stats.internal_heap_minimum);
    cJSON_AddNumberToObject(root, "internalHeapLargest", stats.internal_heap_largest);
    cJSON_AddNumberToObject(root, "internalDmaHeapFree",
                            stats.internal_dma_heap_free);
    cJSON_AddNumberToObject(root, "internalDmaHeapMinimum",
                            stats.internal_dma_heap_minimum);
    cJSON_AddNumberToObject(root, "internalDmaHeapLargest",
                            stats.internal_dma_heap_largest);
    cJSON_AddNumberToObject(root, "psramFree", stats.psram_free);
    cJSON_AddNumberToObject(root, "psramMinimum", stats.psram_minimum);
    cJSON_AddNumberToObject(root, "psramLargest", stats.psram_largest);
    jetkvm_memory_policy_snapshot_t memory_policy = {0};
    if (detailed) {
        jetkvm_memory_policy_get_snapshot(&memory_policy);
    } else {
        jetkvm_memory_policy_get_snapshot_cached(&memory_policy);
    }
    cJSON *memory_budget = cJSON_AddObjectToObject(root, "memoryBudget");
    if (memory_budget) {
        add_heap_detail_source(memory_budget, "heapDetails", !detailed,
                                memory_policy.detailed_sample_timestamp_us);
        cJSON_AddNumberToObject(
            memory_budget, "internalHardFloor",
            JETKVM_MEMORY_INTERNAL_HARD_FLOOR_BYTES);
        cJSON_AddNumberToObject(
            memory_budget, "internalAdmissionFloor",
            JETKVM_MEMORY_INTERNAL_ADMISSION_FLOOR_BYTES);
        cJSON_AddNumberToObject(memory_budget, "dmaHardFloor",
                                JETKVM_MEMORY_DMA_HARD_FLOOR_BYTES);
        cJSON_AddNumberToObject(memory_budget, "dmaAdmissionFloor",
                                JETKVM_MEMORY_DMA_ADMISSION_FLOOR_BYTES);
        cJSON_AddNumberToObject(memory_budget, "psramHardFloor",
                                JETKVM_MEMORY_PSRAM_HARD_FLOOR_BYTES);
        cJSON_AddNumberToObject(memory_budget, "psramAdmissionFloor",
                                JETKVM_MEMORY_PSRAM_ADMISSION_FLOOR_BYTES);
        cJSON_AddNumberToObject(
            memory_budget, "tcpipMboxPointerStorageBytes",
            CONFIG_LWIP_TCPIP_RECVMBOX_SIZE *
                JETKVM_MEMORY_LWIP_MBOX_POINTER_BYTES);
        cJSON_AddNumberToObject(
            memory_budget, "tcpMboxPointerStoragePerSocketBytes",
            CONFIG_LWIP_TCP_RECVMBOX_SIZE *
                JETKVM_MEMORY_LWIP_MBOX_POINTER_BYTES);
        cJSON_AddNumberToObject(
            memory_budget, "tcpMboxPointerStorageAllSocketsMaximumBytes",
            CONFIG_LWIP_TCP_RECVMBOX_SIZE *
                JETKVM_MEMORY_LWIP_MBOX_POINTER_BYTES *
                CONFIG_LWIP_MAX_SOCKETS);
        /* This is a demand-allocated payload upper bound, not boot-time
         * reserved memory. Non-packet messages consume slots without payload. */
        cJSON_AddNumberToObject(
            memory_budget, "tcpipMboxQueuedEthernetPayloadMaximumBytes",
            CONFIG_LWIP_TCPIP_RECVMBOX_SIZE *
                JETKVM_MEMORY_ETHERNET_MAX_FRAME_BYTES);
        cJSON_AddNumberToObject(
            memory_budget, "storageIngressTotalMaximum",
            JETKVM_MEMORY_STORAGE_INGRESS_TOTAL_MAX_BYTES);
        cJSON_AddNumberToObject(
            memory_budget, "rtcWriterPoolBytes",
            JETKVM_MEMORY_RTC_SHARED_POOL_BYTES);
        cJSON_AddNumberToObject(
            memory_budget, "rtcSharedPoolBytes",
            JETKVM_MEMORY_RTC_SHARED_POOL_BYTES);
        cJSON_AddNumberToObject(
            memory_budget, "httpWriterPoolBytes",
            JETKVM_MEMORY_HTTP_WRITER_POOL_BYTES);
        cJSON_AddNumberToObject(
            memory_budget, "httpTcpReceiveMaximumBytes",
            JETKVM_MEMORY_HTTP_TCP_RECEIVE_MAX_BYTES);
        cJSON_AddNumberToObject(
            memory_budget, "httpIngressMaximumBytes",
            JETKVM_MEMORY_HTTP_INGRESS_MAX_BYTES);
        cJSON_AddNumberToObject(
            memory_budget, "sctpReceiveMaximum",
            JETKVM_MEMORY_SCTP_RECEIVE_MAX_BYTES);
        cJSON_AddNumberToObject(
            memory_budget, "rtcProtocolMetadataMaximum",
            JETKVM_MEMORY_RTC_PROTOCOL_METADATA_MAX_BYTES);
        cJSON_AddNumberToObject(
            memory_budget, "rtcIngressHeadroom",
            JETKVM_MEMORY_RTC_INGRESS_HEADROOM_BYTES);
        cJSON_AddBoolToObject(
            memory_budget, "videoRebuildActive",
            memory_policy.video_rebuild_active);
        cJSON_AddNumberToObject(
            memory_budget, "videoRebuildWorkingSetBytes",
            memory_policy.video_rebuild_working_set_bytes);
        cJSON_AddNumberToObject(
            memory_budget, "videoRebuildBeginDenials",
            memory_policy.video_rebuild_begin_denials);
        cJSON_AddNumberToObject(
            memory_budget, "videoRebuildFailures",
            memory_policy.video_rebuild_failures);
        cJSON_AddNumberToObject(
            memory_budget, "videoRebuildBlockedMaterializations",
            memory_policy.video_rebuild_blocked_materializations);
        cJSON_AddNumberToObject(
            memory_budget, "largeInternalAllocationLimit",
            JETKVM_MEMORY_LARGE_INTERNAL_LIMIT_BYTES);
        cJSON_AddNumberToObject(
            memory_budget, "jsonAllocationFailures",
            memory_policy.json_allocation_failures);
        cJSON_AddNumberToObject(
            memory_budget, "controlAllocationFailures",
            memory_policy.control_allocation_failures);
        cJSON_AddNumberToObject(memory_budget, "admissionDenials",
                                memory_policy.admission_denials);
        cJSON_AddNumberToObject(
            memory_budget, "controlArenaBytes",
            JETKVM_MEMORY_CONTROL_ARENA_BYTES);
        cJSON_AddNumberToObject(
            memory_budget, "policyArenasTotalBytes",
            JETKVM_MEMORY_POLICY_ARENAS_TOTAL_BYTES);
        cJSON_AddNumberToObject(
            memory_budget, "lvglArenaBytes",
            JETKVM_MEMORY_LVGL_ARENA_BYTES);
        cJSON_AddNumberToObject(
            memory_budget, "controlCloudQueueMaximum",
            JETKVM_MEMORY_CONTROL_CLOUD_QUEUE_MAX_BYTES);
        cJSON_AddNumberToObject(
            memory_budget, "controlSignalingMaximum",
            JETKVM_MEMORY_CONTROL_SIGNALING_MAX_BYTES);
        cJSON_AddNumberToObject(
            memory_budget, "controlRuntimeHeadroom",
            JETKVM_MEMORY_CONTROL_RUNTIME_HEADROOM_BYTES);
        /* What each kind of TLS reservation takes on this board. */
        cJSON_AddNumberToObject(
            memory_budget, "tlsConnectionReserve",
            JETKVM_MEMORY_TLS_CONNECTION_RESERVE_BYTES +
                JETKVM_MEMORY_TLS_LINK_RESERVE_BYTES);
        cJSON_AddNumberToObject(
            memory_budget, "tlsSessionReserve",
            JETKVM_MEMORY_TLS_SESSION_RESERVE_BYTES +
                JETKVM_MEMORY_TLS_LINK_RESERVE_BYTES);
        cJSON_AddNumberToObject(
            memory_budget, "tlsSessionIceReserve",
            JETKVM_MEMORY_TLS_SESSION_ICE_RESERVE_BYTES +
                JETKVM_MEMORY_TLS_LINK_RESERVE_BYTES);
        cJSON_AddNumberToObject(
            memory_budget, "tlsRequestReserve",
            JETKVM_MEMORY_TLS_REQUEST_RESERVE_BYTES +
                JETKVM_MEMORY_TLS_LINK_RESERVE_BYTES);
        cJSON_AddNumberToObject(
            memory_budget, "downloadLinkReserve",
            JETKVM_MEMORY_DOWNLOAD_LINK_RESERVE_BYTES);
        cJSON_AddNumberToObject(
            memory_budget, "rpcScratchBytes",
            JETKVM_MEMORY_RPC_SCRATCH_BYTES);
        cJSON_AddNumberToObject(
            memory_budget, "controlArenaFree",
            memory_policy.control_arena_free);
        cJSON_AddNumberToObject(
            memory_budget, "controlArenaMinimum",
            memory_policy.control_arena_minimum);
        cJSON_AddNumberToObject(
            memory_budget, "controlArenaLargest",
            memory_policy.control_arena_largest);
        cJSON_AddNumberToObject(
            memory_budget, "lvglArenaCapacity",
            memory_policy.lvgl_arena_capacity);
        cJSON_AddNumberToObject(
            memory_budget, "lvglArenaUsed",
            memory_policy.lvgl_arena_used);
        cJSON_AddNumberToObject(
            memory_budget, "lvglArenaPeakUsed",
            memory_policy.lvgl_arena_peak_used);
        cJSON_AddNumberToObject(
            memory_budget, "lvglArenaFree",
            memory_policy.lvgl_arena_free);
        cJSON_AddNumberToObject(
            memory_budget, "lvglArenaMinimum",
            memory_policy.lvgl_arena_minimum);
        cJSON_AddNumberToObject(
            memory_budget, "lvglArenaLargest",
            memory_policy.lvgl_arena_largest);
        cJSON_AddNumberToObject(
            memory_budget, "lvglAllocatedBlocks",
            memory_policy.lvgl_allocated_blocks);
        cJSON_AddNumberToObject(
            memory_budget, "lvglFreeBlocks",
            memory_policy.lvgl_free_blocks);
        cJSON_AddNumberToObject(
            memory_budget, "lvglAllocationFailures",
            memory_policy.lvgl_allocation_failures);
        cJSON_AddNumberToObject(
            memory_budget, "activeReservations",
            memory_policy.active_reservations);
        cJSON_AddNumberToObject(
            memory_budget, "reservationSlotCapacity",
            JETKVM_MEMORY_RESERVATION_SLOTS);
        cJSON_AddNumberToObject(
            memory_budget, "reservedInternalBytes",
            memory_policy.reservation_internal_bytes);
        cJSON_AddNumberToObject(
            memory_budget, "reservedDmaBytes",
            memory_policy.reservation_dma_bytes);
        cJSON_AddNumberToObject(
            memory_budget, "reservedPsramBytes",
            memory_policy.reservation_psram_bytes);
        cJSON_AddNumberToObject(
            memory_budget, "reservedStorageIngressBytes",
            memory_policy.reservation_storage_ingress_bytes);
        cJSON_AddNumberToObject(
            memory_budget, "storageIngressRemainingBytes",
            memory_policy.reservation_storage_ingress_bytes <=
                    JETKVM_MEMORY_STORAGE_INGRESS_TOTAL_MAX_BYTES ?
                JETKVM_MEMORY_STORAGE_INGRESS_TOTAL_MAX_BYTES -
                    memory_policy.reservation_storage_ingress_bytes :
                0U);
        cJSON_AddNumberToObject(
            memory_budget, "activeStorageIngressReservations",
            memory_policy.active_storage_ingress_reservations);
        cJSON_AddNumberToObject(
            memory_budget, "activeTlsReservations",
            memory_policy.active_tls_reservations);
        cJSON_AddNumberToObject(
            memory_budget, "reservedTlsInternalBytes",
            memory_policy.tls_reservation_internal_bytes);
        cJSON_AddNumberToObject(
            memory_budget, "reservedTlsDmaBytes",
            memory_policy.tls_reservation_dma_bytes);
        add_tls_admissions(memory_budget);
    }
    cJSON_AddNumberToObject(root, "peerTaskStackHighWatermark",
                            stats.peer_task_stack_high_watermark);
    add_task_stack_budgets(root, detailed);
    jetkvm_screen_stats_t screen;
    cJSON *display = cJSON_AddObjectToObject(root, "localDisplay");
    if (display && jetkvm_screen_get_stats(&screen) == ESP_OK) {
        cJSON_AddBoolToObject(display, "ready", screen.initialized);
        cJSON_AddBoolToObject(display, "active", screen.active);
        cJSON_AddNumberToObject(display, "rotation", screen.display_rotation);
        cJSON_AddNumberToObject(display, "brightness", screen.backlight_brightness);
        cJSON_AddNumberToObject(display, "pwmDuty", screen.backlight_duty);
        cJSON_AddNumberToObject(display, "lastError", screen.display_settings_error);
        cJSON_AddStringToObject(display, "wifiFailureShown", screen.wifi_failure_shown);
        cJSON_AddNumberToObject(display, "wifiFailureShows", screen.wifi_failure_shows);
    }
    add_firmware_identity(root);
    if (detailed) {
        add_flash_read_mode(root);
    }
    add_cloud_link(root);
    add_guest_video(root);
    add_video_encode(root);
    add_hdmi_receiver(root);
    add_audio(root);
    *root_out = root;
    return ESP_OK;
}

esp_err_t webrtc_stats_json_create(cJSON **root_out)
{
    return stats_json_create(root_out, false);
}

esp_err_t webrtc_stats_json_create_detailed(cJSON **root_out)
{
    return stats_json_create(root_out, true);
}

esp_err_t webrtc_stats_json_create_bitrate(cJSON **root_out)
{
    if (root_out == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    *root_out = NULL;
    webrtc_transport_stats_t stats = { 0 };
    esp_err_t error = webrtc_transport_get_stats(&stats);
    if (error != ESP_OK) {
        return error;
    }
    cJSON *root = cJSON_CreateObject();
    if (root == NULL) {
        return ESP_ERR_NO_MEM;
    }
    /* The controller's stall input; NoSlot is reported separately. */
    const uint64_t stalls = stats.access_units_queue_full +
                            stats.access_units_would_block + stats.access_units_send_errors;
    add_u64(root, "nowMs", (uint64_t)(esp_timer_get_time() / 1000));
    add_u64(root, "videoBitrateStepMs", (uint64_t)stats.video_bitrate_step_ms);
    add_u64(root, "videoBitrateStepBytes", stats.video_bitrate_step_bytes);
    add_u64(root, "videoBitrateStepStalls", stats.video_bitrate_step_stalls);
    add_u64(root, "videoBitrateStepSlotDrops", stats.video_bitrate_step_slot_drops);
    cJSON_AddNumberToObject(root, "videoBitrateStepRemb", stats.video_bitrate_step_remb);
    cJSON_AddNumberToObject(root, "videoBitrateStepReports", stats.video_bitrate_step_reports);
    cJSON_AddNumberToObject(root, "videoBitrateStepDecreases", stats.video_bitrate_step_decreases);
    cJSON_AddNumberToObject(root, "videoBitrateStepLost", stats.video_bitrate_step_lost);
    cJSON_AddNumberToObject(root, "videoBitrateStepSequence", stats.video_bitrate_step_sequence);
    cJSON_AddNumberToObject(root, "videoBitrateStepLossReports", stats.video_bitrate_step_loss_reports);
    cJSON_AddNumberToObject(root, "videoBitrateLossPermille", stats.video_bitrate_loss_permille);
    cJSON_AddNumberToObject(root, "videoRtcpLossReports", stats.video_rtcp_loss_reports);
    cJSON_AddNumberToObject(root, "videoRtcpCumulativeLost", stats.video_rtcp_cumulative_lost);
    cJSON_AddNumberToObject(root, "videoRtcpHighestSequence", stats.video_rtcp_highest_sequence);
    add_u64(root, "accessUnitsNoSlot", stats.access_units_no_slot);
    cJSON_AddBoolToObject(root, "peerConnected", stats.peer_connected);
    add_paused_streams(root, stats.video_pause_sources);
    cJSON_AddNumberToObject(root, "generation", stats.generation);
    cJSON_AddNumberToObject(root, "videoTargetBitrate", stats.video_target_bitrate);
    cJSON_AddNumberToObject(root, "videoBitrateCeiling", stats.video_bitrate_ceiling);
    cJSON_AddNumberToObject(root, "videoRembBitrate", stats.video_remb_bitrate);
    cJSON_AddNumberToObject(root, "videoRembReports", stats.video_remb_reports);
    cJSON_AddNumberToObject(root, "videoRembDecreases", stats.video_remb_decreases);
    cJSON_AddNumberToObject(root, "videoAchievedBitrate", stats.video_achieved_bitrate);
    cJSON_AddStringToObject(root, "videoBitrateReason",
                            stats.video_bitrate_reason ? stats.video_bitrate_reason : "hold");
    cJSON_AddNumberToObject(root, "videoRembCuts", stats.video_remb_cuts);
    cJSON_AddNumberToObject(root, "videoRembIgnored", stats.video_remb_ignored);
    cJSON_AddNumberToObject(root, "videoBitrateRestores", stats.video_bitrate_restores);
    add_u64(root, "accessUnitsSent", stats.access_units_sent);
    add_u64(root, "accessUnitBytesSent", stats.access_unit_bytes_sent);
    add_u64(root, "accessUnitsStalled", stalls);
    add_u64(root, "syncIdrIssued", stats.sync_idr_issued);
    /* The video mode, which a poll once per second compares with what the
     * Mini publishes over MQTT. */
    cJSON_AddBoolToObject(root, "modeValid", stats.mode_valid);
    cJSON_AddNumberToObject(root, "width", stats.width);
    cJSON_AddNumberToObject(root, "height", stats.height);
    cJSON_AddNumberToObject(root, "fps", stats.fps);
    add_u64(root, "accessUnitsOversize", stats.access_units_oversize);
    cJSON_AddNumberToObject(root, "internalDmaHeapMinimum", stats.internal_dma_heap_minimum);
    cJSON_AddNumberToObject(root, "psramMinimum", stats.psram_minimum);
    add_guest_video(root);
    add_video_encode(root);
    add_hdmi_receiver(root);
    add_audio(root);
    static const char *const phase_names[WEBRTC_PEER_PHASE_COUNT] = {
        "wait", "control", "media", "mainLoop", "rpc",
    };
    cJSON *loop = cJSON_AddObjectToObject(root, "peerLoop");
    for (int phase = 0; loop && phase <= WEBRTC_PEER_PHASE_COUNT; ++phase) {
        add_duration_hist(loop, phase < WEBRTC_PEER_PHASE_COUNT ? phase_names[phase] : "auTakeWait",
                          phase < WEBRTC_PEER_PHASE_COUNT ? &stats.peer_phase[phase] :
                                                            &stats.au_take_wait);
    }
    add_peer_pass(root, &stats.peer_pass);
    add_idle_runs(root, stats.idle_runs, stats.idle_counted);
    /* The full view's networkTiming has the rest. */
    cJSON *network = cJSON_AddObjectToObject(root, "networkTiming");
    if (network) {
        add_u64(network, "rtpRetransmits", stats.network_timing.rtp_retransmits);
    }
    /* Since the last /webrtc/stats?bitrate=1&resetVideoIntervals=1 */
    add_duration_hist(root, "videoEncodeInterval", &stats.video_encode_interval);
    add_duration_hist(root, "videoSendInterval", &stats.video_send_interval);
    /* Per AU size range: [count, bytes, packets, total us, SRTP us, sendto us, max us] */
    cJSON *send = cJSON_AddArrayToObject(root, "videoSendBySize");
    for (int i = 0; send && i < WEBRTC_TRANSPORT_SEND_SIZE_BUCKETS; ++i) {
        const webrtc_transport_send_cost_t *c = &stats.video_send_by_size[i];
        const double values[] = { c->count, (double)c->bytes, (double)c->packets,
                                  (double)c->total_us, (double)c->srtp_us,
                                  (double)c->sendto_us, c->max_us };
        cJSON_AddItemToArray(send, cJSON_CreateDoubleArray(values, 7));
    }

    *root_out = root;
    return ESP_OK;
}
