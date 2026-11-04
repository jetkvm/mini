#include "jetkvm_audio.h"
#include "jetkvm_control.h"
#include "jetkvm_memory_policy.h"
#include "esp_heap_caps.h"
#include "esp_opus_enc.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "nvs.h"
#include "tusb.h"
#include <string.h>
#include <stdlib.h>

/* One frame of 20 ms as USB brings it, 48 kHz stereo, and as the encoder
 * gets it, 48 kHz mono. */
#define PCM_BYTES (960 * 2 * sizeof(int16_t))
#define MONO_BYTES (960 * sizeof(int16_t))
#define PACKET_SLOTS JETKVM_AUDIO_PACKET_SLOTS

/* The worker owns codec and scratch buffers; the mutex protects USB lifetime,
 * preferences and the bounded output queue. Encoding never holds this mutex. */
typedef struct { uint32_t pts; int size; uint8_t data[JETKVM_AUDIO_PACKET_MAX]; } packet_t;
static SemaphoreHandle_t s_mutex;
static TaskHandle_t s_task;
static void *s_encoder;
static uint8_t *s_pcm, *s_encoded;
static packet_t *s_packets;
static int s_encoded_capacity;
static bool s_enabled, s_usb_ready, s_peer_connected, s_streaming, s_usb_paused, s_saving;
static jetkvm_audio_stats_t s_stats;
static uint32_t s_generation;
static unsigned s_read, s_count;
static void invalidate(void) { ++s_generation; s_read = s_count = 0; }
static bool lock(void) { return s_mutex && xSemaphoreTake(s_mutex, pdMS_TO_TICKS(1000)) == pdTRUE; }
static void unlock(void) { xSemaphoreGive(s_mutex); }

static bool get_enabled(bool *enabled, void *ctx)
{
    (void)ctx;
    if (!enabled || !lock()) return false;
    *enabled = s_enabled && s_usb_ready && !s_usb_paused && s_task;
    unlock(); return true;
}
static const char *set_enabled(bool enabled, void *ctx)
{
    (void)ctx;
    if (!lock()) return "Audio unavailable or busy";
    if (s_usb_paused) { unlock(); return "USB configuration change in progress"; }
    if (s_saving) { unlock(); return "Audio settings change in progress"; }
    if (enabled && (!s_usb_ready || !s_task)) {
        unlock(); return "USB audio class not active";
    }
    s_saving = true;
    unlock(); /* Flash writes must not block the worker's USB FIFO reads. */
    nvs_handle_t handle;
    esp_err_t error = nvs_open("jk_audio", NVS_READWRITE, &handle);
    if (error == ESP_OK) {
        error = nvs_set_u8(handle, "enabled", enabled);
        if (error == ESP_OK) error = nvs_commit(handle);
        nvs_close(handle);
    }
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    if (error == ESP_OK) { s_enabled = enabled; invalidate(); }
    s_saving = false;
    unlock(); return error == ESP_OK ? NULL : "Could not save audio settings";
}

static void queue_packet(uint32_t generation, const uint8_t *data, size_t size, uint32_t pts)
{
    if (!size || size > JETKVM_AUDIO_PACKET_MAX || !lock()) return;
    if (generation == s_generation && s_usb_ready && !s_usb_paused && s_enabled && s_peer_connected && s_streaming) {
        /* Drop oldest on congestion: never build a latency backlog. */
        if (s_count == PACKET_SLOTS) {
            ++s_stats.dropped_frames; s_read = (s_read + 1) % PACKET_SLOTS; --s_count;
        }
        packet_t *packet = &s_packets[(s_read + s_count++) % PACKET_SLOTS];
        packet->pts = pts; packet->size = size;
        memcpy(packet->data, data, size);
    }
    unlock();
}

/* USB samples and esp_timer need not share a clock. Anchor once per continuous
 * capture segment; accumulated clock drift must not create an RTP sample gap.
 * The reader starts a new segment only after overflow or a long read gap. */
static uint32_t capture_pts(uint32_t *next, bool *started, bool *anchored, uint32_t now_ms,
                            unsigned backlog_bytes)
{
    uint32_t captured = now_ms - 20 - backlog_bytes / 192;
    if (!*started) {
        /* A recovered segment may advance the sample clock, but must not
         * regress it after negative drift. Compare across millisecond wrap;
         * a new generation can establish a completely fresh anchor. */
        if (!*anchored || (uint32_t)(captured - *next) < (UINT32_C(1) << 31)) {
            *next = captured;
        }
        *anchored = true;
        *started = true;
    }
    uint32_t pts = *next;
    *next += 20;
    return pts;
}

