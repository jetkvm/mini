#ifndef WEBRTC_STATS_JSON_H
#define WEBRTC_STATS_JSON_H

#include "cJSON.h"
#include "esp_err.h"

/* Builds live /webrtc/stats; expensive heap details include cache/sample metadata.
 * The caller owns *root_out and
 * must release it with cJSON_Delete(). */
esp_err_t webrtc_stats_json_create(cJSON **root_out);
/* Explicit full diagnostics may interrupt video while scanning heaps. */
esp_err_t webrtc_stats_json_create_detailed(cJSON **root_out);
/* Bitrate controller inputs and decisions only (?bitrate=1), small enough to
 * poll at 4 Hz without loading the video link. */
esp_err_t webrtc_stats_json_create_bitrate(cJSON **root_out);

#endif /* WEBRTC_STATS_JSON_H */
