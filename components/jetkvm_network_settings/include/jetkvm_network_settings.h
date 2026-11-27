#pragma once
#include <stddef.h>
#include "esp_netif.h"
#include "jetkvm_control.h"

/* ESP-IDF reserves one byte of its 32-byte hostname buffer for the terminator. */
#define JETKVM_NETWORK_HOSTNAME_MAX_LENGTH 31U

const char *jetkvm_network_settings_validate(const jetkvm_network_settings_t *settings);
/* DHCP with upstream's default hostname "jetkvm-<device ID>" ("jetkvm" without an ID). */
void jetkvm_network_settings_defaults(jetkvm_network_settings_t *settings, const char *device_id);
/* An empty hostname becomes the default, so saved settings always name the host; true when changed. */
bool jetkvm_network_settings_fill_hostname(jetkvm_network_settings_t *settings, const char *device_id);
/* The old factory hostname "esp32p4" becomes the default; true when changed. */
bool jetkvm_network_settings_migrate_hostname(jetkvm_network_settings_t *settings, const char *device_id);
/* Persisted blobs written before the time-sync fields, before mdns_mode or
 * before ipv6_mode existed keep loading. */
#define JETKVM_NETWORK_SETTINGS_LEGACY_SIZE offsetof(jetkvm_network_settings_t, time_sync_custom)
#define JETKVM_NETWORK_SETTINGS_TIME_SYNC_SIZE offsetof(jetkvm_network_settings_t, mdns_mode)
#define JETKVM_NETWORK_SETTINGS_MDNS_SIZE offsetof(jetkvm_network_settings_t, ipv6_mode)
bool jetkvm_network_settings_from_blob(const void *blob, size_t size, jetkvm_network_settings_t *settings);
/* True when only time-sync fields, mdns_mode or ipv6_mode differ, so IPv4
 * needs no reconfiguration. */
bool jetkvm_network_settings_connectivity_equal(const jetkvm_network_settings_t *a,
                                                const jetkvm_network_settings_t *b);
/*
 * Static IPv4 settings belong to one Wi-Fi network. `static_ssid` is that
 * network ("" applies them everywhere, as on Ethernet); `wifi_ssid` is the
 * network the station joins, or NULL when no Wi-Fi network is known. On any
 * other network the station uses DHCP.
 */
bool jetkvm_network_settings_static_applies(const jetkvm_network_settings_t *settings,
                                            const char *static_ssid, const char *wifi_ssid);

esp_err_t jetkvm_network_settings_init(esp_netif_t *netif);
/* The interface takes an ipv6_mode other than "disabled": the Ethernet
 * interface of a build with CONFIG_LWIP_IPV6_AUTOCONFIG. */
bool jetkvm_network_settings_ipv6_supported(void);
/* The interface has an IPv6 address that is not link-local and that it
 * prefers: an address that a WebRTC session can offer to the browser. */
bool jetkvm_network_ipv6_routed(void);
/* The IPv6 address to show for the interface: such an address, else the
 * link-local address, else "". */
void jetkvm_network_ipv6_address(char *address, size_t capacity);
/* The link of the interface came up. lwIP drops the IPv6 addresses of an
 * interface that goes down, so this applies ipv6_mode again. */
void jetkvm_network_settings_link_up(void);
/* `listener` gets the settings in effect each time a save changes them: after
 * the interface applied them (or rolled them back), and after a save of only
 * time-sync fields or mdns_mode. It runs with the settings lock held, so it
 * must not call back into this module. */
void jetkvm_network_settings_set_listener(void (*listener)(const jetkvm_network_settings_t *settings));
/* The Wi-Fi station is about to join `ssid`; re-applies IPv4 when the static
 * settings start or stop applying. */
void jetkvm_network_settings_set_wifi_network(const char *ssid);
/* Copy the network the static settings belong to ("" when none or DHCP). */
void jetkvm_network_settings_static_network(char *ssid, size_t capacity);
/* Upgrade from one saved network: bind unbound static settings to it. */
esp_err_t jetkvm_network_settings_bind_static_if_unbound(const char *ssid);
bool jetkvm_network_settings_get(jetkvm_network_settings_t *settings, void *ctx);
const char *jetkvm_network_settings_set(const jetkvm_network_settings_t *settings, void *ctx);
const char *jetkvm_network_renew_dhcp(void *ctx);
void jetkvm_network_snapshot(jetkvm_network_state_t *state);

/* Monotonic lease estimate; resampling after an idle UI must see renewals. */
typedef struct {
    int64_t deadline_us, sampled_us, expiry, wall_offset;
    bool initialized;
} jetkvm_network_lease_clock_t;
int64_t jetkvm_network_lease_expiry(jetkvm_network_lease_clock_t *clock,
    uint32_t remaining, bool renewed, uint32_t coarse_seconds,
    int64_t now_us, int64_t wall_seconds);
