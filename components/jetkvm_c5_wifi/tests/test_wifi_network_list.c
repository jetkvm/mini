#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "wifi_network_list.h"

#define S(text) (const uint8_t *)(text), strlen(text)

static wifi_network_seen_t seen(const char *ssid, int8_t rssi, uint8_t channel)
{
    /* Test networks below are saved without a password (open). */
    wifi_network_seen_t record = {.rssi = rssi, .channel = channel,
                                  .security = WIFI_NETWORK_SECURITY_OPEN};
    memcpy(record.ssid, ssid, strlen(ssid));
    return record;
}

static void test_upsert_replaces_password_without_duplicate(void)
{
    wifi_network_list_t list;
    wifi_network_list_init(&list);
    assert(wifi_network_list_upsert(&list, S("Home"), S("password1")) == WIFI_NETWORK_LIST_OK);
    assert(wifi_network_list_upsert(&list, S("Office"), S("password2")) == WIFI_NETWORK_LIST_OK);
    assert(wifi_network_list_upsert(&list, S("Home"), S("changed-pw")) == WIFI_NETWORK_LIST_OK);
    assert(list.count == 2);
    assert(list.items[0].password_len == 10 && memcmp(list.items[0].password, "changed-pw", 10) == 0);
    /* SSIDs are case-sensitive byte strings. */
    assert(wifi_network_list_find(&list, S("home")) == -1);
}

static void test_limits(void)
{
    wifi_network_list_t list;
    wifi_network_list_init(&list);
    char ssid[8];
    for (unsigned i = 0; i < WIFI_NETWORK_LIST_MAX; ++i) {
        snprintf(ssid, sizeof(ssid), "net%u", i);
        assert(wifi_network_list_upsert(&list, S(ssid), S("")) == WIFI_NETWORK_LIST_OK);
    }
    assert(wifi_network_list_upsert(&list, S("ninth"), S("password")) == WIFI_NETWORK_LIST_FULL);
    /* A full list still accepts a password change for a saved network. */
    assert(wifi_network_list_upsert(&list, S("net3"), S("password")) == WIFI_NETWORK_LIST_OK);

    assert(wifi_network_list_upsert(&list, S(""), S("")) == WIFI_NETWORK_LIST_INVALID);
    assert(wifi_network_list_upsert(&list, S("123456789012345678901234567890123"), S("")) ==
           WIFI_NETWORK_LIST_INVALID);
    assert(wifi_network_list_upsert(&list, S("net1"), S("short")) == WIFI_NETWORK_LIST_INVALID);
}

static void test_passwords(void)
{
    assert(wifi_network_password_valid(S("")));
    assert(!wifi_network_password_valid(S("1234567")));
    assert(wifi_network_password_valid(S("12345678")));
    char text[65];
    memset(text, 'a', 63);
    text[63] = '\0';
    assert(wifi_network_password_valid(S(text)));
    memset(text, 'f', 64);
    text[64] = '\0';
    assert(wifi_network_password_valid(S(text))); /* 64 hex digits: raw PSK */
    text[10] = 'g';
    assert(!wifi_network_password_valid(S(text)));
    assert(!wifi_network_password_valid(S("pass\nword")));
}

static void test_remove(void)
{
    wifi_network_list_t list;
    wifi_network_list_init(&list);
    assert(wifi_network_list_upsert(&list, S("A"), S("")) == WIFI_NETWORK_LIST_OK);
    assert(wifi_network_list_upsert(&list, S("B"), S("")) == WIFI_NETWORK_LIST_OK);
    assert(wifi_network_list_upsert(&list, S("C"), S("")) == WIFI_NETWORK_LIST_OK);
    assert(wifi_network_list_remove(&list, S("B")) == WIFI_NETWORK_LIST_OK);
    assert(list.count == 2 && wifi_network_list_find(&list, S("C")) == 1);
    assert(wifi_network_list_remove(&list, S("B")) == WIFI_NETWORK_LIST_NOT_FOUND);
    assert(wifi_network_list_remove(&list, S("A")) == WIFI_NETWORK_LIST_OK);
    assert(wifi_network_list_remove(&list, S("C")) == WIFI_NETWORK_LIST_LAST_NETWORK);
    assert(list.count == 1);
    /* BLE setup may remove the last network. */
    assert(wifi_network_list_forget(&list, S("C")) == WIFI_NETWORK_LIST_OK);
    assert(list.count == 0U);
    assert(wifi_network_list_forget(&list, S("C")) == WIFI_NETWORK_LIST_NOT_FOUND);
}

