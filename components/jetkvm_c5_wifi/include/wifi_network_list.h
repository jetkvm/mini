#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* Saved station networks, independent of ESP-IDF so the host tests can run
 * the same code. The list is stored as one NVS blob. */

#define WIFI_NETWORK_LIST_MAX 8U
#define WIFI_NETWORK_LIST_VERSION 1U
#define WIFI_NETWORK_SSID_MAX 32U
#define WIFI_NETWORK_PASSWORD_MAX 64U

typedef struct {
    uint8_t ssid[WIFI_NETWORK_SSID_MAX];
    uint8_t ssid_len;
    uint8_t password[WIFI_NETWORK_PASSWORD_MAX];
    uint8_t password_len;
    uint8_t reserved; /* written as 0; ignored on load */
} wifi_saved_network_t;

typedef struct {
    uint8_t version;
    uint8_t count;
    wifi_saved_network_t items[WIFI_NETWORK_LIST_MAX];
} wifi_network_list_t;

typedef enum {
    WIFI_NETWORK_LIST_OK = 0,
    WIFI_NETWORK_LIST_INVALID,
    WIFI_NETWORK_LIST_FULL,
    WIFI_NETWORK_LIST_NOT_FOUND,
    WIFI_NETWORK_LIST_LAST_NETWORK,
} wifi_network_list_result_t;

/** How an AP authenticates, as far as a saved network cares: a network
 *  saved without a password joins OPEN APs, one with a password joins PSK
 *  APs, and no saved network joins OTHER (WEP, Enterprise, WAPI). */
typedef enum {
    WIFI_NETWORK_SECURITY_OPEN = 0,
    WIFI_NETWORK_SECURITY_PSK,
    WIFI_NETWORK_SECURITY_OTHER,
} wifi_network_security_t;

/** One access point from a scan. `ssid` is NUL-padded as in wifi_ap_record_t. */
typedef struct {
    uint8_t ssid[WIFI_NETWORK_SSID_MAX + 1U];
    int8_t rssi;
    uint8_t channel;
    wifi_network_security_t security;
} wifi_network_seen_t;

/** True when a network saved with (or without) a password can join an AP
 *  with this security. */
bool wifi_network_security_matches(wifi_network_security_t security, bool has_password);

typedef struct {
    uint8_t index;   /* into wifi_network_list_t.items */
    int8_t rssi;     /* strongest matching AP */
    uint8_t channel; /* channel of that AP */
} wifi_network_candidate_t;

void wifi_network_list_init(wifi_network_list_t *list);

/** Open networks have no password; WPA needs 8-63 characters or 64 hex digits. */
bool wifi_network_password_valid(const uint8_t *password, size_t length);

/** Index of the entry with this exact SSID, or -1. */
int wifi_network_list_find(const wifi_network_list_t *list,
                           const uint8_t *ssid, size_t ssid_len);

/** Add a network or replace the password of the one with this SSID. */
wifi_network_list_result_t wifi_network_list_upsert(
    wifi_network_list_t *list, const uint8_t *ssid, size_t ssid_len,
    const uint8_t *password, size_t password_len);

/** Remove a network. The last network cannot be removed. */
wifi_network_list_result_t wifi_network_list_remove(
    wifi_network_list_t *list, const uint8_t *ssid, size_t ssid_len);

/** Remove a network, also the last one. BLE setup does this: the device has
 *  a setup device connected then, and with no network it enters setup mode. */
wifi_network_list_result_t wifi_network_list_forget(
    wifi_network_list_t *list, const uint8_t *ssid, size_t ssid_len);

/**
 * Saved networks the scan saw, strongest first. Each appears once, with the
 * RSSI and channel of its strongest AP whose security matches the saved
 * network (a password, or open). Returns the number written to `out`.
 */
size_t wifi_network_list_candidates(const wifi_network_list_t *list,
                                    const wifi_network_seen_t *seen,
                                    size_t seen_count,
                                    wifi_network_candidate_t *out,
                                    size_t out_capacity);

/** Load a stored blob. Returns false (and an empty list) for a bad blob. */
bool wifi_network_list_decode(wifi_network_list_t *list, const void *blob,
                              size_t length);

/**
 * Build a one-entry list from the single-network record of older firmware
 * (the SSID and password arrays of wifi_sta_config_t, NUL-padded).
 */
bool wifi_network_list_from_legacy(wifi_network_list_t *list,
                                   const uint8_t ssid[WIFI_NETWORK_SSID_MAX],
                                   const uint8_t password[WIFI_NETWORK_PASSWORD_MAX]);