static bool capture_discontinuous(bool capture_started, bool fifo_overflowed,
                                   uint32_t now_ms, uint32_t last_read_ms)
{
    /* The existing USB FIFO holds about 42 ms. Recover before another whole
     * FIFO can arrive and TinyUSB normalizes a double overflow back to full. */
    return capture_started &&
           (fifo_overflowed || (uint32_t)(now_ms - last_read_ms) > 40);
}

static bool capture_read_skipped(uint16_t before, uint16_t after,
                                 uint16_t count, uint16_t depth)
{
    /* TinyUSB's sole reader owns rd_idx; read_n advances it further when it
     * corrects an overwritten FIFO. This also catches overflow between the
     * pre-read check and read_n. Audio's FIFO item size is one byte. */
    return depth != 0 && after != (before + (uint32_t)count) % (2U * depth);
}

/* The host plays stereo into the USB audio function; the browser gets mono.
 * Each sample of the frame becomes the mean of its left and its right
 * sample, in place: the mono frame is the first half of the buffer. Sound
 * that is the same on both channels keeps its level, sound on one channel
 * alone comes out at half its amplitude, and the mean of two samples cannot
 * overflow. */
static void downmix_to_mono(uint8_t *pcm, size_t stereo_bytes)
{
    for (size_t i = 0; i < stereo_bytes / (2 * sizeof(int16_t)); ++i) {
        int16_t left, right;
        memcpy(&left, pcm + i * 2 * sizeof(int16_t), sizeof(left));
        memcpy(&right, pcm + i * 2 * sizeof(int16_t) + sizeof(int16_t), sizeof(right));
        const int16_t mono = (int16_t)(((int32_t)left + right) / 2);
        memcpy(pcm + i * sizeof(int16_t), &mono, sizeof(mono));
    }
}

static void audio_task(void *arg)
{
    (void)arg;
    uint32_t generation = UINT32_MAX;
    size_t used = 0;
    uint32_t next_pts = 0;
    uint32_t last_read_ms = 0;
    bool clock_started = false;
    bool clock_anchored = false;
    unsigned encoded_burst = 0;
    for (;;) {
        bool encode = false;
        unsigned backlog_bytes = 0;
        uint32_t captured_at_ms = 0;
        if (lock()) {
            if (generation != s_generation) {
                generation = s_generation; used = 0; clock_started = false;
                clock_anchored = false;
                esp_opus_enc_reset(s_encoder);
            }
            if (s_usb_ready && !s_usb_paused) {
                bool enabled = s_enabled && s_peer_connected && s_streaming;
                if (!enabled) used = 0;
                uint32_t read_at_ms = (uint32_t)(esp_timer_get_time() / 1000);
                tu_fifo_t *fifo = tud_audio_get_ep_out_ff();
                if (enabled && capture_discontinuous(
                        clock_started || used != 0,
                        fifo && tu_fifo_overflowed(fifo),
                        read_at_ms, last_read_ms)) {
                    /* Never join an old partial frame to a recovered segment. */
                    used = 0;
                    clock_started = false;
                }
                /* Also drain disabled audio so re-enabling cannot replay it. */
                uint16_t read_index = fifo ? fifo->rd_idx : 0;
                uint16_t count = tud_audio_read(s_pcm + used, PCM_BYTES - used);
                if (count != 0) last_read_ms = read_at_ms;
                s_stats.pcm_bytes += count;
                if (enabled) {
                    if (fifo && capture_read_skipped(
                            read_index, fifo->rd_idx, count, fifo->depth)) {
                        /* Drop the uncertain chunk and any older partial
                         * frame. Resume with a complete fresh capture. */
                        used = 0;
                        count = 0;
                        clock_started = false;
                    }
                    used += count; encode = used == PCM_BYTES;
                    backlog_bytes = tud_audio_available();
                    captured_at_ms = (uint32_t)(esp_timer_get_time() / 1000);
                }
            } else used = 0;
            unlock();
        }
        if (encode) {
            downmix_to_mono(s_pcm, PCM_BYTES);
            esp_audio_enc_in_frame_t input = {.buffer = s_pcm, .len = MONO_BYTES};
            esp_audio_enc_out_frame_t output = {.buffer = s_encoded, .len = s_encoded_capacity};
            uint32_t pts = capture_pts(&next_pts, &clock_started, &clock_anchored,
                                       captured_at_ms, backlog_bytes);
            esp_audio_err_t error = esp_opus_enc_process(s_encoder, &input, &output);
            used = 0;
            __atomic_fetch_add(error == ESP_AUDIO_ERR_OK ? &s_stats.encoded_frames : &s_stats.encode_errors, 1, __ATOMIC_RELAXED);
            if (error == ESP_AUDIO_ERR_OK) {
                queue_packet(generation, s_encoded, output.encoded_bytes, pts);
            }
        }
        /* A mandatory 10 ms tick after every encode throttles capture when
         * video preempts this worker. Drain up to two complete frames before
         * sleeping; still give lower-priority tasks a bounded scheduling turn. */
        if (!encode || ++encoded_burst >= 2) {
            encoded_burst = 0;
            vTaskDelay(1);
        } else taskYIELD();
    }
}

