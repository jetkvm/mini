#include "jetkvm_timing_log.h"

#include <inttypes.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>

#include "freertos/FreeRTOS.h"

typedef struct {
    int64_t us;
    uint32_t value;
} entry_t;

/* count is every entry since the start; the newest is at count - 1, modulo
 * the capacity. Both capacities divide 2^32, so a count that wraps stays in
 * step. */
typedef struct {
    entry_t *entries;
    uint32_t count;
} ring_t;

/* "[" int64 "," uint32 "]" and a comma. */
#define ENTRY_JSON_MAX 34U
/* The object's braces, names and counts. */
#define FRAME_JSON_MAX 128U

static portMUX_TYPE s_lock = portMUX_INITIALIZER_UNLOCKED;
static ring_t s_keys;
static ring_t s_frames;
/* json reads the rings without the lock; the writers stay out meanwhile. */
static bool s_reading;

static void append(ring_t *ring, uint32_t capacity, int64_t us, uint32_t value)
{
    portENTER_CRITICAL(&s_lock);
    if (ring->entries != NULL && !s_reading) {
        ring->entries[ring->count % capacity] = (entry_t) {.us = us, .value = value};
        ring->count++;
    }
    portEXIT_CRITICAL(&s_lock);
}

/* Puts new rings in place and frees the old ones. */
static void replace(entry_t *keys, entry_t *frames)
{
    portENTER_CRITICAL(&s_lock);
    entry_t *old_keys = s_keys.entries;
    entry_t *old_frames = s_frames.entries;
    s_keys = (ring_t) {.entries = keys};
    s_frames = (ring_t) {.entries = frames};
    portEXIT_CRITICAL(&s_lock);
    free(old_keys);
    free(old_frames);
}

esp_err_t jetkvm_timing_log_start(void)
{
    entry_t *keys = calloc(JETKVM_TIMING_LOG_KEYS, sizeof(entry_t));
    entry_t *frames = calloc(JETKVM_TIMING_LOG_FRAMES, sizeof(entry_t));
    if (keys == NULL || frames == NULL) {
        free(keys);
        free(frames);
        return ESP_ERR_NO_MEM;
    }
    replace(keys, frames);
    return ESP_OK;
}

void jetkvm_timing_log_stop(void)
{
    replace(NULL, NULL);
}

void jetkvm_timing_log_key(uint8_t key, int64_t written_us)
{
    append(&s_keys, JETKVM_TIMING_LOG_KEYS, written_us, key);
}

void jetkvm_timing_log_frame(uint32_t rtp_timestamp, int64_t complete_us)
{
    append(&s_frames, JETKVM_TIMING_LOG_FRAMES, complete_us, rtp_timestamp);
}

static uint32_t held(const ring_t *ring, uint32_t capacity)
{
    return ring->entries == NULL ? 0U : ring->count < capacity ? ring->count : capacity;
}

static size_t print_ring(char *out, size_t size, const ring_t *ring, uint32_t capacity)
{
    const uint32_t entries = held(ring, capacity);
    size_t at = 0U;
    for (uint32_t i = 0U; i < entries; i++) {
        const entry_t *entry = &ring->entries[(ring->count - entries + i) % capacity];
        at += (size_t)snprintf(out + at, size - at, "%s[%" PRId64 ",%" PRIu32 "]",
                               i == 0U ? "" : ",", entry->us, entry->value);
    }
    return at;
}

char *jetkvm_timing_log_json(void)
{
    portENTER_CRITICAL(&s_lock);
    s_reading = true;
    const ring_t keys = s_keys;
    const ring_t frames = s_frames;
    portEXIT_CRITICAL(&s_lock);

    const size_t size = FRAME_JSON_MAX +
        ENTRY_JSON_MAX * ((size_t)held(&keys, JETKVM_TIMING_LOG_KEYS) +
                          held(&frames, JETKVM_TIMING_LOG_FRAMES));
    char *json = malloc(size);
    if (json != NULL) {
        size_t at = (size_t)snprintf(
            json, size, "{\"running\":%s,\"keyCount\":%" PRIu32 ",\"frameCount\":%" PRIu32 ",\"keys\":[",
            keys.entries != NULL ? "true" : "false", keys.count, frames.count);
        at += print_ring(json + at, size - at, &keys, JETKVM_TIMING_LOG_KEYS);
        at += (size_t)snprintf(json + at, size - at, "],\"frames\":[");
        at += print_ring(json + at, size - at, &frames, JETKVM_TIMING_LOG_FRAMES);
        (void)snprintf(json + at, size - at, "]}");
    }

    portENTER_CRITICAL(&s_lock);
    s_reading = false;
    portEXIT_CRITICAL(&s_lock);
    return json;
}
