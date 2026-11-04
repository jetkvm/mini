#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* Why a join of a Wi-Fi network failed, and the short text that the web UI
 * (getWifiSettings `connect_error`) and the local display show for it.
 * Independent of ESP-IDF so the host tests run the same code. */

/* The disconnect reasons of wifi_err_reason_t that the classification names;
 * jetkvm_c5_wifi.c checks them against esp_wifi_types. */
#define WIFI_JOIN_REASON_ASSOC_TOOMANY 5U
#define WIFI_JOIN_REASON_MIC_FAILURE 14U
#define WIFI_JOIN_REASON_4WAY_HANDSHAKE_TIMEOUT 15U
#define WIFI_JOIN_REASON_802_1X_AUTH_FAILED 23U
#define WIFI_JOIN_REASON_NO_AP_FOUND 201U
#define WIFI_JOIN_REASON_AUTH_FAIL 202U
#define WIFI_JOIN_REASON_ASSOC_FAIL 203U
#define WIFI_JOIN_REASON_HANDSHAKE_TIMEOUT 204U
#define WIFI_JOIN_REASON_ASSOC_COMEBACK_TIME_TOO_LONG 208U
#define WIFI_JOIN_REASON_NO_AP_FOUND_W_COMPATIBLE_SECURITY 210U
#define WIFI_JOIN_REASON_NO_AP_FOUND_IN_AUTHMODE_THRESHOLD 211U
#define WIFI_JOIN_REASON_NO_AP_FOUND_IN_RSSI_THRESHOLD 212U

/* Room for the longest text of wifi_join_failure_text, with its NUL. */
#define WIFI_JOIN_FAILURE_TEXT_CAPACITY 24U

typedef enum {
    WIFI_JOIN_OK = 0,
    /* The AP rejected the handshake: the password is wrong. */
    WIFI_JOIN_WRONG_PASSWORD,
    /* No AP with the name (and a matching security) answered. */
    WIFI_JOIN_NOT_FOUND,
    /* The AP admitted the station, but DHCP gave no address in time. */
    WIFI_JOIN_NO_ADDRESS,
    /* The AP refused the association (for example, too many stations). */
    WIFI_JOIN_REFUSED,
    /* No disconnect and no address within the join time. */
    WIFI_JOIN_TIMED_OUT,
    /* Any other disconnect reason, shown with its number. */
    WIFI_JOIN_OTHER,
    /* A request (another network, a band, setup) ended the join. */
    WIFI_JOIN_CANCELLED,
} wifi_join_failure_kind_t;

typedef struct {
    wifi_join_failure_kind_t kind;
    /* The disconnect reason; 0 when there was none. */
    uint16_t reason;
} wifi_join_failure_t;

/* The failure that a disconnect with `reason` during a join means. */
wifi_join_failure_t wifi_join_failure_from_reason(uint16_t reason);

/* The failure of a join that ended without a disconnect: no address when the
 * AP admitted the station, else a time-out. */
wifi_join_failure_t wifi_join_failure_timeout(bool associated);

/* The text for `failure`, for example "Wrong password" or "Wi-Fi error 39".
 * Returns the length written (without the NUL); `text` is "" for OK. */
size_t wifi_join_failure_text(wifi_join_failure_t failure, char *text, size_t capacity);
