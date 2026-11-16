#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "wifi_join_failure.h"

/* The text a disconnect with `reason` gives. */
static const char *reason_text(uint16_t reason)
{
    static char text[WIFI_JOIN_FAILURE_TEXT_CAPACITY];
    (void)wifi_join_failure_text(wifi_join_failure_from_reason(reason), text, sizeof(text));
    return text;
}

static void test_wrong_password(void)
{
    /* The handshake failures: AUTH_FAIL, 4WAY_HANDSHAKE_TIMEOUT,
     * HANDSHAKE_TIMEOUT, MIC_FAILURE and 802_1X_AUTH_FAILED. */
    const uint16_t reasons[] = {202, 15, 204, 14, 23};
    for (size_t i = 0; i < sizeof(reasons) / sizeof(reasons[0]); ++i) {
        assert(wifi_join_failure_from_reason(reasons[i]).kind == WIFI_JOIN_WRONG_PASSWORD);
        assert(strcmp(reason_text(reasons[i]), "Wrong password") == 0);
    }
}

static void test_no_answer_to_an_unproven_password_is_a_wrong_password(void)
{
    char text[WIFI_JOIN_FAILURE_TEXT_CAPACITY];
    /* A WPA3 AP that drops a wrong confirm: AUTH_EXPIRE, twice. */
    wifi_join_failure_t twice = wifi_join_failure_after_retry(2, 2, true);
    assert(twice.kind == WIFI_JOIN_WRONG_PASSWORD);
    (void)wifi_join_failure_text(twice, text, sizeof(text));
    assert(strcmp(text, "Wrong password") == 0);
    /* The AP rejected the first attempt (AUTH_FAIL, 4WAY_HANDSHAKE_TIMEOUT)
     * and gave no answer to the second. */
    assert(wifi_join_failure_after_retry(202, 2, true).kind == WIFI_JOIN_WRONG_PASSWORD);
    assert(wifi_join_failure_after_retry(15, 2, true).kind == WIFI_JOIN_WRONG_PASSWORD);
    /* One AUTH_EXPIRE beside another reason is as the second reason alone. */
    wifi_join_failure_t once = wifi_join_failure_after_retry(205, 2, true);
    assert(once.kind == WIFI_JOIN_OTHER && once.reason == 2);
    assert(wifi_join_failure_after_retry(2, 205, true).kind == WIFI_JOIN_OTHER);
    assert(wifi_join_failure_after_retry(2, 201, true).kind == WIFI_JOIN_NOT_FOUND);
    assert(wifi_join_failure_after_retry(2, 203, true).kind == WIFI_JOIN_REFUSED);
}

static void test_no_answer_to_a_proven_password_shows_its_number(void)
{
    char text[WIFI_JOIN_FAILURE_TEXT_CAPACITY];
    /* A saved network whose AP is out of reach or busy, or an open network:
     * the same two AUTH_EXPIRE do not blame the password. */
    wifi_join_failure_t twice = wifi_join_failure_after_retry(2, 2, false);
    assert(twice.kind == WIFI_JOIN_OTHER && twice.reason == 2);
    (void)wifi_join_failure_text(twice, text, sizeof(text));
    assert(strcmp(text, "Wi-Fi error 2") == 0);
    assert(wifi_join_failure_after_retry(202, 2, false).kind == WIFI_JOIN_OTHER);
}

static void test_not_found(void)
{
    const uint16_t reasons[] = {201, 210, 211, 212};
    for (size_t i = 0; i < sizeof(reasons) / sizeof(reasons[0]); ++i) {
        assert(wifi_join_failure_from_reason(reasons[i]).kind == WIFI_JOIN_NOT_FOUND);
        assert(strcmp(reason_text(reasons[i]), "Network not found") == 0);
    }
}

static void test_refused(void)
{
    const uint16_t reasons[] = {203, 5, 208};
    for (size_t i = 0; i < sizeof(reasons) / sizeof(reasons[0]); ++i) {
        assert(wifi_join_failure_from_reason(reasons[i]).kind == WIFI_JOIN_REFUSED);
        assert(strcmp(reason_text(reasons[i]), "Network refused") == 0);
    }
}

static void test_other_reasons_show_their_number(void)
{
    /* 2 AUTH_EXPIRE and 205 CONNECTION_FAIL are transient on the bench; 39
     * TIMEOUT and 200 BEACON_TIMEOUT are a lost link. */
    assert(wifi_join_failure_from_reason(2).kind == WIFI_JOIN_OTHER);
    assert(wifi_join_failure_from_reason(2).reason == 2);
    assert(strcmp(reason_text(2), "Wi-Fi error 2") == 0);
    assert(strcmp(reason_text(205), "Wi-Fi error 205") == 0);
    assert(strcmp(reason_text(39), "Wi-Fi error 39") == 0);
    assert(strcmp(reason_text(200), "Wi-Fi error 200") == 0);
    /* The largest reason fits the text capacity. */
    assert(strcmp(reason_text(65535), "Wi-Fi error 65535") == 0);
    /* A failure without a reason (the connect request failed). */
    char text[WIFI_JOIN_FAILURE_TEXT_CAPACITY];
    wifi_join_failure_t none = {.kind = WIFI_JOIN_OTHER, .reason = 0};
    (void)wifi_join_failure_text(none, text, sizeof(text));
    assert(strcmp(text, "Could not connect") == 0);
}

static void test_timeouts(void)
{
    char text[WIFI_JOIN_FAILURE_TEXT_CAPACITY];
    /* Associated, but DHCP gave no address. */
    wifi_join_failure_t dhcp = wifi_join_failure_timeout(true);
    assert(dhcp.kind == WIFI_JOIN_NO_ADDRESS && dhcp.reason == 0);
    (void)wifi_join_failure_text(dhcp, text, sizeof(text));
    assert(strcmp(text, "No IP address") == 0);
    /* Never associated. */
    wifi_join_failure_t silent = wifi_join_failure_timeout(false);
    assert(silent.kind == WIFI_JOIN_TIMED_OUT);
    (void)wifi_join_failure_text(silent, text, sizeof(text));
    assert(strcmp(text, "No answer") == 0);
}

static void test_ok_cancelled_and_capacity(void)
{
    char text[WIFI_JOIN_FAILURE_TEXT_CAPACITY];
    wifi_join_failure_t ok = {.kind = WIFI_JOIN_OK, .reason = 0};
    assert(wifi_join_failure_text(ok, text, sizeof(text)) == 0 && text[0] == '\0');
    wifi_join_failure_t cancelled = {.kind = WIFI_JOIN_CANCELLED, .reason = 0};
    (void)wifi_join_failure_text(cancelled, text, sizeof(text));
    assert(strcmp(text, "Cancelled") == 0);
    /* A short buffer gets a cut, terminated text and the length written. */
    char small[6];
    assert(wifi_join_failure_text(wifi_join_failure_from_reason(202), small, sizeof(small)) == 5);
    assert(strcmp(small, "Wrong") == 0);
    assert(wifi_join_failure_text(ok, small, 0) == 0);
}

int main(void)
{
    test_wrong_password();
    test_no_answer_to_an_unproven_password_is_a_wrong_password();
    test_no_answer_to_a_proven_password_shows_its_number();
    test_not_found();
    test_refused();
    test_other_reasons_show_their_number();
    test_timeouts();
    test_ok_cancelled_and_capacity();
    printf("wifi join failure: PASS\n");
    return 0;
}
