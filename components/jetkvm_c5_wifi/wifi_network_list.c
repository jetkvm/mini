#include "wifi_network_list.h"

#include <string.h>

void wifi_network_list_init(wifi_network_list_t *list)
{
    memset(list, 0, sizeof(*list));
    list->version = WIFI_NETWORK_LIST_VERSION;
}

static size_t bounded_length(const uint8_t *text, size_t capacity)
{
    const uint8_t *end = memchr(text, 0, capacity);
    return end != NULL ? (size_t)(end - text) : capacity;
}

static bool is_hex_digit(uint8_t c)
{
    return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') ||
           (c >= 'A' && c <= 'F');
}

bool wifi_network_password_valid(const uint8_t *password, size_t length)
{
    if (length == 0U) return true;
    if (password == NULL) return false;
    if (length == WIFI_NETWORK_PASSWORD_MAX) {
        for (size_t i = 0; i < length; ++i)
            if (!is_hex_digit(password[i])) return false;
        return true;
    }
    if (length < 8U || length > 63U) return false;
    for (size_t i = 0; i < length; ++i)
        if (password[i] < 0x20U || password[i] > 0x7eU) return false;
    return true;
}

static bool ssid_valid(const uint8_t *ssid, size_t ssid_len)
{
    return ssid != NULL && ssid_len > 0U && ssid_len <= WIFI_NETWORK_SSID_MAX &&
           memchr(ssid, 0, ssid_len) == NULL;
}

int wifi_network_list_find(const wifi_network_list_t *list,
                           const uint8_t *ssid, size_t ssid_len)
{
    if (!ssid_valid(ssid, ssid_len)) return -1;
    for (uint8_t i = 0; i < list->count; ++i) {
        const wifi_saved_network_t *item = &list->items[i];
        if (item->ssid_len == ssid_len && memcmp(item->ssid, ssid, ssid_len) == 0)
            return i;
    }
    return -1;
}

wifi_network_list_result_t wifi_network_list_upsert(
    wifi_network_list_t *list, const uint8_t *ssid, size_t ssid_len,
    const uint8_t *password, size_t password_len)
{
    if (!ssid_valid(ssid, ssid_len) ||
        !wifi_network_password_valid(password, password_len)) {
        return WIFI_NETWORK_LIST_INVALID;
    }
    int index = wifi_network_list_find(list, ssid, ssid_len);
    if (index < 0) {
        if (list->count >= WIFI_NETWORK_LIST_MAX) return WIFI_NETWORK_LIST_FULL;
        index = list->count++;
    }
    wifi_saved_network_t *item = &list->items[index];
    memset(item, 0, sizeof(*item));
    memcpy(item->ssid, ssid, ssid_len);
    item->ssid_len = (uint8_t)ssid_len;
    if (password_len > 0U) memcpy(item->password, password, password_len);
    item->password_len = (uint8_t)password_len;
    return WIFI_NETWORK_LIST_OK;
}

wifi_network_list_result_t wifi_network_list_remove(
    wifi_network_list_t *list, const uint8_t *ssid, size_t ssid_len)
{
    int index = wifi_network_list_find(list, ssid, ssid_len);
    if (index < 0) return WIFI_NETWORK_LIST_NOT_FOUND;
    if (list->count == 1U) return WIFI_NETWORK_LIST_LAST_NETWORK;
    return wifi_network_list_forget(list, ssid, ssid_len);
}

wifi_network_list_result_t wifi_network_list_forget(
    wifi_network_list_t *list, const uint8_t *ssid, size_t ssid_len)
{
    int index = wifi_network_list_find(list, ssid, ssid_len);
    if (index < 0) return WIFI_NETWORK_LIST_NOT_FOUND;
    memmove(&list->items[index], &list->items[index + 1],
            (size_t)(list->count - 1U - (uint8_t)index) * sizeof(list->items[0]));
    --list->count;
    memset(&list->items[list->count], 0, sizeof(list->items[0]));
    return WIFI_NETWORK_LIST_OK;
}

bool wifi_network_security_matches(wifi_network_security_t security, bool has_password)
{
    return has_password ? security == WIFI_NETWORK_SECURITY_PSK
                        : security == WIFI_NETWORK_SECURITY_OPEN;
}

size_t wifi_network_list_candidates(const wifi_network_list_t *list,
                                    const wifi_network_seen_t *seen,
                                    size_t seen_count,
                                    wifi_network_candidate_t *out,
                                    size_t out_capacity)
{
    bool visible[WIFI_NETWORK_LIST_MAX] = {false};
    int8_t best[WIFI_NETWORK_LIST_MAX];
    uint8_t channel[WIFI_NETWORK_LIST_MAX];
    for (size_t i = 0; i < seen_count; ++i) {
        size_t len = bounded_length(seen[i].ssid, WIFI_NETWORK_SSID_MAX);
        int index = wifi_network_list_find(list, seen[i].ssid, len);
        if (index < 0) continue;
        /* An AP with the saved name but other security cannot be joined with
         * the saved password (or its absence). */
        if (!wifi_network_security_matches(seen[i].security,
                                           list->items[index].password_len > 0U)) continue;
        if (!visible[index] || seen[i].rssi > best[index]) {
            best[index] = seen[i].rssi;
            channel[index] = seen[i].channel;
        }
        visible[index] = true;
    }

    size_t count = 0;
    for (uint8_t i = 0; i < list->count && count < out_capacity; ++i) {
        if (!visible[i]) continue;
        /* Insertion sort: at most eight entries. */
        size_t at = count++;
        while (at > 0U && out[at - 1U].rssi < best[i]) {
            out[at] = out[at - 1U];
            --at;
        }
        out[at] = (wifi_network_candidate_t){
            .index = i, .rssi = best[i], .channel = channel[i]};
    }
    return count;
}

bool wifi_network_list_decode(wifi_network_list_t *list, const void *blob,
                              size_t length)
{
    wifi_network_list_init(list);
    if (blob == NULL || length != sizeof(*list)) return false;
    wifi_network_list_t stored;
    memcpy(&stored, blob, sizeof(stored));
    if (stored.version != WIFI_NETWORK_LIST_VERSION ||
        stored.count > WIFI_NETWORK_LIST_MAX) {
        return false;
    }
    for (uint8_t i = 0; i < stored.count; ++i) {
        const wifi_saved_network_t *item = &stored.items[i];
        if (item->ssid_len > WIFI_NETWORK_SSID_MAX ||
            item->password_len > WIFI_NETWORK_PASSWORD_MAX ||
            wifi_network_list_upsert(list, item->ssid, item->ssid_len,
                                     item->password, item->password_len) !=
                WIFI_NETWORK_LIST_OK) {
            wifi_network_list_init(list);
            return false;
        }
    }
    return true;
}

bool wifi_network_list_from_legacy(wifi_network_list_t *list,
                                   const uint8_t ssid[WIFI_NETWORK_SSID_MAX],
                                   const uint8_t password[WIFI_NETWORK_PASSWORD_MAX])
{
    wifi_network_list_init(list);
    size_t ssid_len = bounded_length(ssid, WIFI_NETWORK_SSID_MAX);
    size_t password_len = bounded_length(password, WIFI_NETWORK_PASSWORD_MAX);
    return wifi_network_list_upsert(list, ssid, ssid_len, password,
                                    password_len) == WIFI_NETWORK_LIST_OK;
}
