#pragma once
#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"
#define JETKVM_AUDIO_PACKET_MAX 1275
#define JETKVM_AUDIO_PACKET_SLOTS 10
#define JETKVM_AUDIO_STACK_BYTES (64 * 1024)
/* Startup precedes USB and RPC workers. USB ownership serializes prepare/use. */
esp_err_t jetkvm_audio_init(void);
esp_err_t jetkvm_audio_prepare_usb(void);
void jetkvm_audio_usb_ready(bool ready);
void jetkvm_audio_usb_pause(bool paused);
void jetkvm_audio_set_peer_connected(bool connected);
bool jetkvm_audio_pop(uint8_t packet[JETKVM_AUDIO_PACKET_MAX], int *size, uint32_t *pts);

typedef struct {
    bool usb_ready, streaming, enabled;
    uint32_t received_packets, received_bytes, pcm_bytes, encoded_frames, encode_errors, dropped_frames;
} jetkvm_audio_stats_t;
void jetkvm_audio_get_stats(jetkvm_audio_stats_t *stats);
void jetkvm_audio_stream_stopped(void);
