/* JetKVM P4 HDMI capture and H.264 pipeline lifecycle. */
#ifndef JETKVM_VIDEO_H
#define JETKVM_VIDEO_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"
#include "webrtc_transport.h"

#ifdef __cplusplus
extern "C" {
#endif

/* The source of "videoEncode" in /webrtc/stats: the encoder's times since
 * boot, all zero before the first frame. Callable from any task, also before
 * jetkvm_video_start(); main registers it with
 * webrtc_transport_set_video_encode_source(). */
void jetkvm_video_get_encode_stats(webrtc_transport_video_encode_t *encode);

/* The source of "hdmiReceiver" in /webrtc/stats: copies of what the capture
 * task read from the receiver chip last. It reads no register. Callable from
 * any task, also before jetkvm_video_start(); main registers it with
 * webrtc_transport_set_hdmi_receiver_source(). */
void jetkvm_video_get_hdmi_receiver_stats(
    webrtc_transport_hdmi_receiver_t *receiver);

/* Allocates the video buffers for the device's lifetime: six capture slots
 * and the H.264 DB buffer of the largest mode (about 21.8 MB of PSRAM). Call
 * once at boot, before other long-lived PSRAM owners and before
 * jetkvm_video_start(); mode changes never free or request them again. */
esp_err_t jetkvm_video_reserve_buffers(void);

/* Starts the persistent capture-owner task. The current firmware has no
 * runtime shutdown path; reset/OTA remains the lifecycle boundary. */
esp_err_t jetkvm_video_start(void);

/* Runtime state consumed by the screen and settings RPC adapters. */
/* Cached source +5 V presence, also updated while the host display is hidden
 * or capture is asleep. Does not require a supported video mode or a viewer. */
bool jetkvm_video_is_hdmi_connected(void);
bool jetkvm_video_is_sleep_active(void);

/* Ethernet boards use the physical link state to select the local LCD mode:
 * link down falls back to HDMI preview, while link up restores H.264/WebRTC.
 * The capture-owner task performs the actual pipeline rebuild. */
void jetkvm_video_set_network_available(bool available);

/* Policy values are persisted by the caller before being published here. */
void jetkvm_video_set_host_display_idle_enabled(bool enabled);
void jetkvm_video_set_sleep_after_seconds(int32_t duration_seconds);

/* Suspend/resume the capture pipeline while an OTA install runs; see the gate
 * in jetkvm_video.c for why a running capture would tear the session down. */
void jetkvm_video_set_capture_suspended(bool suspended);
/* True while capture is suspended; present_guest and restore_physical
 * requests wait until it resumes. */
bool jetkvm_video_capture_suspended(void);

/* OS Services guest video owns the HDMI receiver while the computer's own
 * display output is replaced by the sender's stream. Both calls queue a
 * request to the capture task and return at once: present_guest stops the
 * physical capture and programs the given 256-byte EDID (HPD low ~2 s);
 * restore_physical reprograms the stored EDID preset and resumes capture. */
esp_err_t jetkvm_video_present_guest(const uint8_t edid[256]);
esp_err_t jetkvm_video_restore_physical(void);
/* True from the moment present_guest is accepted until restore completes. */
bool jetkvm_video_guest_owns_display(void);
/* True once the EDID of the latest accepted present_guest is on the bridge
 * and HPD is high again; false again at each present_guest/restore_physical. */
bool jetkvm_video_guest_display_presented(void);
/* While the guest display is presented, physical capture is down and its
 * frame buffers are idle. Lends the first count of them (DMA-capable PSRAM,
 * *bytes each) to service video until jetkvm_video_return_guest_buffers();
 * hardware capture stays down until then, also after a restore. The caller
 * writes back its cache lines before the return. ESP_ERR_INVALID_STATE
 * unless presented and nothing is lent. */
esp_err_t jetkvm_video_lend_guest_buffers(uint8_t *buffers[], size_t count,
                                          size_t *bytes);
void jetkvm_video_return_guest_buffers(void);

#ifdef __cplusplus
}
#endif

#endif /* JETKVM_VIDEO_H */
