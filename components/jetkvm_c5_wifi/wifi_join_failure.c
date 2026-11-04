#include "wifi_join_failure.h"

#include <stdio.h>

wifi_join_failure_t wifi_join_failure_from_reason(uint16_t reason)
{
    wifi_join_failure_kind_t kind;
    switch (reason) {
    case WIFI_JOIN_REASON_AUTH_FAIL:
    case WIFI_JOIN_REASON_4WAY_HANDSHAKE_TIMEOUT:
    case WIFI_JOIN_REASON_HANDSHAKE_TIMEOUT:
    case WIFI_JOIN_REASON_MIC_FAILURE:
    case WIFI_JOIN_REASON_802_1X_AUTH_FAILED:
        kind = WIFI_JOIN_WRONG_PASSWORD;
        break;
    case WIFI_JOIN_REASON_NO_AP_FOUND:
    case WIFI_JOIN_REASON_NO_AP_FOUND_W_COMPATIBLE_SECURITY:
    case WIFI_JOIN_REASON_NO_AP_FOUND_IN_AUTHMODE_THRESHOLD:
    case WIFI_JOIN_REASON_NO_AP_FOUND_IN_RSSI_THRESHOLD:
        kind = WIFI_JOIN_NOT_FOUND;
        break;
    case WIFI_JOIN_REASON_ASSOC_FAIL:
    case WIFI_JOIN_REASON_ASSOC_TOOMANY:
    case WIFI_JOIN_REASON_ASSOC_COMEBACK_TIME_TOO_LONG:
        kind = WIFI_JOIN_REFUSED;
        break;
    default:
        kind = WIFI_JOIN_OTHER;
        break;
    }
    return (wifi_join_failure_t){.kind = kind, .reason = reason};
}

wifi_join_failure_t wifi_join_failure_timeout(bool associated)
{
    return (wifi_join_failure_t){
        .kind = associated ? WIFI_JOIN_NO_ADDRESS : WIFI_JOIN_TIMED_OUT,
        .reason = 0,
    };
}

size_t wifi_join_failure_text(wifi_join_failure_t failure, char *text, size_t capacity)
{
    const char *fixed = NULL;
    switch (failure.kind) {
    case WIFI_JOIN_OK: fixed = ""; break;
    case WIFI_JOIN_WRONG_PASSWORD: fixed = "Wrong password"; break;
    case WIFI_JOIN_NOT_FOUND: fixed = "Network not found"; break;
    case WIFI_JOIN_NO_ADDRESS: fixed = "No IP address"; break;
    case WIFI_JOIN_REFUSED: fixed = "Network refused"; break;
    case WIFI_JOIN_TIMED_OUT: fixed = "No answer"; break;
    case WIFI_JOIN_CANCELLED: fixed = "Cancelled"; break;
    case WIFI_JOIN_OTHER: break;
    }
    if (capacity == 0U) return 0;
    int length = fixed != NULL || failure.reason == 0U ?
                     snprintf(text, capacity, "%s", fixed != NULL ? fixed : "Could not connect") :
                     snprintf(text, capacity, "Wi-Fi error %u", (unsigned)failure.reason);
    if (length < 0) {
        text[0] = '\0';
        return 0;
    }
    return (size_t)length < capacity ? (size_t)length : capacity - 1U;
}