esp_err_t jetkvm_audio_init(void)
{
    if (s_mutex) return ESP_OK;
    s_mutex = xSemaphoreCreateMutex();
    if (!s_mutex) return ESP_ERR_NO_MEM;
    nvs_handle_t handle;
    if (nvs_open("jk_audio", NVS_READONLY, &handle) == ESP_OK) {
        uint8_t enabled = 0;
        if (nvs_get_u8(handle, "enabled", &enabled) == ESP_OK) s_enabled = enabled == 1;
        nvs_close(handle);
    }
    const jetkvm_audio_context_t context = {.get_enabled = get_enabled, .set_enabled = set_enabled};
    jetkvm_control_set_default_audio_context(&context);
    return ESP_OK;
}

esp_err_t jetkvm_audio_prepare_usb(void)
{
    if (!lock()) return ESP_ERR_INVALID_STATE;
    if (s_task) { unlock(); return ESP_OK; }
    jetkvm_memory_policy_reservation_t reservation = {0};
    const jetkvm_memory_policy_request_t request = {
        .internal_bytes = 8192, .psram_bytes = 256 * 1024, .psram_largest_block = 128 * 1024,
    };
    if (!jetkvm_memory_policy_reserve("USB audio", &request, &reservation)) {
        unlock(); return ESP_ERR_NO_MEM;
    }
    esp_opus_enc_config_t config = ESP_OPUS_ENC_CONFIG_DEFAULT();
    config.sample_rate = 48000; config.channel = 1; config.bits_per_sample = 16;
    config.bitrate = 64000; config.frame_duration = ESP_OPUS_ENC_FRAME_DURATION_20_MS;
    config.application_mode = ESP_OPUS_ENC_APPLICATION_AUDIO; config.complexity = 3;
    int input_size = 0;
    esp_err_t error = ESP_ERR_NO_MEM;
    if (esp_opus_enc_open(&config, sizeof(config), &s_encoder) != ESP_AUDIO_ERR_OK) goto done;
    if (esp_opus_enc_get_frame_size(s_encoder, &input_size, &s_encoded_capacity) != ESP_AUDIO_ERR_OK ||
        input_size != MONO_BYTES || s_encoded_capacity <= 0 || s_encoded_capacity > 4096) {
        error = ESP_ERR_INVALID_SIZE; goto done;
    }
    s_pcm = heap_caps_malloc(PCM_BYTES, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    s_encoded = heap_caps_malloc(s_encoded_capacity, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    s_packets = heap_caps_calloc(PACKET_SLOTS, sizeof(packet_t), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!s_pcm || !s_encoded || !s_packets) goto done;
    /* The H.264 owner occupies CPU1. On CPU0, audio must preempt HTTP stats
     * serialization (priority 5) to keep its 42 ms receive FIFO from filling.
     * Encoding is bounded to two frames per burst; TCP/IP retains priority. */
    if (xTaskCreatePinnedToCoreWithCaps(audio_task, "usb_audio", JETKVM_AUDIO_STACK_BYTES, NULL, 6, &s_task, 0,
                                      MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT) != pdPASS) goto done;
    error = ESP_OK;
done:
    if (error != ESP_OK) {
        if (s_encoder) esp_opus_enc_close(s_encoder);
        s_encoder = NULL; s_task = NULL;
        free(s_pcm); free(s_encoded); free(s_packets);
        s_pcm = s_encoded = NULL; s_packets = NULL;
    }
    jetkvm_memory_policy_release(&reservation);
    unlock(); return error;
}

void jetkvm_audio_usb_ready(bool ready)
{
    /* Teardown must wait for an in-progress FIFO read before uninstalling USB. */
    if (!s_mutex || xSemaphoreTake(s_mutex, portMAX_DELAY) != pdTRUE) return;
    s_usb_ready = ready;
    s_usb_paused = false;
    if (!ready) s_streaming = false;
    invalidate();
    unlock();
}
void jetkvm_audio_usb_pause(bool paused)
{
    /* Quiesce reads before uninstall without losing the host alternate setting
     * if uninstall fails. Real detach/SET_INTERFACE callbacks remain authoritative. */
    if (!s_mutex || xSemaphoreTake(s_mutex, portMAX_DELAY) != pdTRUE) return;
    s_usb_paused = paused; invalidate(); unlock();
}
void jetkvm_audio_set_peer_connected(bool connected)
{
    if (!lock()) return;
    if (s_peer_connected != connected) { s_peer_connected = connected; invalidate(); }
    unlock();
}
bool jetkvm_audio_pop(uint8_t data[JETKVM_AUDIO_PACKET_MAX], int *size, uint32_t *pts)
{
    if (!s_mutex || xSemaphoreTake(s_mutex, 0) != pdTRUE) return false;
    bool available = s_count && s_enabled && s_usb_ready && !s_usb_paused && s_peer_connected;
    if (available) {
        packet_t *packet = &s_packets[s_read];
        *size = packet->size; *pts = packet->pts;
        memcpy(data, packet->data, packet->size);
        s_read = (s_read + 1) % PACKET_SLOTS; --s_count;
    }
    unlock(); return available;
}

/* There is one fixed SOF-synchronous clock and no hardware volume controls. */
bool tud_audio_get_req_entity_cb(uint8_t rhport, const tusb_control_request_t *request)
{
    const audio_control_request_t *audio = (const audio_control_request_t *)request;
    if (audio->bEntityID != 4 || audio->bChannelNumber != 0) return false;
    if (audio->bControlSelector == AUDIO_CS_CTRL_SAM_FREQ) {
        if (audio->bRequest == AUDIO_CS_REQ_CUR) {
            audio_control_cur_4_t rate = {.bCur = 48000};
            return tud_audio_buffer_and_schedule_control_xfer(rhport, request, &rate, sizeof(rate));
        }
        if (audio->bRequest == AUDIO_CS_REQ_RANGE) {
            audio_control_range_4_n_t(1) range = {.wNumSubRanges = 1, .subrange = {{48000, 48000, 0}}};
            return tud_audio_buffer_and_schedule_control_xfer(rhport, request, &range, sizeof(range));
        }
    }
    if (audio->bControlSelector == AUDIO_CS_CTRL_CLK_VALID && audio->bRequest == AUDIO_CS_REQ_CUR) {
        audio_control_cur_1_t valid = {.bCur = 1};
        return tud_audio_buffer_and_schedule_control_xfer(rhport, request, &valid, sizeof(valid));
    }
    return false;
}

void jetkvm_audio_stream_stopped(void)
{
    if (!lock()) return;
    s_streaming = false; invalidate(); unlock();
}
bool tud_audio_set_itf_cb(uint8_t rhport, const tusb_control_request_t *request)
{
    (void)rhport;
    if (!lock()) return false;
    s_streaming = tu_le16toh(request->wValue) == 1;
    invalidate(); unlock(); return true;
}
void jetkvm_audio_get_stats(jetkvm_audio_stats_t *stats)
{
    memset(stats, 0, sizeof(*stats));
    if (!lock()) return;
    stats->usb_ready = s_usb_ready; stats->streaming = s_streaming;
    stats->enabled = s_enabled && s_usb_ready;
    stats->received_packets = __atomic_load_n(&s_stats.received_packets, __ATOMIC_RELAXED);
    stats->received_bytes = __atomic_load_n(&s_stats.received_bytes, __ATOMIC_RELAXED);
    stats->pcm_bytes = s_stats.pcm_bytes;
    stats->encoded_frames = __atomic_load_n(&s_stats.encoded_frames, __ATOMIC_RELAXED);
    stats->encode_errors = __atomic_load_n(&s_stats.encode_errors, __ATOMIC_RELAXED);
    stats->dropped_frames = s_stats.dropped_frames;
    unlock();
}

bool tud_audio_rx_done_isr(uint8_t rhport, uint16_t bytes, uint8_t function,
                           uint8_t endpoint, uint8_t alternate)
{
    (void)rhport; (void)bytes; (void)function; (void)endpoint; (void)alternate;
    __atomic_fetch_add(&s_stats.received_packets, 1, __ATOMIC_RELAXED);
    __atomic_fetch_add(&s_stats.received_bytes, bytes, __ATOMIC_RELAXED);
    return true;
}
