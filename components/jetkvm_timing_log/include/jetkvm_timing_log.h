#pragma once

#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* A log of two events on the esp_timer clock: a keyboard report left on USB,
 * and a video frame was complete. A measurement tool starts the log, sends
 * keys, and reads the log: the time from a key to the frame that shows it is
 * then the difference of two times of this clock, and the network between the
 * tool and the Mini is not part of it.
 *
 * The log holds memory only between start and stop. Start, stop and json are
 * for one task (the HTTP server's); key and frame are for any task. */

/* The log keeps the newest entries of each kind. */
#define JETKVM_TIMING_LOG_KEYS 256U
#define JETKVM_TIMING_LOG_FRAMES 1024U

/* Starts an empty log. A log that runs is emptied. */
esp_err_t jetkvm_timing_log_start(void);

/* Stops the log and frees its memory. */
void jetkvm_timing_log_stop(void);

/* A keyboard report was written to USB at written_us. key is the first key
 * code of the report, 0 when no key is down. */
void jetkvm_timing_log_key(uint8_t key, int64_t written_us);

/* The last byte of a video frame was there at complete_us. rtp_timestamp is
 * the RTP timestamp that the frame's packets carry. */
void jetkvm_timing_log_frame(uint32_t rtp_timestamp, int64_t complete_us);

/* The log as JSON, oldest entry first:
 *
 *   {"running":true,"keyCount":2,"frameCount":1,
 *    "keys":[[<written_us>,<key>],...],"frames":[[<complete_us>,<rtp_timestamp>],...]}
 *
 * keyCount and frameCount count every entry since the start, also those that
 * newer entries replaced. Entries that arrive during the call are not kept.
 * The caller frees the string with free(). NULL when there is no memory. */
char *jetkvm_timing_log_json(void);

#ifdef __cplusplus
}
#endif
