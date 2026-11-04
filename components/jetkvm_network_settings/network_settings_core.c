#include "jetkvm_network_settings.h"
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

static void default_hostname(jetkvm_network_settings_t *settings, const char *device_id)
{
    if (device_id && device_id[0]) {
        snprintf(settings->hostname, sizeof(settings->hostname), "jetkvm-%s", device_id);
    } else {
        strcpy(settings->hostname, "jetkvm");
    }
}
void jetkvm_network_settings_defaults(jetkvm_network_settings_t *settings, const char *device_id)
{
    memset(settings, 0, sizeof(*settings));
    default_hostname(settings, device_id);
    settings->dhcp = true;
}
bool jetkvm_network_settings_fill_hostname(jetkvm_network_settings_t *settings, const char *device_id)
{
    if (settings->hostname[0]) return false;
    default_hostname(settings, device_id);
    return true;
}
bool jetkvm_network_settings_migrate_hostname(jetkvm_network_settings_t *settings, const char *device_id)
{
    /* Factory hostname of firmware that predates the upstream default. */
    if (strcmp(settings->hostname, "esp32p4") != 0) return false;
    default_hostname(settings, device_id);
    return true;
}

static bool ipv4(const char *text, uint32_t *address)
{
    uint32_t result = 0;
    for (unsigned i = 0; i < 4; ++i) {
        const char *start = text;
        unsigned part = 0;
        while (*text >= '0' && *text <= '9') {
            part = part * 10 + (unsigned)(*text++ - '0');
            if (part > 255 || text - start > 3) return false;
        }
        if (start == text || (text - start > 1 && *start == '0')) return false;
        result = (result << 8) | part;
        if (i == 3 ? *text != '\0' : *text++ != '.') return false;
    }
    *address = result;
    return true;
}
static bool unicast(uint32_t address)
{
    return (address >> 24) != 0 && (address >> 24) != 127 && (address >> 28) < 14;
}
static bool hostname_char(char ch)
{
    return (ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') ||
           (ch >= '0' && ch <= '9');
}
static bool server_char(char ch)
{
    return hostname_char(ch) || ch == '-' || ch == '.';
}
static bool ntp_server(const char *text)
{
    size_t length = strlen(text);
    if (!length || length >= JETKVM_TIME_SYNC_SERVER_CAPACITY ||
        !hostname_char(text[0]) || !hostname_char(text[length - 1])) return false;
    for (size_t i = 0; i < length; ++i) {
        if (!server_char(text[i])) return false;
        /* Labels are non-empty and neither start nor end with a hyphen. */
        if (text[i] == '.' && (text[i - 1] == '-' || text[i + 1] == '.' || text[i + 1] == '-')) return false;
    }
    return true;
}
bool jetkvm_network_settings_from_blob(const void *blob, size_t size, jetkvm_network_settings_t *settings)
{
    if (!blob || !settings || (size != sizeof(*settings) && size != JETKVM_NETWORK_SETTINGS_LEGACY_SIZE)) return false;
    memset(settings, 0, sizeof(*settings));
    memcpy(settings, blob, size);
    return jetkvm_network_settings_validate(settings) == NULL;
}
bool jetkvm_network_settings_static_applies(const jetkvm_network_settings_t *settings,
                                            const char *static_ssid, const char *wifi_ssid)
{
    if (!settings || settings->dhcp) return false;
    if (!static_ssid || static_ssid[0] == '\0' || !wifi_ssid) return true;
    return strcmp(static_ssid, wifi_ssid) == 0;
}
bool jetkvm_network_settings_connectivity_equal(const jetkvm_network_settings_t *a,
                                                const jetkvm_network_settings_t *b)
{
    return memcmp(a, b, JETKVM_NETWORK_SETTINGS_LEGACY_SIZE) == 0;
}
const char *jetkvm_network_settings_validate(const jetkvm_network_settings_t *settings)
{
    if (!settings || !memchr(settings->hostname, 0, sizeof(settings->hostname)) ||
        !memchr(settings->address, 0, sizeof(settings->address)) ||
        !memchr(settings->netmask, 0, sizeof(settings->netmask)) ||
        !memchr(settings->gateway, 0, sizeof(settings->gateway))) return "Invalid network settings";
    /* A persisted blob must contain a valid C bool representation before read. */
    unsigned char dhcp = ((const unsigned char *)settings)[offsetof(jetkvm_network_settings_t, dhcp)];
    if (dhcp > 1) return "Invalid IPv4 mode";
    size_t length = strlen(settings->hostname);
    if (!length || length > JETKVM_NETWORK_HOSTNAME_MAX_LENGTH || !hostname_char(settings->hostname[0]) ||
        !hostname_char(settings->hostname[length - 1])) return "Invalid hostname";
    for (size_t i = 0; i < length; ++i) {
        if (!hostname_char(settings->hostname[i]) && settings->hostname[i] != '-') return "Invalid hostname";
    }
    for (size_t i = 0; i < 3; ++i) {
        if (!memchr(settings->dns[i], 0, sizeof(settings->dns[i]))) return "Invalid DNS server";
    }
    const unsigned char *bytes = (const unsigned char *)settings;
    if (bytes[offsetof(jetkvm_network_settings_t, time_sync_custom)] > 1 ||
        bytes[offsetof(jetkvm_network_settings_t, time_sync_disable_fallback)] > 1) return "Invalid time sync mode";
    for (size_t i = 0; i < JETKVM_TIME_SYNC_MAX_SERVERS; ++i) {
        if (!memchr(settings->time_sync_ntp_servers[i], 0, sizeof(settings->time_sync_ntp_servers[i])) ||
            (settings->time_sync_ntp_servers[i][0] && !ntp_server(settings->time_sync_ntp_servers[i])))
            return "NTP server must be a hostname or IPv4 address";
    }
    /* DHCP leases need not satisfy our static subnet policy. Only require
     * bounded strings in inactive persisted fields so they remain safe to read. */
    if (settings->dhcp) return NULL;
    for (size_t i = 0; i < 3; ++i) {
        if (settings->dns[i][0]) {
            uint32_t dns;
            if (!ipv4(settings->dns[i], &dns) || !unicast(dns)) return "Invalid DNS server";
        }
    }
    uint32_t address, mask, gateway = 0;
    if (!ipv4(settings->address, &address) || !unicast(address))
        return "Static IPv4 address must be a dotted-decimal unicast address";
    if (!ipv4(settings->netmask, &mask))
        return "Netmask must use dotted-decimal notation";
    uint32_t hostmask = ~mask;
    /* lwIP Ethernet treats the other /31 endpoint as broadcast. /32 requires
     * a separate off-subnet gateway policy. Enforce /1–/30 in firmware. */
    if (!mask || hostmask < 3 || (hostmask & (hostmask + 1)))
        return "Mini supports contiguous netmasks from /1 to /30";
    if ((address & hostmask) == 0 || (address & hostmask) == hostmask)
        return "Static IPv4 address must not be the subnet or broadcast address";
    if (settings->gateway[0] &&
        (!ipv4(settings->gateway, &gateway) || (gateway &&
         (!unicast(gateway) || (gateway & mask) != (address & mask) ||
          (gateway & hostmask) == 0 || (gateway & hostmask) == hostmask || gateway == address)))) {
        return "Gateway must be a different host in the static subnet";
    }
    return NULL;
}

int64_t jetkvm_network_lease_expiry(jetkvm_network_lease_clock_t *clock,
    uint32_t remaining, bool renewed, uint32_t coarse_seconds,
    int64_t now_us, int64_t wall_seconds)
{
    /* A sampling gap can hide an automatic renewal whose new age exceeds
     * the previous sample. Use the configured lwIP coarse timer interval. */
    if (!clock->initialized || renewed || now_us - clock->sampled_us > (int64_t)coarse_seconds * 2000000) {
        clock->deadline_us = now_us + (int64_t)remaining * 1000000;
        clock->expiry = 0;
        clock->initialized = true;
    }
    clock->sampled_us = now_us;
    if (wall_seconds <= 1700000000) return 0;
    int64_t offset = wall_seconds - now_us / 1000000;
    if (!clock->expiry || offset > clock->wall_offset + 2 || offset < clock->wall_offset - 2) {
        clock->expiry = wall_seconds + (clock->deadline_us > now_us ?
            (clock->deadline_us - now_us) / 1000000 : 0);
        clock->wall_offset = offset;
    }
    return clock->expiry;
}
