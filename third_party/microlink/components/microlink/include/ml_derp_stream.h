#pragma once

#include <stddef.h>
#include <stdint.h>

/* Stream callbacks return a byte count, AGAIN, or FAIL. They never block.
 * A TLS writer must receive the same buffer and length after AGAIN. */
#define ML_DERP_STREAM_FAIL (-1)
#define ML_DERP_STREAM_AGAIN (-2)
#define ML_DERP_STREAM_MAX_PAYLOAD 65536u
#define ML_DERP_STREAM_IDLE_TIMEOUT_MS 10000u

typedef int (*ml_derp_stream_io_fn)(void *, uint8_t *, size_t);
typedef void *(*ml_derp_stream_alloc_fn)(size_t);

typedef struct {
    uint8_t header[5];
    size_t header_used;
    uint8_t *rx_data;
    size_t rx_len;
    size_t rx_used;
    uint64_t rx_progress_ms;
    uint8_t *tx_data;
    size_t tx_len;
    size_t tx_used;
    size_t tx_frames;
    uint64_t tx_progress_ms;
} ml_derp_stream_t;

typedef struct {
    uint8_t type;
    uint8_t *data; /* Caller owns this allocation after a complete read. */
    size_t len;
} ml_derp_stream_frame_t;

typedef struct {
    uint8_t type;
    const uint8_t *peer_key;
    const uint8_t *data;
    size_t len;
} ml_derp_stream_write_frame_t;

void ml_derp_stream_reset(ml_derp_stream_t *stream);
int ml_derp_stream_begin(ml_derp_stream_t *stream, uint8_t type,
                         const uint8_t *peer_key, const uint8_t *data,
                         size_t len, uint64_t now_ms,
                         ml_derp_stream_alloc_fn allocate);
int ml_derp_stream_begin_batch(ml_derp_stream_t *stream,
                               const ml_derp_stream_write_frame_t *frames,
                               size_t count, uint64_t now_ms,
                               ml_derp_stream_alloc_fn allocate);
/* Poll once. 1 means progress (write) or a complete frame (read), 0 means
 * pending, and FAIL means the connection must be closed. */
int ml_derp_stream_write(ml_derp_stream_t *stream, ml_derp_stream_io_fn write,
                         void *context, uint64_t now_ms);
int ml_derp_stream_read(ml_derp_stream_t *stream, ml_derp_stream_io_fn read,
                        void *context, uint64_t now_ms,
                        ml_derp_stream_alloc_fn allocate,
                        ml_derp_stream_frame_t *frame);