static void test_candidates(void)
{
    wifi_network_list_t list;
    wifi_network_list_init(&list);
    assert(wifi_network_list_upsert(&list, S("Home"), S("")) == WIFI_NETWORK_LIST_OK);
    assert(wifi_network_list_upsert(&list, S("Parents"), S("")) == WIFI_NETWORK_LIST_OK);
    assert(wifi_network_list_upsert(&list, S("Closet"), S("")) == WIFI_NETWORK_LIST_OK);
    assert(wifi_network_list_upsert(&list, S("Office"), S("")) == WIFI_NETWORK_LIST_OK);

    const wifi_network_seen_t scan[] = {
        seen("Neighbour", -30, 1), seen("Home", -80, 6), seen("Office", -60, 11),
        seen("Home", -55, 36), /* second AP of the same network */
        seen("", -40, 1),      /* a hidden AP */
    };
    wifi_network_candidate_t out[WIFI_NETWORK_LIST_MAX];
    size_t n = wifi_network_list_candidates(&list, scan, sizeof(scan) / sizeof(scan[0]), out, 8);
    /* Closet is saved but not in range; it is not a candidate. */
    assert(n == 2);
    /* The channel is the strongest AP's, so the join can start there. */
    assert(out[0].index == 0 && out[0].rssi == -55 && out[0].channel == 36);
    assert(out[1].index == 3 && out[1].rssi == -60 && out[1].channel == 11);

    /* Nothing saved in range and nothing hidden: no candidates. */
    wifi_network_list_t away;
    wifi_network_list_init(&away);
    assert(wifi_network_list_upsert(&away, S("Parents"), S("")) == WIFI_NETWORK_LIST_OK);
    assert(wifi_network_list_candidates(&away, scan, 5, out, 8) == 0);

    /* The output capacity is honoured. */
    assert(wifi_network_list_candidates(&list, scan, 5, out, 1) == 1 && out[0].index == 0);

    /* Same SSID, other security: a stronger WPA AP does not hide the open
     * one, and a saved WPA network ignores an open AP with its name. */
    wifi_network_list_t mixed;
    wifi_network_list_init(&mixed);
    assert(wifi_network_list_upsert(&mixed, S("Cafe"), S("")) == WIFI_NETWORK_LIST_OK);
    assert(wifi_network_list_upsert(&mixed, S("Home"), S("password1")) == WIFI_NETWORK_LIST_OK);
    wifi_network_seen_t twins[] = {
        seen("Cafe", -40, 1), seen("Cafe", -70, 11), seen("Home", -30, 6), seen("Home", -60, 36),
    };
    twins[0].security = WIFI_NETWORK_SECURITY_PSK;  /* stronger Cafe AP needs a password */
    twins[3].security = WIFI_NETWORK_SECURITY_PSK;  /* only the weaker Home AP uses WPA */
    n = wifi_network_list_candidates(&mixed, twins, 4, out, 8);
    assert(n == 2);
    assert(out[0].index == 1 && out[0].rssi == -60 && out[0].channel == 36);
    assert(out[1].index == 0 && out[1].rssi == -70 && out[1].channel == 11);

    /* A stronger WEP or Enterprise AP with the name of a saved PSK network
     * takes no password the device can send; the PSK AP is the candidate. */
    wifi_network_seen_t other[] = {seen("Home", -30, 6), seen("Home", -50, 1), seen("Home", -65, 44)};
    other[0].security = WIFI_NETWORK_SECURITY_OTHER; /* Enterprise */
    other[1].security = WIFI_NETWORK_SECURITY_OTHER; /* WEP */
    other[2].security = WIFI_NETWORK_SECURITY_PSK;
    n = wifi_network_list_candidates(&mixed, other, 3, out, 8);
    assert(n == 1 && out[0].index == 1 && out[0].rssi == -65 && out[0].channel == 44);
    assert(!wifi_network_security_matches(WIFI_NETWORK_SECURITY_OTHER, true));
    assert(!wifi_network_security_matches(WIFI_NETWORK_SECURITY_OTHER, false));
}

static void test_decode(void)
{
    wifi_network_list_t list;
    wifi_network_list_init(&list);
    assert(wifi_network_list_upsert(&list, S("Home"), S("password1")) == WIFI_NETWORK_LIST_OK);
    wifi_network_list_t loaded;
    assert(wifi_network_list_decode(&loaded, &list, sizeof(list)));
    assert(loaded.count == 1 && wifi_network_list_find(&loaded, S("Home")) == 0);
    /* The reserved byte (a hidden flag in development builds) is ignored. */
    wifi_network_list_t flagged = list;
    flagged.items[0].reserved = 1;
    assert(wifi_network_list_decode(&loaded, &flagged, sizeof(flagged)) && loaded.count == 1 &&
           loaded.items[0].reserved == 0);

    assert(!wifi_network_list_decode(&loaded, &list, sizeof(list) - 1));
    assert(loaded.count == 0);

    wifi_network_list_t bad = list;
    bad.version = 2;
    assert(!wifi_network_list_decode(&loaded, &bad, sizeof(bad)) && loaded.count == 0);
    bad = list;
    bad.count = WIFI_NETWORK_LIST_MAX + 1;
    assert(!wifi_network_list_decode(&loaded, &bad, sizeof(bad)) && loaded.count == 0);
    bad = list;
    bad.items[0].password_len = 3; /* not a valid WPA passphrase */
    assert(!wifi_network_list_decode(&loaded, &bad, sizeof(bad)) && loaded.count == 0);
    bad = list;
    bad.items[0].ssid_len = 40;
    assert(!wifi_network_list_decode(&loaded, &bad, sizeof(bad)) && loaded.count == 0);
}

static void test_legacy(void)
{
    uint8_t ssid[32] = {0}, password[64] = {0};
    memcpy(ssid, "Home", 4);
    memcpy(password, "password1", 9);
    wifi_network_list_t list;
    assert(wifi_network_list_from_legacy(&list, ssid, password));
    assert(list.count == 1 && list.items[0].ssid_len == 4 && list.items[0].password_len == 9);

    /* A 32-byte SSID and a 64-digit PSK fill their arrays with no NUL. */
    memset(ssid, 'x', sizeof(ssid));
    memset(password, 'A', sizeof(password));
    assert(wifi_network_list_from_legacy(&list, ssid, password));
    assert(list.items[0].ssid_len == 32 && list.items[0].password_len == 64);

    memset(ssid, 0, sizeof(ssid));
    assert(!wifi_network_list_from_legacy(&list, ssid, password) && list.count == 0);
}

int main(void)
{
    test_upsert_replaces_password_without_duplicate();
    test_limits();
    test_passwords();
    test_remove();
    test_candidates();
    test_decode();
    test_legacy();
    puts("Wi-Fi saved network list: PASS");
    return 0;
}
