/*
 * SPDX-FileCopyrightText: 2026
 * SPDX-License-Identifier: MIT
 */

/*
 * The KVM side of one OS Services USB function: the reset exchange, the
 * byte-stream parser, the handshake and every service. The session is a
 * single-threaded state machine. The caller serializes every call (on the
 * device, the os_services task owns it) and supplies the clock, randomness,
 * the signing key, the IN endpoint and the effects as platform hooks, so the
 * host tests drive it byte for byte.
 */

#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "os_services_video_rate.h"
#include "os_services_wire.h"

#ifdef __cplusplus
extern "C" {
#endif

/* The KVM keeps this many clipboard requests open at once. */
#define OSVC_CLIPBOARD_PENDING_MAX 4U
/* The KVM abandons a clipboard request after this long. */
#define OSVC_CLIPBOARD_TIMEOUT_MS 2000
/* Handshake and liveness limits (Handshake, Liveness). */
#define OSVC_HANDSHAKE_TIMEOUT_MS 15000
#define OSVC_LIVENESS_TIMEOUT_MS 15000
#define OSVC_CAPTURE_DISPLAY_CAPACITY (OSVC_INFO_DISPLAY_MAX + 1U)

/* Terminal sessions the KVM keeps open at once: as many as a sender runs. */
#define OSVC_TERMINAL_SESSIONS_MAX 4U
/* The KVM closes a terminal session that terminal.opened did not answer in
 * this long (Terminal service). */
#define OSVC_TERMINAL_OPEN_TIMEOUT_MS 5000
/* The longest a terminal.output waits for room in the browser's channel,
 * with the OUT endpoint unread, before the terminal ends. An answer behind
 * it waits as long, so it is no longer than a clipboard request may take;
 * well within the sender's 30 seconds for a bulk write. */
#define OSVC_TERMINAL_OUTPUT_HOLD_MS OSVC_CLIPBOARD_TIMEOUT_MS
/* The browser's terminal channel. */
#define OSVC_TERMINAL_CHANNEL_LABEL "guest-terminal"
/* The line a terminal ends with when keystrokes were lost. */
#define OSVC_TERMINAL_INPUT_LOST \
    "Terminal closed: keystrokes were not delivered"

/* Files in one drop. The protocol bounds a drop only by the 64 KiB control
 * payload; this is the size of the KVM's table. */
#define OSVC_FILE_DROP_MAX 256U
/* The KVM gives up on a drop when neither side makes progress for this long
 * (File service). */
#define OSVC_FILE_IDLE_TIMEOUT_MS 30000
/* The browser opens a drop's channel right after the drop is accepted. A
 * drop still without one this long after (the browser left, or could not
 * create the channel) fails, so it cannot hold the sender and refuse the
 * next drop for the whole idle timeout. */
#define OSVC_FILE_ATTACH_TIMEOUT_MS 10000
/* A drop's browser channel is this prefix and the drop ID in decimal. */
#define OSVC_FILE_CHANNEL_PREFIX "guestfile_"

/* The KVM gives up on an update when neither side makes progress for this
 * long, as on a file drop. */
#define OSVC_UPDATE_IDLE_TIMEOUT_MS 30000
/* Why an update failed, cut to 64 bytes on a UTF-8 boundary: the browser's
 * videoInputState event has room for no more (os_services_release.h). */
#define OSVC_UPDATE_MESSAGE_CAPACITY 65U

/* Service video (Video service, Handover). A switch to service video that
 * gets no enabled video.status in this long returns to hardware capture. */
#define OSVC_VIDEO_SWITCH_TIMEOUT_MS 45000
/* After video.config with enabled 0, the KVM waits this long for the
 * disabled video.status before it removes its display anyway. */
#define OSVC_VIDEO_DISABLE_TIMEOUT_MS 15000
/* The retry budget: this many switches in the window, across sessions. */
#define OSVC_VIDEO_RETRY_SWITCHES 3U
#define OSVC_VIDEO_RETRY_WINDOW_MS 120000
/* A switch that has shown service video this long has succeeded and leaves
 * the budget: a sender or host restart after it is no failed switch. */
#define OSVC_VIDEO_RETRY_SHOWN_MS 10000
/* At most one video.request_keyframe in this long; a sync request the
 * platform handed over is given back (requeued) after it too. */
#define OSVC_VIDEO_KEYFRAME_INTERVAL_MS 600
/* A frame waits this long for an access-unit slot; then it is dropped and
 * the platform hears of the gap. Until then the transport holds the USB
 * read, so the bus NAKs the sender instead of losing frames. A weak Wi-Fi
 * link takes up to 770 ms to send a keyframe: a shorter wait drops the next
 * frame, whose gap asks for another keyframe, and so on. */
#define OSVC_VIDEO_SLOT_WAIT_MS 1000
/* SPS and PPS kept from the stream for keyframes that come without them. */
#define OSVC_VIDEO_PARAMETER_SETS_MAX 1024U
/* Static targets sent in video.config when the settings name none; the
 * KVM adapts below them (os_services_video_rate.h). */
#define OSVC_VIDEO_KBPS_720P 5000U
#define OSVC_VIDEO_KBPS_1080P 8000U
#define OSVC_VIDEO_KBPS_1440P 12000U
/* The hardware path's ceiling for any target. */
#define OSVC_VIDEO_KBPS_MAX 12000U
#define OSVC_VIDEO_FPS_MAX 60U
/* The KVM's uplink holds the stream back when frames wait for a slot this
 * share of the time (permille) in this many intervals in a row; the target
 * then goes under what the transport sent and under itself, in proportion
 * to the frames the KVM took of those the sender delivers at this target
 * without such waits. One frame waits at most OSVC_VIDEO_SLOT_WAIT_MS, so
 * a keyframe's wait spans no more than two. */
#define OSVC_VIDEO_PRESSURE_PERMILLE 250U
#define OSVC_VIDEO_PRESSURE_INTERVALS 3U
#define OSVC_VIDEO_PRESSURE_SENT_PERCENT 85U
/* Why video faulted or a switch was refused, for the browser. */
#define OSVC_VIDEO_ERROR_CAPACITY 96U

/* The KVM settings the session acts on. */
typedef struct {
    bool clipboard_enabled;
    bool terminal_enabled;
    bool files_enabled;
    bool video_enabled;
    /* Empty lets the sender select. */
    char capture_display[OSVC_CAPTURE_DISPLAY_CAPACITY];
    uint32_t width;
    uint32_t height;
    osvc_layout_t layout;
    /* 0 selects the encoder default and the native rate. */
    uint32_t target_kbps;
    uint32_t maximum_fps;
} osvc_settings_t;

/* The source the KVM shows (Handover). */
typedef enum {
    OSVC_SOURCE_PHYSICAL = 0,
    OSVC_SOURCE_SWITCHING_TO_GUEST,
    OSVC_SOURCE_GUEST,
    OSVC_SOURCE_SWITCHING_TO_PHYSICAL,
} osvc_video_source_t;

/* The network's view of service video, from video_network(). */
typedef struct {
    /* The target follows the receiver feedback (video quality Auto). With
     * a manual video quality it does not: the estimate is the static
     * target, and only the KVM's own slot waits lower the target. */
    bool adaptive;
    /* An interval of about a second ended since the last call: the target
     * the receiver feedback allows, whether that feedback cut it in this
     * interval, and what the transport sent, in kbps. */
    bool interval;
    uint32_t estimate_kbps;
    bool cut;
    uint32_t sent_kbps;
} osvc_video_network_t;

/* One access unit of service video, handed to frame_commit(). */
typedef struct {
    /* Bytes in the reserved slot, SPS and PPS the KVM added included. */
    size_t length;
    uint32_t pts_90k;
    /* now_us() when the frame's envelope was read and when its last byte
     * was copied. */
    int64_t arrival_us;
    int64_t received_us;
    /* The stream the frame belongs to (video_stream()). */
    uint32_t width;
    uint32_t height;
    uint32_t fps;
    /* The platform's keyframe request (keyframe_wanted()) this keyframe
     * answers; 0 for none. */
    uint32_t keyframe_generation;
    bool keyframe;
    /* The KVM put its cached SPS and PPS in front of the access unit. */
    bool parameter_sets_added;
} osvc_video_frame_t;

typedef struct {
    void *ctx;
    int64_t (*now_ms)(void *ctx);
    void (*fill_random)(void *ctx, uint8_t *out, size_t length);
    /* The KVM's Ed25519 key, made on first use and kept until a factory
     * reset. sign() signs with the same key. */
    bool (*public_key)(void *ctx, uint8_t key[OSVC_PUBLIC_KEY_LEN]);
    bool (*sign)(void *ctx, const uint8_t *message, size_t length,
                 uint8_t signature[OSVC_SIGNATURE_LEN]);
    /* Queues one message for the IN endpoint: the envelope, then head, then
     * body (either can be empty), in order and atomically, all of it or
     * none. Returns false when it does not fit; the session then ends, as
     * the KVM ends it when a write does not complete in 2 seconds. */
    bool (*write)(void *ctx, const uint8_t envelope[OSVC_ENVELOPE_LEN],
                  const uint8_t *head, size_t head_length,
                  const uint8_t *body, size_t body_length);
    /* The session ended. The transport drops what is queued for the IN
     * endpoint and stops reading OUT until the next RESET. */
    void (*session_ended)(void *ctx, const char *reason);
    /* A RESET: the transport cancels every bulk request of the previous
     * session and, once each has completed, calls
     * osvc_session_transport_drained() and reads OUT again. */
    void (*reset_transport)(void *ctx);
    /* Anything the browser shows changed: connection, registrations,
     * sender info, approval. */
    void (*state_changed)(void *ctx);
    /* A clipboard request finished: the sender answered, the request timed
     * out, or the session or the service went away. text is only valid for
     * the call. */
    void (*clipboard_done)(void *ctx, uint32_t request_id,
                           osvc_clipboard_status_t status, bool answered,
                           const uint8_t *text, size_t length,
                           bool truncated);

    /*
     * The terminal and file hooks are optional: a platform that leaves
     * terminal_output NULL offers no terminal, and one that leaves
     * file_credit or file_done NULL receives no files. Like the others,
     * they must not block or call back into the session.
     */

    /* Shell output of a terminal session, in order; data is only valid for
     * the call. Returns false when the browser cannot take it now (its
     * channel is backed up): the session keeps the message and reads
     * nothing more until the hook takes it (osvc_session_holding()), and
     * offers it again on each osvc_session_receive() and
     * osvc_session_tick(), whose request timeouts wait meanwhile. After
     * OSVC_TERMINAL_OUTPUT_HOLD_MS it closes the terminal on both sides
     * instead and reports terminal_closed. */
    bool (*terminal_output)(void *ctx, uint32_t terminal_id,
                            const uint8_t *data, size_t length);
    /* A terminal session ended other than by osvc_session_terminal_close():
     * the shell exited, the sender refused or lost it, the open timed out,
     * or the setting, the service or the session went away. reason is the
     * line the browser shows ("" for none); exited is true only for a shell
     * that ended by itself (reason "The shell exited"). Nothing more
     * comes for terminal_id. */
    void (*terminal_closed)(void *ctx, uint32_t terminal_id,
                            const char *reason, bool exited);
    /* The sender accepts bytes more of a transfer: the browser may send that
     * many more (its credit is cumulative). Held until the drop's channel is
     * attached; never for a transfer that has ended. */
    void (*file_credit)(void *ctx, uint32_t drop_id, uint32_t transfer_id,
                        uint64_t bytes);
    /* A transfer ended, exactly once per transfer: the sender's file.done,
     * a cancel, a refused chunk, the idle timeout, or the setting, the
     * service or the session went away. path (not terminated, only valid
     * for the call) is the sender's final path for OK when it arrived while
     * the channel was attached; otherwise it is empty. last is set on the
     * drop's final transfer: the drop is over and its ID is forgotten. Held
     * until the drop's channel is attached; none after
     * osvc_session_file_channel_closed(). */
    void (*file_done)(void *ctx, uint32_t drop_id, uint32_t transfer_id,
                      osvc_file_status_t status, const char *path,
                      size_t path_length, bool last);

    /*
     * Service video. A platform without display_present, display_restore,
     * video_stream, video_stopped, frame_reserve, frame_commit and
     * frame_cancel shows no service video: every registration is answered
     * with video disabled. The others are optional. Like every hook, they
     * must not block or call back into the session.
     */

    /* A finer clock for the frame timestamps; now_ms() * 1000 without it. */
    int64_t (*now_us)(void *ctx);
    /* Stop hardware capture and present the KVM display to the target:
     * width x height preferred, an EDID with this serial. Returns false when
     * the display cannot be presented. The platform reports progress with
     * osvc_session_display_status(). */
    bool (*display_present)(void *ctx, uint32_t width, uint32_t height,
                            uint32_t serial);
    /* Remove the KVM display and restart hardware capture. */
    void (*display_restore)(void *ctx);
    /* A coded sequence starts: the frames that follow are width x height at
     * fps, and frames queued from before are discarded. Returns the bytes
     * one access unit of the stream may take, or 0 when the KVM cannot show
     * such a stream; video then faults. A keyframe above that faults video
     * too, and a larger delta frame is dropped. */
    size_t (*video_stream)(void *ctx, uint32_t width, uint32_t height,
                           uint32_t fps);
    /* Service video stopped for a switch to hardware capture; no frame
     * follows until video_stream(). */
    void (*video_stopped)(void *ctx);
    /* A slot for one access unit of length bytes, at most what
     * video_stream() returned. NULL with *busy set: every slot is taken, try
     * again; NULL without: no viewer takes video, so the frame is
     * discarded. */
    uint8_t *(*frame_reserve)(void *ctx, size_t length, bool *busy);
    /* The reserved slot holds the access unit. Returns whether the viewer's
     * queue took it. */
    bool (*frame_commit)(void *ctx, const osvc_video_frame_t *frame);
    /* The reserved slot is given back; written bytes of it were written. */
    void (*frame_cancel)(void *ctx, size_t written);
    /* Frames were lost before the platform got them; the viewer needs a
     * keyframe. */
    void (*video_gap)(void *ctx);
    /* The platform's keyframe request: a non-zero generation that the
     * session owns until it calls keyframe_done(), or 0 for none. */
    uint32_t (*keyframe_wanted)(void *ctx);
    /* A keyframe for generation was committed (delivered) or not in time. */
    void (*keyframe_done)(void *ctx, uint32_t generation, bool delivered);
    /* The network's view of the stream, whose static target is
     * ceiling_kbps; on every tick while it plays. Without it the targets
     * stay static. */
    void (*video_network)(void *ctx, uint32_t ceiling_kbps,
                          osvc_video_network_t *network);
} osvc_platform_t;

typedef enum {
    OSVC_LINK_IDLE = 0,       /* no session; waiting for RESET */
    OSVC_LINK_RESETTING,      /* RESET answered; old transfers draining */
    OSVC_LINK_ADMITTED,       /* phase Ready; searching for session.hello */
    OSVC_LINK_OPEN,           /* handshake done */
} osvc_link_state_t;

typedef struct {
    uint32_t flags;
    osvc_target_os_t os;
} osvc_clipboard_registration_t;

typedef struct {
    uint32_t request_id;
    int64_t deadline_ms;
    /* OSVC_CLIPBOARD_GET or OSVC_CLIPBOARD_SET. */
    uint16_t kind;
    bool in_use;
} osvc_clipboard_request_t;

typedef enum {
    OSVC_TERMINAL_FREE = 0,
    OSVC_TERMINAL_OPENING,    /* terminal.open sent; no terminal.opened yet */
    OSVC_TERMINAL_RUNNING,    /* the sender answered OK */
} osvc_terminal_state_t;

typedef struct {
    uint32_t terminal_id;
    osvc_terminal_state_t state;
    /* While opening: when the KVM gives up on terminal.opened. */
    int64_t deadline_ms;
} osvc_terminal_t;

/* One file of a drop the browser offers (osvc_session_file_drop()). */
typedef struct {
    /* UTF-8, not terminated. */
    const char *name;
    size_t name_length;
    uint64_t size;
} osvc_file_entry_t;

typedef struct {
    uint64_t size;
    /* Bytes relayed to the sender, which is also the next chunk's offset,
     * and the sender's cumulative credit. sent <= credit and sent <= size. */
    uint64_t sent;
    uint64_t credit;
    /* The part of credit the browser has been told about. */
    uint64_t credit_reported;
    /* Valid once ended. */
    osvc_file_status_t status;
    bool ready;
    bool ended;
    /* file_done delivered. */
    bool reported;
} osvc_file_transfer_t;

/* The drop in progress; drop_id 0 means none. Transfer i has the ID
 * drop_id + 1 + i. */
typedef struct {
    uint32_t drop_id;
    uint32_t count;
    uint32_t ended;
    uint32_t reported;
    /* The browser's channel is open: outcomes go to it as they happen. */
    bool attached;
    /* The last progress of either side, or the drop's end while its
     * outcomes wait for the channel. */
    int64_t progress_ms;
    /* When an unattached drop with open transfers fails. */
    int64_t attach_deadline_ms;
    osvc_file_transfer_t transfers[OSVC_FILE_DROP_MAX];
} osvc_file_drop_t;

/* The update in progress, or the last one; update_id 0 means none yet. */
typedef struct {
    uint32_t update_id;
    uint64_t size;
    /* Bytes relayed, which is also the next chunk's offset, and the sender's
     * cumulative credit. sent <= credit and sent <= size. */
    uint64_t sent;
    uint64_t credit;
    bool ready;
    bool ended;
    /* Valid once ended: the sender's update.status, or Failed or Cancelled
     * when the KVM ended the update. */
    uint8_t status;
    char message[OSVC_UPDATE_MESSAGE_CAPACITY];
    int64_t progress_ms;
} osvc_update_t;

/* Counters for the tests, the logs and the diagnostics RPC. */
typedef struct {
    uint32_t sessions_opened;
    uint32_t sessions_ended;
    uint32_t resets;
    uint32_t messages_in;
    uint32_t messages_skipped;
    uint32_t service_faults;
    uint64_t bytes_in;
    uint64_t bytes_discarded;
    uint64_t bytes_out;
    /* The time from admission to session.ready, of the last handshake. */
    int64_t last_handshake_ms;
} osvc_counters_t;

/* Switches to service video started in the retry window, across sessions
 * (Switch to service video). */
typedef struct {
    int64_t starts_ms[OSVC_VIDEO_RETRY_SWITCHES];
    uint32_t count;
    /* A switch was refused; only the user's video setting clears it. */
    bool blocked;
} osvc_video_recovery_t;

/* Service video counters, kept across sessions for /webrtc/stats. */
typedef struct {
    /* Access units handed to frame_commit(), and their bytes. */
    uint32_t frames;
    uint32_t keyframes;
    uint64_t bytes;
    /* Frames read past while no stream was shown or no viewer took them. */
    uint32_t frames_discarded;
    /* Frames dropped after the slot wait, or too large for a slot. */
    uint32_t frames_dropped;
    /* Frames that waited for a slot, the longest wait and all of them. */
    uint32_t slot_waits;
    uint32_t slot_wait_max_ms;
    uint32_t keyframe_requests;
    /* Keyframes the KVM completed with its cached SPS and PPS. */
    uint32_t parameter_sets_added;
    uint32_t stream_resets;
    uint32_t switches;
    /* Switches that ended without service video. */
    uint32_t switch_failures;
    uint32_t faults;
    uint32_t largest_keyframe;
    uint32_t slot_wait_ms;
    /* Frames the sender did not send at the stream's rate: a gap of 1.5 to
     * 2.5 frame periods between the capture times of two frames it sent is
     * one, and so on. Gaps above 1 s and across a stream's start do not
     * count. */
    uint32_t sender_skipped_frames;
    /* The service task's waits for a slot that ended at the tick, not at
     * the slot's release (jetkvm_os_services.c). */
    uint32_t slot_wait_tick_wakeups;
} osvc_video_counters_t;

typedef enum {
    OSVC_PARSE_SEARCH = 0,
    OSVC_PARSE_HEADER,
    OSVC_PARSE_PAYLOAD,
    OSVC_PARSE_SKIP,
    /* The terminal.output in payload waits for the browser. */
    OSVC_PARSE_HELD,
    OSVC_PARSE_STOPPED,
    /* A video.frame: its 12-byte head, then the wait for a slot, then the
     * access unit straight into the slot. A frame with parameter sets that
     * gets no slot is read into the payload buffer for its SPS and PPS. */
    OSVC_PARSE_FRAME_HEAD,
    OSVC_PARSE_FRAME_WAIT,
    OSVC_PARSE_FRAME_DATA,
    OSVC_PARSE_FRAME_SETS,
} osvc_parse_state_t;

typedef struct osvc_session {
    osvc_platform_t platform;
    osvc_settings_t settings;

    /* Reset exchange. */
    osvc_link_state_t link;
    bool faulted;
    uint8_t token[OSVC_TOKEN_LEN];
    int64_t admitted_ms;
    int64_t last_message_ms;

    /* Stream parser. */
    osvc_parse_state_t parse;
    uint8_t header[OSVC_ENVELOPE_LEN];
    size_t header_fill;
    uint16_t service;
    uint16_t kind;
    uint32_t payload_size;
    uint32_t payload_fill;
    uint8_t *payload;
    size_t payload_capacity;
    /* When the held terminal.output first went to the browser. */
    int64_t held_ms;
    uint64_t searched;
    /* The tail of the input kept across calls while searching for the
     * hello: one byte less than the hello message. */
    uint8_t search_tail[OSVC_ENVELOPE_LEN + OSVC_HELLO_LEN - 1U];
    size_t search_tail_fill;

    /* Open session. */
    uint16_t protocol_minor;
    bool approval_pending;
    bool sender_info_valid;
    char sender_version[OSVC_INFO_VERSION_MAX + 1U];
    /* Display names, NUL-separated, display_count of them. */
    char displays[OSVC_INFO_DISPLAYS_MAX * (OSVC_INFO_DISPLAY_MAX + 1U)];
    size_t display_count;
    char fingerprint[OSVC_FINGERPRINT_TEXT_CAPACITY];
    /* Messages received in this session. */
    uint32_t session_messages;
    /* Set when the KVM detached a service for an invalid message; cleared
     * by the service's next capabilities message (Errors). */
    bool service_faulted[OSVC_SERVICE_COUNT];
    char last_fault_reason[96];

    /* Clipboard service. */
    bool clipboard_registered;
    osvc_clipboard_registration_t clipboard;
    osvc_clipboard_request_t clipboard_requests[OSVC_CLIPBOARD_PENDING_MAX];
    uint32_t next_clipboard_request;

    /* Terminal service: the sender's registration and the open sessions. */
    bool terminal_shell;
    osvc_terminal_t terminals[OSVC_TERMINAL_SESSIONS_MAX];
    uint32_t next_terminal_id;

    /* File service: the sender's registration and the drop in progress.
     * Drop and transfer IDs come from one counter. */
    bool file_receive;
    osvc_file_drop_t file_drop;
    uint32_t next_file_id;
    char file_error[128];

    /* Update service: the sender's registration and the update. */
    bool update_install;
    char update_package[OSVC_UPDATE_PACKAGE_MAX + 1U];
    osvc_update_t update;
    uint32_t next_update_id;

    /* Video service: the sender's registration. */
    bool video_registered;
    uint32_t video_codec_flags;
    uint32_t video_max_width;
    uint32_t video_max_height;
    uint32_t video_max_fps;
    /* A registration or the user asks for a switch to service video. */
    bool video_switch_wanted;
    /* The registration's video.config is still owed. */
    bool video_answer_pending;
    /* video.config with enabled 1 went out for the display presented now;
     * a new presentation owes another once it is up. */
    bool video_config_enabled;
    /* The sender was told to enable video and has not reported it disabled
     * since: its enabled statuses are expected, if perhaps stale. */
    bool video_sender_enabled;
    /* video.config with enabled 0 went out while the KVM showed service
     * video; the KVM display stays until the disabled status. */
    bool video_disable_pending;
    /* An enabled status started a stream; frames are shown. */
    bool video_stream_started;
    /* The next frame starts a coded sequence (stream_reset 1). */
    bool video_reset_pending;
    /* The pts clock is re-anchored at the next frame. */
    bool video_pts_restart;
    uint32_t video_width;
    uint32_t video_height;
    uint32_t video_fps;
    /* What one access unit of the stream may take (video_stream()). */
    size_t video_frame_capacity;
    uint64_t video_last_capture_us;
    /* The capture time of the stream's last frame whose head was read,
     * every frame the sender sent, also one dropped for want of a slot. */
    bool video_sent_capture_valid;
    uint64_t video_sent_capture_us;
    /* The platform's keyframe request held, and when it was taken. */
    uint32_t video_keyframe_generation;
    int64_t video_keyframe_taken_ms;
    bool video_keyframe_requested;
    int64_t video_keyframe_request_ms;
    /* Why video faulted or switches are refused, for the browser. */
    char video_error[OSVC_VIDEO_ERROR_CAPACITY];
    /* SPS and PPS of the stream, each behind a 4-byte start code. */
    uint8_t parameter_sets[OSVC_VIDEO_PARAMETER_SETS_MAX];
    size_t parameter_sets_length;

    /* The video.frame being read. */
    uint8_t frame_head[OSVC_VIDEO_FRAME_HEADER_LEN];
    size_t frame_head_fill;
    uint32_t frame_flags;
    uint64_t frame_capture_us;
    int64_t frame_arrival_us;
    /* The access unit's size, and what goes in front of it. */
    size_t frame_unit_length;
    size_t frame_prefix;
    uint8_t *frame_slot;
    size_t frame_fill;
    bool frame_waiting;
    int64_t frame_wait_ms;

    /* The adapted targets, how long frames waited for a slot in the
     * current network interval and how many frames it took, and in how
     * many intervals before it in a row they waited long. */
    osvc_video_rate_t video_rate;
    int64_t video_interval_ms;
    uint32_t video_interval_wait_ms;
    uint32_t video_interval_frames;
    uint32_t video_pressure_intervals;
    /* The frames per 1000 s that the KVM took in the last interval at this
     * target in which frames did not wait long: what the sender delivers
     * by itself. 0 until there is such an interval. */
    uint32_t video_free_mfps;
    /* The adapted targets follow the receiver feedback (video quality
     * Auto). */
    bool video_rate_follows;
    /* target_kbps of the sender's last enabled video.status. */
    uint32_t video_sender_kbps;
    /* The session ends for the KVM's own reason (its USB restart, or the
     * sender exits to install the KVM's update), and the next session
     * switches back: this session's switch does not count against the
     * retry budget. */
    bool video_switch_back;

    /* Kept across sessions: the shown source, the retry budget, the stream
     * clock and the counters. */
    osvc_video_source_t video_source;
    int64_t video_source_ms;
    /* display_restore() went out; the platform has not confirmed it. */
    bool video_restoring;
    /* The platform holds display handovers (osvc_session_display_held). */
    bool video_display_held;
    osvc_video_recovery_t video_recovery;
    /* The current switch has been taken back out of the retry budget. */
    bool video_switch_refunded;
    bool video_pts_valid;
    uint64_t video_pts_us;
    osvc_video_counters_t video_counters;

    osvc_counters_t counters;
    char last_end_reason[96];
} osvc_session_t;

/*
 * Initializes a session with no USB session in progress. payload_buffer
 * holds one control payload (at least OSVC_MAX_CONTROL_PAYLOAD bytes) and
 * belongs to the session until it is dropped.
 */
void osvc_session_init(osvc_session_t *session, const osvc_platform_t *platform,
                       const osvc_settings_t *settings, uint8_t *payload_buffer,
                       size_t payload_capacity);

/* The vendor RESET request: ends any session and answers with a new token
 * in phase Resetting. */
void osvc_session_usb_reset(osvc_session_t *session,
                            uint8_t status[OSVC_USB_STATUS_LEN]);
/* RESET with a token the caller made (non-zero), for a transport that
 * answers the control request before the session's task runs. */
void osvc_session_usb_reset_with_token(osvc_session_t *session,
                                       const uint8_t token[OSVC_TOKEN_LEN],
                                       uint8_t status[OSVC_USB_STATUS_LEN]);
/* The vendor STATUS request. */
void osvc_session_usb_status(const osvc_session_t *session,
                             uint8_t status[OSVC_USB_STATUS_LEN]);
/* The transport completed every bulk request of the previous session and
 * reads OUT again: the session for the RESET token is admitted. */
void osvc_session_transport_drained(osvc_session_t *session);
/* Ends the session, for a reason outside the stream: the IN endpoint did
 * not drain in time. */
void osvc_session_end(osvc_session_t *session, const char *reason);
/* The USB function was unbound, disabled, enabled or lost the bus;
 * by_kvm when the KVM restarted it for a mount or a USB setting. */
void osvc_session_usb_reconfigured(osvc_session_t *session, bool by_kvm);
/* The USB function failed; no session can start until it recovers. */
void osvc_session_set_faulted(osvc_session_t *session, bool faulted);

/*
 * Bytes from the OUT endpoint. Returns the number of bytes consumed; it is
 * less than length when the session ended, after which the transport stops
 * reading, or when a video frame waits for an access-unit slot
 * (osvc_session_frame_waiting()) or a terminal.output for the browser
 * (osvc_session_holding()). The caller then keeps the rest of the read, and
 * with it the bus, and offers it again: when the platform frees a slot or
 * the browser takes output, and at least every tick. After
 * OSVC_VIDEO_SLOT_WAIT_MS the frame is dropped and the rest is consumed.
 */
size_t osvc_session_receive(osvc_session_t *session, const uint8_t *data,
                            size_t length);
bool osvc_session_frame_waiting(const osvc_session_t *session);
/* A terminal.output waits for room in the browser's channel. */
bool osvc_session_holding(const osvc_session_t *session);

/*
 * The KVM display as the platform reports it, at least every tick while
 * the source is switching: presented once display_present()'s EDID is on
 * the target and its hotplug has completed; owned from display_present()
 * until display_restore() has completed. Owned turning false on its own
 * means the display was lost.
 */
void osvc_session_display_status(osvc_session_t *session, bool presented,
                                 bool owned);

/*
 * Whether the platform holds display handovers, reported every tick. While
 * a firmware install suspends capture, the KVM display neither appears nor
 * goes. A registration is answered with video disabled meanwhile, so the
 * sender does not wait for the display; video.config with enabled 1 follows
 * once the display is presented. A switch's timeout starts again when the
 * hold ends.
 */
void osvc_session_display_held(osvc_session_t *session, bool held);

/* "physical", "switching_to_guest", "guest" or "switching_to_physical". */
const char *osvc_video_source_name(osvc_video_source_t source);

/* Runs the timers. Call at least every 100 ms. */
void osvc_session_tick(osvc_session_t *session);

/* Applies new KVM settings, and sends what the change requires. */
void osvc_session_apply_settings(osvc_session_t *session,
                                 const osvc_settings_t *settings);

/* The KVM key's fingerprint, for the settings page; empty until known. */
void osvc_session_set_fingerprint(osvc_session_t *session, const char *text);

/*
 * Starts a clipboard.get (text NULL) or clipboard.set request. Returns the
 * request ID, or 0 when the request cannot start; *status then says why
 * (Unsupported: no sender, no access or sharing off; Failed: too many open
 * requests or the write failed).
 */
uint32_t osvc_session_clipboard_request(osvc_session_t *session,
                                        const uint8_t *text, size_t length,
                                        osvc_clipboard_status_t *status);

bool osvc_session_open(const osvc_session_t *session);
/* Clipboard gestures reach the target: a sender with clipboard access is
 * connected and sharing is on. */
bool osvc_session_clipboard_available(const osvc_session_t *session);

/*
 * Terminal service. The browser's "guest-terminal" channel maps onto one
 * terminal session: open when the channel opens, input for its binary
 * messages, resize for its {"rows","cols"} messages, close when it closes.
 * Output and the end arrive through terminal_output and terminal_closed.
 */

/* A shell is offered (getVideoState's guestTerminal): a sender with a shell
 * is connected, the terminal setting is on and the platform takes output. */
bool osvc_session_terminal_available(const osvc_session_t *session);

/*
 * Opens a terminal session: terminal.open with size 0 by 0, then
 * terminal.resize when rows and cols are both non-zero (each at most
 * 65535). Returns NULL with *terminal_id set, or the line to show the
 * browser when no terminal can open (*terminal_id is then 0). The sender has
 * 5 seconds to answer; a refusal or a timeout arrives as terminal_closed.
 */
const char *osvc_session_terminal_open(osvc_session_t *session, uint32_t rows,
                                       uint32_t cols, uint32_t *terminal_id);

/*
 * Keystrokes for the shell, in terminal.input messages of at most 65532
 * bytes. Returns false when terminal_id is not open, or when a write failed:
 * a gap would change what the shell runs, so the terminal closes
 * (terminal_closed) and the session ends.
 */
bool osvc_session_terminal_input(osvc_session_t *session, uint32_t terminal_id,
                                 const uint8_t *data, size_t length);

/* The browser took bytes more of the terminal's output: terminal.credit lets
 * the sender send that much more. Returns false, and sends nothing, for a
 * terminal that is not open; a failed write ends the session. */
bool osvc_session_terminal_credit(osvc_session_t *session, uint32_t terminal_id,
                                  uint32_t bytes);

/* The browser's terminal size: rows and cols 1 to 65535. Returns false, and
 * sends nothing, for an invalid size or a terminal that is not open. */
bool osvc_session_terminal_resize(osvc_session_t *session, uint32_t terminal_id,
                                  uint32_t rows, uint32_t cols);

/* The browser closed its terminal: terminal.close to the sender, and no
 * terminal_closed. Nothing happens for an ID that is not open. */
void osvc_session_terminal_close(osvc_session_t *session, uint32_t terminal_id);

/*
 * File service. The browser's guestFileDrop RPC maps onto
 * osvc_session_file_drop(), which names the channel the browser then opens
 * (OSVC_FILE_CHANNEL_PREFIX and the drop ID). When that channel opens, the
 * glue attaches it; its binary messages (transfer ID, then bytes) go to
 * osvc_session_file_data(), its {"type":"cancel","transfer":N} messages to
 * osvc_session_file_cancel(), and its close to
 * osvc_session_file_channel_closed(). file_credit and file_done become the
 * channel's {"type":"credit"} and {"type":"done"} messages.
 */

/* Files can be received (getVideoState's guestFile.receive): a sender that
 * receives files is connected, the files setting is on and the platform
 * relays credit and outcomes. */
bool osvc_session_file_available(const osvc_session_t *session);

/*
 * Offers count files to the sender in one file.drop. Returns NULL with
 * *drop_id and transfer_ids[0..count) set, or the reason to show the browser
 * (then nothing was sent and *drop_id is 0; the text stays valid until the
 * next call). One drop is open at a time. Names follow
 * osvc_file_name_valid(), a drop has at most OSVC_FILE_DROP_MAX files, and
 * its file.drop payload must fit 64 KiB.
 */
const char *osvc_session_file_drop(osvc_session_t *session,
                                   const osvc_file_entry_t *files, size_t count,
                                   uint32_t *drop_id, uint32_t *transfer_ids);

/*
 * The drop's channel is open: credit and outcomes held so far are delivered
 * now, and the rest as they happen. Returns false for a drop that is unknown
 * or already attached; the glue then closes the channel.
 */
bool osvc_session_file_attach(osvc_session_t *session, uint32_t drop_id);

/*
 * Bytes of a transfer from the drop's channel, relayed at once as file.chunk
 * messages of at most 65524 bytes each. Returns true when they went to the
 * sender. False: dropped, because there are none or the transfer is not open
 * in this drop; refused, because they exceed the transfer's credit or size,
 * which fails the whole drop; or the write failed, which ends the session.
 */
bool osvc_session_file_data(osvc_session_t *session, uint32_t drop_id,
                            uint32_t transfer_id, const uint8_t *data,
                            size_t length);

/* The browser stopped a transfer: file.cancel to the sender and file_done
 * Cancelled. Returns false when the transfer is not open in this drop. */
bool osvc_session_file_cancel(osvc_session_t *session, uint32_t drop_id,
                              uint32_t transfer_id);

/* The drop's channel closed: each transfer still open is cancelled on the
 * sender, and the drop is forgotten without further callbacks. */
void osvc_session_file_channel_closed(osvc_session_t *session,
                                      uint32_t drop_id);

/* A channel for the drop was refused or closed before it was attached:
 * when no channel is attached to the drop, each transfer still open ends
 * with status (file.cancel to the sender, no file_done) and the drop is
 * forgotten, so the next drop is taken at once. Returns the drop's count of
 * transfers, or 0 when it did nothing. */
uint32_t osvc_session_file_end_unattached(osvc_session_t *session,
                                          uint32_t drop_id,
                                          osvc_file_status_t status);

/* The size of a transfer of the drop, for a drop and transfer that exist.
 * Only reads the session. */
bool osvc_session_file_size(const osvc_session_t *session, uint32_t drop_id,
                            uint32_t transfer_id, uint64_t *size);

/* The drop's channel lost bytes the browser sent (the glue could not hold
 * them): each transfer still open fails, with file.cancel to the sender and
 * file_done Failed, as for bytes beyond the credit. */
void osvc_session_file_fail(osvc_session_t *session, uint32_t drop_id);

/*
 * Update service. The update relay (os_services_relay.c) downloads and checks
 * a release package, and the service task relays it with these calls; the
 * outcome is in session->update once it has ended.
 */

/* A sender that installs packages is connected. */
bool osvc_session_update_available(const osvc_session_t *session);

/* Sends update.begin. Returns NULL with *update_id set, or why no update
 * started (nothing was sent then, unless the write failed). One update is
 * open at a time. */
const char *osvc_session_update_begin(osvc_session_t *session, uint64_t size,
                                      const uint8_t sha256[32],
                                      const char *version,
                                      uint32_t *update_id);

/* Relays up to length bytes of the update, at the next offset, in
 * update.chunk messages as far as the sender's credit and the package size
 * allow. Returns the bytes sent: 0 while there is no credit or once the
 * update has ended. */
size_t osvc_session_update_send(osvc_session_t *session, uint32_t update_id,
                                const uint8_t *data, size_t length);

/* Ends the update with update.cancel and status Cancelled, for reason. */
void osvc_session_update_cancel(osvc_session_t *session, uint32_t update_id,
                                const char *reason);

const char *osvc_clipboard_status_name(osvc_clipboard_status_t status);
const char *osvc_target_os_name(osvc_target_os_t os);
/* "ok", "failed" or "cancelled", as the drop's channel reports them. */
const char *osvc_file_status_name(osvc_file_status_t status);

#ifdef __cplusplus
}
#endif
