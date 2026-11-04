/* OS Services RPC vectors. main() in test_jetkvm_control.c
 * runs test_os_services(); every default context installed here is removed
 * again before it returns. */
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cJSON.h"
#include "jetkvm_control.h"

void test_os_services(void);

/* Dispatches request and returns the reply, which the caller frees. */
static char *call(const jetkvm_control_context_t *context, const char *request)
{
    enum { CAPACITY = 128 * 1024 };
    char *output = malloc(CAPACITY);
    assert(output);
    size_t length = 0;
    assert(jetkvm_control_handle_rpc_with_context(
        context, (const uint8_t *)request, strlen(request), output, CAPACITY,
        &length) == JETKVM_CONTROL_RESPONSE_READY);
    assert(length == strlen(output));
    return output;
}

static void expect(const jetkvm_control_context_t *context, const char *request,
                   const char *expected)
{
    char *actual = call(context, request);
    if (strcmp(actual, expected) != 0) {
        fprintf(stderr, "\nMISMATCH for %s\n  actual:   %s\n  expected: %s\n",
                request, actual, expected);
    }
    assert(strcmp(actual, expected) == 0);
    free(actual);
}

/* Every fake checks that it gets the context's ctx. */
static int os_ctx;

/* A OS Services settings store as the OS Services component keeps it. */
static struct {
    bool video_enabled;
    bool clipboard_enabled;
    char resolution[16];
    bool get_fails;
    const char *refusal;
    unsigned get_calls, set_calls;
    char last_request[128];
} settings = { .resolution = "auto" };

static bool add_settings(cJSON *object)
{
    return cJSON_AddBoolToObject(object, "video_enabled", settings.video_enabled) &&
        cJSON_AddStringToObject(object, "resolution", settings.resolution) &&
        cJSON_AddBoolToObject(object, "clipboard_enabled", settings.clipboard_enabled);
}

static bool get_settings(cJSON *object, void *ctx)
{
    assert(ctx == &os_ctx);
    ++settings.get_calls;
    return !settings.get_fails && add_settings(object);
}

static const char *set_settings(const cJSON *request, cJSON *applied, void *ctx)
{
    assert(ctx == &os_ctx);
    ++settings.set_calls;
    char *printed = cJSON_PrintUnformatted(request);
    assert(printed && strlen(printed) < sizeof(settings.last_request));
    strcpy(settings.last_request, printed);
    cJSON_free(printed);
    if (settings.refusal) return settings.refusal;
    const cJSON *video = cJSON_GetObjectItemCaseSensitive(request, "video_enabled");
    const cJSON *clipboard = cJSON_GetObjectItemCaseSensitive(request, "clipboard_enabled");
    if (cJSON_IsBool(video)) settings.video_enabled = cJSON_IsTrue(video);
    if (cJSON_IsBool(clipboard)) settings.clipboard_enabled = cJSON_IsTrue(clipboard);
    return add_settings(applied) ? NULL : "out of memory";
}

/* The video state fields the OS Services component adds. */
static struct {
    bool fails;
    bool empty;
    unsigned calls;
} video_state;

static bool add_video_state(cJSON *object, void *ctx)
{
    assert(ctx == &os_ctx);
    ++video_state.calls;
    if (video_state.fails) return false;
    if (video_state.empty) return true;
    cJSON *bridge = NULL;
    return cJSON_AddStringToObject(object, "source", "physical") &&
        (bridge = cJSON_AddObjectToObject(object, "osServices")) != NULL &&
        cJSON_AddBoolToObject(bridge, "connected", true) &&
        cJSON_AddBoolToObject(bridge, "clipboard", false) &&
        cJSON_AddStringToObject(bridge, "fingerprint", "ab:cd") &&
        cJSON_AddBoolToObject(object, "guestTerminal", true);
}

/* The update relay's answer to guestUpdateStart. */
static struct {
    const char *reason;
    unsigned calls;
} updater;

static const char *update_start(void *ctx)
{
    assert(ctx == &os_ctx);
    ++updater.calls;
    return updater.reason;
}

static const jetkvm_os_services_context_t os_services = {
    .get_settings = get_settings,
    .set_settings = set_settings,
    .add_video_state = add_video_state,
    .update_start = update_start,
    .ctx = &os_ctx,
};

#define GUEST_FIELDS \
    ",\"source\":\"physical\",\"osServices\":{\"connected\":true," \
    "\"clipboard\":false,\"fingerprint\":\"ab:cd\"},\"guestTerminal\":true"

static void test_without_os_services(const jetkvm_control_context_t *context)
{
    expect(context, "{\"jsonrpc\":\"2.0\",\"method\":\"getOsServicesSettings\",\"id\":1}",
        "{\"jsonrpc\":\"2.0\",\"id\":1,\"error\":{\"code\":-32603,\"message\":\"Internal error\",\"data\":\"OS Services unavailable\"}}");
    expect(context, "{\"jsonrpc\":\"2.0\",\"method\":\"setOsServicesSettings\",\"params\":{\"settings\":{\"video_enabled\":true}},\"id\":2}",
        "{\"jsonrpc\":\"2.0\",\"id\":2,\"error\":{\"code\":-32603,\"message\":\"Internal error\",\"data\":\"OS Services unavailable\"}}");
    expect(context, "{\"jsonrpc\":\"2.0\",\"method\":\"guestUpdateStart\",\"id\":6}",
        "{\"jsonrpc\":\"2.0\",\"id\":6,\"error\":{\"code\":-32603,\"message\":\"Internal error\",\"data\":\"OS Services unavailable\"}}");
    expect(context, "{\"jsonrpc\":\"2.0\",\"method\":\"setOsServicesSettings\",\"params\":{},\"id\":3}",
        "{\"jsonrpc\":\"2.0\",\"id\":3,\"error\":{\"code\":-32602,\"message\":\"Invalid params\",\"data\":\"settings must be an object\"}}");
    /* Without the component nothing changes in the existing replies. */
    expect(context, "{\"jsonrpc\":\"2.0\",\"method\":\"getDeviceCapabilities\",\"id\":4}",
        "{\"jsonrpc\":\"2.0\",\"id\":4,\"result\":[]}");
    expect(context, "{\"jsonrpc\":\"2.0\",\"method\":\"getVideoState\",\"id\":5}",
        "{\"jsonrpc\":\"2.0\",\"id\":5,\"result\":{\"ready\":true,\"streaming\":1,\"width\":1920,\"height\":1080,\"fps\":60}}");
    char fields[8] = "stale";
    size_t length = 99;
    assert(jetkvm_control_render_guest_video_fields(fields, sizeof(fields), &length));
    assert(length == 0 && fields[0] == '\0');
}

static void test_settings(const jetkvm_control_context_t *context)
{
    const char *current =
        "{\"video_enabled\":false,\"resolution\":\"auto\",\"clipboard_enabled\":false}";
    char expected[512];
    /* A getter takes no params; an empty or null params object is fine. */
    const char *gets[] = {
        "{\"jsonrpc\":\"2.0\",\"method\":\"getOsServicesSettings\",\"id\":10}",
        "{\"jsonrpc\":\"2.0\",\"method\":\"getOsServicesSettings\",\"params\":{},\"id\":10}",
        "{\"jsonrpc\":\"2.0\",\"method\":\"getOsServicesSettings\",\"params\":null,\"id\":10}",
    };
    snprintf(expected, sizeof(expected), "{\"jsonrpc\":\"2.0\",\"id\":10,\"result\":%s}", current);
    for (size_t i = 0; i < sizeof(gets) / sizeof(gets[0]); ++i) expect(context, gets[i], expected);
    assert(settings.get_calls == 3);

    /* The settings object reaches the component as sent; the reply is
     * what it applied, omitted fields included. */
    expect(context,
        "{\"jsonrpc\":\"2.0\",\"method\":\"setOsServicesSettings\",\"params\":{\"settings\":{\"clipboard_enabled\":true}},\"id\":11}",
        "{\"jsonrpc\":\"2.0\",\"id\":11,\"result\":{\"video_enabled\":false,\"resolution\":\"auto\",\"clipboard_enabled\":true}}");
    assert(settings.set_calls == 1 && settings.clipboard_enabled);
    assert(strcmp(settings.last_request, "{\"clipboard_enabled\":true}") == 0);
    expect(context,
        "{\"jsonrpc\":\"2.0\",\"method\":\"setOsServicesSettings\",\"params\":{\"settings\":{}},\"id\":12}",
        "{\"jsonrpc\":\"2.0\",\"id\":12,\"result\":{\"video_enabled\":false,\"resolution\":\"auto\",\"clipboard_enabled\":true}}");
    assert(settings.set_calls == 2 && strcmp(settings.last_request, "{}") == 0);

    /* A refusal is the error's data; nothing is applied. */
    settings.refusal = "unsupported resolution: 8k";
    expect(context,
        "{\"jsonrpc\":\"2.0\",\"method\":\"setOsServicesSettings\",\"params\":{\"settings\":{\"resolution\":\"8k\",\"video_enabled\":true}},\"id\":13}",
        "{\"jsonrpc\":\"2.0\",\"id\":13,\"error\":{\"code\":-32603,\"message\":\"Internal error\",\"data\":\"unsupported resolution: 8k\"}}");
    assert(settings.set_calls == 3 && !settings.video_enabled);
    assert(strcmp(settings.last_request, "{\"resolution\":\"8k\",\"video_enabled\":true}") == 0);
    settings.refusal = NULL;

    const char *invalid[] = {
        "{\"jsonrpc\":\"2.0\",\"method\":\"setOsServicesSettings\",\"id\":14}",
        "{\"jsonrpc\":\"2.0\",\"method\":\"setOsServicesSettings\",\"params\":{},\"id\":14}",
        "{\"jsonrpc\":\"2.0\",\"method\":\"setOsServicesSettings\",\"params\":[{}],\"id\":14}",
        "{\"jsonrpc\":\"2.0\",\"method\":\"setOsServicesSettings\",\"params\":{\"settings\":true},\"id\":14}",
        "{\"jsonrpc\":\"2.0\",\"method\":\"setOsServicesSettings\",\"params\":{\"settings\":[]},\"id\":14}",
        "{\"jsonrpc\":\"2.0\",\"method\":\"setOsServicesSettings\",\"params\":{\"video_enabled\":true},\"id\":14}",
    };
    for (size_t i = 0; i < sizeof(invalid) / sizeof(invalid[0]); ++i) {
        expect(context, invalid[i],
            "{\"jsonrpc\":\"2.0\",\"id\":14,\"error\":{\"code\":-32602,\"message\":\"Invalid params\",\"data\":\"settings must be an object\"}}");
    }
    assert(settings.set_calls == 3);

    settings.get_fails = true;
    expect(context, gets[0],
        "{\"jsonrpc\":\"2.0\",\"id\":10,\"error\":{\"code\":-32603,\"message\":\"Internal error\",\"data\":\"OS Services unavailable\"}}");
    settings.get_fails = false;
}

static bool wifi_available(jetkvm_wifi_settings_t *wifi, void *ctx)
{
    (void)ctx;
    *wifi = (jetkvm_wifi_settings_t){ .available = true };
    return true;
}

static bool media_state(jetkvm_control_virtual_media_state_t *state, void *ctx)
{
    (void)state; (void)ctx;
    assert(!"capabilities only check that a provider is set");
    return false;
}

static void test_capabilities(const jetkvm_control_context_t *context)
{
    expect(context, "{\"jsonrpc\":\"2.0\",\"method\":\"getDeviceCapabilities\",\"id\":20}",
        "{\"jsonrpc\":\"2.0\",\"id\":20,\"result\":[\"os_services\"]}");
    /* It follows the board's capabilities, which keep their order. */
    jetkvm_control_context_t board = *context;
    board.get_wifi_settings = wifi_available;
    board.virtual_media.get_state = media_state;
    expect(&board, "{\"jsonrpc\":\"2.0\",\"method\":\"getDeviceCapabilities\",\"id\":21}",
        "{\"jsonrpc\":\"2.0\",\"id\":21,\"result\":[\"removable_storage\",\"wifi\",\"os_services\"]}");
    char event[256];
    size_t length = 0;
    assert(jetkvm_control_render_capabilities_notification(NULL, event, sizeof(event), &length));
    assert(strcmp(event, "{\"jsonrpc\":\"2.0\",\"method\":\"deviceCapabilities\",\"params\":[\"os_services\"]}") == 0);
    assert(length == strlen(event));

    /* Video state alone is not the settings page the capability shows. */
    jetkvm_os_services_context_t state_only = os_services;
    state_only.get_settings = NULL;
    state_only.set_settings = NULL;
    jetkvm_control_set_default_os_services_context(&state_only);
    expect(context, "{\"jsonrpc\":\"2.0\",\"method\":\"getDeviceCapabilities\",\"id\":22}",
        "{\"jsonrpc\":\"2.0\",\"id\":22,\"result\":[]}");
    jetkvm_control_set_default_os_services_context(&os_services);
}

static void test_video_state(const jetkvm_control_context_t *context)
{
    video_state.calls = 0;
    expect(context, "{\"jsonrpc\":\"2.0\",\"method\":\"getVideoState\",\"id\":30}",
        "{\"jsonrpc\":\"2.0\",\"id\":30,\"result\":{\"ready\":true,\"streaming\":1,\"width\":1920,\"height\":1080,\"fps\":60"
        GUEST_FIELDS "}}");
    jetkvm_control_context_t no_signal = *context;
    no_signal.video = (jetkvm_video_state_t){ .error = "no_signal" };
    expect(&no_signal, "{\"jsonrpc\":\"2.0\",\"method\":\"getVideoState\",\"id\":31}",
        "{\"jsonrpc\":\"2.0\",\"id\":31,\"result\":{\"ready\":false,\"streaming\":0,\"error\":\"no_signal\",\"width\":0,\"height\":0,\"fps\":0"
        GUEST_FIELDS "}}");
    assert(video_state.calls == 2);
    video_state.fails = true;
    expect(context, "{\"jsonrpc\":\"2.0\",\"method\":\"getVideoState\",\"id\":32}",
        "{\"jsonrpc\":\"2.0\",\"id\":32,\"error\":{\"code\":-32603,\"message\":\"Internal error\",\"data\":\"OS Services unavailable\"}}");
    video_state.fails = false;
}

static void test_video_fields(void)
{
    const char expected[] = GUEST_FIELDS;
    char fields[sizeof(expected) + 8];
    size_t length = 99;
    assert(jetkvm_control_render_guest_video_fields(fields, sizeof(fields), &length));
    assert(length == strlen(expected) && strcmp(fields, expected) == 0);
    /* Exactly enough room, then one byte short. */
    memset(fields, 'x', sizeof(fields));
    assert(jetkvm_control_render_guest_video_fields(fields, sizeof(expected), &length));
    assert(length == strlen(expected) && strcmp(fields, expected) == 0);
    memset(fields, 'x', sizeof(fields));
    assert(!jetkvm_control_render_guest_video_fields(fields, sizeof(expected) - 1U, &length));
    assert(length == 0 && fields[0] == '\0');

    /* Fields placed before the closing braces make a valid event. */
    char event[512] = "{\"jsonrpc\":\"2.0\",\"method\":\"videoInputState\",\"params\":{\"ready\":false";
    size_t base = strlen(event);
    assert(jetkvm_control_render_guest_video_fields(event + base, sizeof(event) - base - 2U, &length));
    strcpy(event + base + length, "}}");
    cJSON *parsed = cJSON_Parse(event);
    const cJSON *params = cJSON_GetObjectItemCaseSensitive(parsed, "params");
    assert(cJSON_IsFalse(cJSON_GetObjectItemCaseSensitive(params, "ready")));
    assert(strcmp(cJSON_GetObjectItemCaseSensitive(params, "source")->valuestring, "physical") == 0);
    assert(cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(params, "guestTerminal")));
    cJSON_Delete(parsed);

    /* A component with nothing to add adds nothing, not a lone comma. */
    video_state.empty = true;
    assert(jetkvm_control_render_guest_video_fields(fields, sizeof(fields), &length));
    assert(length == 0 && fields[0] == '\0');
    video_state.empty = false;
    video_state.fails = true;
    assert(!jetkvm_control_render_guest_video_fields(fields, sizeof(fields), &length));
    assert(length == 0 && fields[0] == '\0');
    video_state.fails = false;
    assert(!jetkvm_control_render_guest_video_fields(fields, 0, &length));
}

#define MAX_TEXT JETKVM_GUEST_CLIPBOARD_MAX_TEXT
#define REPLY_BUFFER (128U * 1024U) /* the RPC worker's large reply */

/* The sender as the OS Services component reaches it. */
typedef struct {
    const char *status;
    const char *reason;
    const char *text;
    size_t length;
    bool truncated;
    unsigned get_calls, set_calls;
    char set_text[MAX_TEXT + 1U];
    size_t set_length;
} fake_sender_t;
static fake_sender_t sender;

static const char *clipboard_get(char *text, size_t capacity, size_t *length,
                                 bool *truncated, const char **status, void *ctx)
{
    assert(ctx == &sender);
    assert(capacity == MAX_TEXT + 1U && sender.length <= capacity);
    ++sender.get_calls;
    memcpy(text, sender.text, sender.length);
    *length = sender.length;
    *truncated = sender.truncated;
    *status = sender.status;
    return sender.reason;
}

static const char *clipboard_set(const char *text, size_t length,
                                 const char **status, void *ctx)
{
    assert(ctx == &sender);
    assert(length <= MAX_TEXT && text[length] == '\0');
    ++sender.set_calls;
    memcpy(sender.set_text, text, length + 1U);
    sender.set_length = length;
    *status = sender.status;
    return sender.reason;
}

static const jetkvm_guest_clipboard_context_t clipboard = {
    .get = clipboard_get, .set = clipboard_set, .ctx = &sender,
};

/* A guestClipboardSet request for text, escaped by cJSON. */
static char *set_request(const char *text)
{
    cJSON *request = cJSON_CreateObject();
    cJSON *params = cJSON_AddObjectToObject(request, "params");
    assert(cJSON_AddStringToObject(request, "jsonrpc", "2.0") &&
           cJSON_AddStringToObject(request, "method", "guestClipboardSet") &&
           params && cJSON_AddStringToObject(params, "text", text) &&
           cJSON_AddNumberToObject(request, "id", 7));
    char *printed = cJSON_PrintUnformatted(request);
    cJSON_Delete(request);
    assert(printed);
    return printed;
}

/* Gets text through guestClipboardGet; returns the parsed result. */
static cJSON *clipboard_get_reply(const jetkvm_control_context_t *context,
                                  const char *text, size_t length)
{
    sender.status = "ok";
    sender.text = text;
    sender.length = length;
    char *reply = call(context, "{\"jsonrpc\":\"2.0\",\"method\":\"guestClipboardGet\",\"id\":1}");
    assert(strlen(reply) <= REPLY_BUFFER - 1U);
    cJSON *parsed = cJSON_Parse(reply);
    free(reply);
    assert(parsed);
    cJSON *result = cJSON_DetachItemFromObjectCaseSensitive(parsed, "result");
    cJSON_Delete(parsed);
    assert(cJSON_IsObject(result));
    assert(strcmp(cJSON_GetObjectItemCaseSensitive(result, "status")->valuestring, "ok") == 0);
    return result;
}

static const char *result_text(const cJSON *result)
{
    const cJSON *text = cJSON_GetObjectItemCaseSensitive(result, "text");
    assert(cJSON_IsString(text));
    return text->valuestring;
}

static bool result_truncated(const cJSON *result)
{
    const cJSON *truncated = cJSON_GetObjectItemCaseSensitive(result, "truncated");
    assert(cJSON_IsBool(truncated));
    return cJSON_IsTrue(truncated);
}

static void test_clipboard_unconnected(const jetkvm_control_context_t *context)
{
    const char *unconnected =
        "{\"jsonrpc\":\"2.0\",\"id\":1,\"error\":{\"code\":-32603,\"message\":\"Internal error\",\"data\":\"no OS Services clipboard is connected\"}}";
    expect(context, "{\"jsonrpc\":\"2.0\",\"method\":\"guestClipboardGet\",\"id\":1}", unconnected);
    expect(context, "{\"jsonrpc\":\"2.0\",\"method\":\"guestClipboardSet\",\"params\":{\"text\":\"hi\"},\"id\":1}", unconnected);
    /* Validation does not need a sender. */
    expect(context, "{\"jsonrpc\":\"2.0\",\"method\":\"guestClipboardSet\",\"params\":{},\"id\":1}",
        "{\"jsonrpc\":\"2.0\",\"id\":1,\"error\":{\"code\":-32602,\"message\":\"Invalid params\",\"data\":\"text must be a string\"}}");
    expect(context, "{\"jsonrpc\":\"2.0\",\"method\":\"guestClipboardSet\",\"params\":{\"text\":\"\xc0\xaf\"},\"id\":1}",
        "{\"jsonrpc\":\"2.0\",\"id\":1,\"error\":{\"code\":-32603,\"message\":\"Internal error\",\"data\":\"clipboard text is not valid UTF-8\"}}");
}

static void test_clipboard_get(const jetkvm_control_context_t *context)
{
    const char *get = "{\"jsonrpc\":\"2.0\",\"method\":\"guestClipboardGet\",\"id\":1}";
    sender = (fake_sender_t){ .status = "ok", .text = "h\xc3\xa9llo\n\"x\"\x01", .length = 11 };
    expect(context, get,
        "{\"jsonrpc\":\"2.0\",\"id\":1,\"result\":{\"status\":\"ok\",\"text\":\"h\xc3\xa9llo\\n\\\"x\\\"\\u0001\",\"truncated\":false}}");
    /* The sender cut the text itself. */
    sender.truncated = true;
    expect(context, "{\"jsonrpc\":\"2.0\",\"method\":\"guestClipboardGet\",\"params\":{},\"id\":2}",
        "{\"jsonrpc\":\"2.0\",\"id\":2,\"result\":{\"status\":\"ok\",\"text\":\"h\xc3\xa9llo\\n\\\"x\\\"\\u0001\",\"truncated\":true}}");
    sender.truncated = false;
    /* Text is present, and empty, whenever the status is not ok. */
    const char *statuses[] = { "unsupported", "empty", "denied", "failed" };
    for (size_t i = 0; i < sizeof(statuses) / sizeof(statuses[0]); ++i) {
        sender.status = statuses[i];
        char expected[256];
        snprintf(expected, sizeof(expected),
                 "{\"jsonrpc\":\"2.0\",\"id\":1,\"result\":{\"status\":\"%s\",\"text\":\"\",\"truncated\":false}}",
                 statuses[i]);
        expect(context, get, expected);
    }
    sender.status = NULL;
    expect(context, get,
        "{\"jsonrpc\":\"2.0\",\"id\":1,\"result\":{\"status\":\"failed\",\"text\":\"\",\"truncated\":false}}");
    /* No answer: the reason is the error's data. */
    sender.status = "ok";
    sender.reason = "the target computer's clipboard did not answer in time";
    expect(context, get,
        "{\"jsonrpc\":\"2.0\",\"id\":1,\"error\":{\"code\":-32603,\"message\":\"Internal error\",\"data\":\"the target computer's clipboard did not answer in time\"}}");
    sender.reason = NULL;
    /* cJSON strings end at NUL; what follows is cut. */
    sender.text = "ab\0cd";
    sender.length = 5;
    expect(context, get,
        "{\"jsonrpc\":\"2.0\",\"id\":1,\"result\":{\"status\":\"ok\",\"text\":\"ab\",\"truncated\":true}}");
    assert(sender.get_calls == 9);
}

static void test_clipboard_get_budget(const jetkvm_control_context_t *context)
{
    char *text = malloc(MAX_TEXT + 1U);
    assert(text);

    /* All ASCII: 64 KiB prints as 64 KiB and is not cut. */
    memset(text, 'a', MAX_TEXT);
    cJSON *result = clipboard_get_reply(context, text, MAX_TEXT);
    assert(strlen(result_text(result)) == MAX_TEXT && !result_truncated(result));
    assert(memcmp(result_text(result), text, MAX_TEXT) == 0);
    cJSON_Delete(result);

    /* Control characters print as \u00XX, six bytes each: the reply keeps
     * 120 KiB of escapes, 20480 characters, and says it was cut. */
    memset(text, 0x01, MAX_TEXT);
    result = clipboard_get_reply(context, text, MAX_TEXT);
    assert(strlen(result_text(result)) == 20480U && result_truncated(result));
    assert(memcmp(result_text(result), text, 20480U) == 0);
    cJSON_Delete(result);

    /* Quotes print as two bytes each. */
    memset(text, '"', MAX_TEXT);
    result = clipboard_get_reply(context, text, MAX_TEXT);
    assert(strlen(result_text(result)) == 61440U && result_truncated(result));
    cJSON_Delete(result);

    /* "ab", then \x01 and U+1F600 (6 + 4 printed bytes) repeated: the
     * budget runs out two bytes into an emoji, which is left out whole. */
    static const char group[] = "\x01\xf0\x9f\x98\x80";
    size_t length = 2U;
    memcpy(text, "ab", 2U);
    while (length + 5U <= MAX_TEXT) {
        memcpy(text + length, group, 5U);
        length += 5U;
    }
    result = clipboard_get_reply(context, text, length);
    const char *kept = result_text(result);
    assert(strlen(kept) == 2U + 12287U * 5U + 1U && result_truncated(result));
    assert(memcmp(kept, text, strlen(kept)) == 0);
    assert(kept[strlen(kept) - 1U] == '\x01');
    cJSON_Delete(result);

    /* A sender that reports more than the limit is cut to it. */
    memset(text, 'b', MAX_TEXT + 1U);
    result = clipboard_get_reply(context, text, MAX_TEXT + 1U);
    assert(strlen(result_text(result)) == MAX_TEXT && result_truncated(result));
    cJSON_Delete(result);
    free(text);
}

static void expect_set(const jetkvm_control_context_t *context, const char *text,
                       const char *expected)
{
    char *request = set_request(text);
    char *reply = call(context, request);
    if (strcmp(reply, expected) != 0) {
        fprintf(stderr, "\nMISMATCH for set of %zu bytes\n  actual:   %.300s\n  expected: %s\n",
                strlen(text), reply, expected);
    }
    assert(strcmp(reply, expected) == 0);
    free(reply);
    free(request);
}

static void test_clipboard_set(const jetkvm_control_context_t *context)
{
    const char *ok = "{\"jsonrpc\":\"2.0\",\"id\":7,\"result\":{\"status\":\"ok\"}}";
    const char *not_utf8 =
        "{\"jsonrpc\":\"2.0\",\"id\":7,\"error\":{\"code\":-32603,\"message\":\"Internal error\",\"data\":\"clipboard text is not valid UTF-8\"}}";
    sender = (fake_sender_t){ .status = "ok" };
    expect_set(context, "h\xc3\xa9llo\n", ok);
    assert(sender.set_calls == 1 && sender.set_length == 7);
    assert(strcmp(sender.set_text, "h\xc3\xa9llo\n") == 0);
    expect_set(context, "", ok);
    assert(sender.set_calls == 2 && sender.set_length == 0);
    /* JSON escapes reach the sender decoded. */
    expect(context,
        "{\"jsonrpc\":\"2.0\",\"method\":\"guestClipboardSet\",\"params\":{\"text\":\"\\u00e9\\ud83d\\ude00\"},\"id\":7}", ok);
    assert(strcmp(sender.set_text, "\xc3\xa9\xf0\x9f\x98\x80") == 0);
    sender.status = "denied";
    expect_set(context, "x", "{\"jsonrpc\":\"2.0\",\"id\":7,\"result\":{\"status\":\"denied\"}}");
    sender.status = "ok";
    sender.reason = "send clipboard request: USB write failed";
    expect_set(context, "x",
        "{\"jsonrpc\":\"2.0\",\"id\":7,\"error\":{\"code\":-32603,\"message\":\"Internal error\",\"data\":\"send clipboard request: USB write failed\"}}");
    sender.reason = NULL;
    assert(sender.set_calls == 5);

    /* The longest text the protocol carries, then one byte more. */
    char *text = malloc(MAX_TEXT + 2U);
    assert(text);
    memset(text, 'a', MAX_TEXT);
    text[MAX_TEXT] = '\0';
    expect_set(context, text, ok);
    assert(sender.set_calls == 6 && sender.set_length == MAX_TEXT);
    text[MAX_TEXT] = 'a';
    text[MAX_TEXT + 1U] = '\0';
    expect_set(context, text,
        "{\"jsonrpc\":\"2.0\",\"id\":7,\"error\":{\"code\":-32603,\"message\":\"Internal error\",\"data\":\"clipboard text exceeds 65528 bytes\"}}");
    free(text);
    assert(sender.set_calls == 6);

    /* Strict UTF-8. */
    const char *valid[] = {
        "\xc3\xa9", "\xe2\x82\xac", "\xef\xbf\xbf", "\xee\x80\x80",
        "\xf0\x9f\x98\x80", "\xf4\x8f\xbf\xbf", "\x7f",
    };
    for (size_t i = 0; i < sizeof(valid) / sizeof(valid[0]); ++i) expect_set(context, valid[i], ok);
    assert(sender.set_calls == 6 + sizeof(valid) / sizeof(valid[0]));
    const char *invalid[] = {
        "\x80",                 /* continuation without a lead byte */
        "\xc0\xaf",             /* overlong '/' */
        "\xe0\x80\xaf",         /* overlong '/' */
        "\xf0\x80\x80\xaf",     /* overlong '/' */
        "\xed\xa0\x80",         /* U+D800, a surrogate */
        "\xed\xbf\xbf",         /* U+DFFF */
        "\xf4\x90\x80\x80",     /* U+110000 */
        "\xf5\x80\x80\x80",
        "\xf8\x88\x80\x80\x80", /* five-byte form */
        "\xff",
        "a\xe2\x82",            /* cut short */
        "\xe2\x28\xa1",         /* bad continuation */
        "\xc3\xa9\xc3",
    };
    for (size_t i = 0; i < sizeof(invalid) / sizeof(invalid[0]); ++i) expect_set(context, invalid[i], not_utf8);
    assert(sender.set_calls == 6 + sizeof(valid) / sizeof(valid[0]));

    const char *params[] = { "", ",\"params\":{}", ",\"params\":{\"text\":5}",
                             ",\"params\":{\"text\":null}", ",\"params\":[\"x\"]" };
    for (size_t i = 0; i < sizeof(params) / sizeof(params[0]); ++i) {
        char request[128];
        snprintf(request, sizeof(request),
                 "{\"jsonrpc\":\"2.0\",\"method\":\"guestClipboardSet\"%s,\"id\":7}", params[i]);
        expect(context, request,
            "{\"jsonrpc\":\"2.0\",\"id\":7,\"error\":{\"code\":-32602,\"message\":\"Invalid params\",\"data\":\"text must be a string\"}}");
    }
}

static void test_clipboard_default(void)
{
    jetkvm_guest_clipboard_context_t none = jetkvm_control_default_guest_clipboard_context();
    assert(!none.get && !none.set && !none.ctx);
    jetkvm_control_set_default_guest_clipboard_context(&clipboard);
    jetkvm_guest_clipboard_context_t installed = jetkvm_control_default_guest_clipboard_context();
    assert(installed.get == clipboard_get && installed.set == clipboard_set &&
           installed.ctx == &sender);
    /* The default is for the worker to install; a request context without
     * it still has no clipboard. */
    const jetkvm_control_context_t peer = { 0 };
    test_clipboard_unconnected(&peer);
    jetkvm_control_set_default_guest_clipboard_context(NULL);
    none = jetkvm_control_default_guest_clipboard_context();
    assert(!none.get && !none.set && !none.ctx);
}

/* The OS Services file drop: it sees the checked files and answers with the
 * channel, or with a reason. */
static struct {
    unsigned calls;
    int count;
    double last_size;
    const char *reason;
} dropper;

static const char *drop_files(const cJSON *files, cJSON *result, void *ctx)
{
    assert(ctx == &sender);
    assert(cJSON_IsArray(files) && result && cJSON_IsObject(result));
    ++dropper.calls;
    dropper.count = cJSON_GetArraySize(files);
    const cJSON *last = cJSON_GetArrayItem(files, dropper.count - 1);
    dropper.last_size = last ?
        cJSON_GetObjectItemCaseSensitive(last, "size")->valuedouble : -1.0;
    if (dropper.reason) {
        /* Whatever was added is discarded with the reason. */
        cJSON_AddStringToObject(result, "channel", "unused");
        return dropper.reason;
    }
    cJSON *transfers = cJSON_CreateArray();
    for (int i = 0; i < dropper.count; ++i) {
        cJSON_AddItemToArray(transfers, cJSON_CreateNumber(8 + i));
    }
    cJSON_AddStringToObject(result, "channel", "guestfile_7");
    cJSON_AddItemToObject(result, "transfers", transfers);
    return NULL;
}

static const jetkvm_guest_file_context_t file_drop = {
    .drop = drop_files, .ctx = &sender,
};

static void drop_request(char *request, size_t capacity, const char *drop)
{
    snprintf(request, capacity,
             "{\"jsonrpc\":\"2.0\",\"method\":\"guestFileDrop\",\"params\":{\"drop\":%s},\"id\":3}",
             drop);
}

static void test_file_drop_invalid(const jetkvm_control_context_t *context)
{
    static const char *const objects[] = {
        "null", "[]", "\"files\"", "3",
    };
    static const char *const drops[] = {
        "{}",
        "{\"files\":{}}",
        "{\"files\":[3]}",
        "{\"files\":[{\"size\":1}]}",
        "{\"files\":[{\"name\":5,\"size\":1}]}",
        "{\"files\":[{\"name\":\"a\"}]}",
        "{\"files\":[{\"name\":\"a\",\"size\":\"1\"}]}",
        "{\"files\":[{\"name\":\"a\",\"size\":-1}]}",
        "{\"files\":[{\"name\":\"a\",\"size\":1.5}]}",
        "{\"files\":[{\"name\":\"a\",\"size\":9007199254740994}]}",
        "{\"files\":[{\"name\":\"a\",\"size\":1},{\"name\":\"b\",\"size\":1e300}]}",
    };
    char request[512];
    const unsigned calls = dropper.calls;
    for (size_t i = 0; i < sizeof(objects) / sizeof(objects[0]); ++i) {
        drop_request(request, sizeof(request), objects[i]);
        expect(context, request,
            "{\"jsonrpc\":\"2.0\",\"id\":3,\"error\":{\"code\":-32602,\"message\":\"Invalid params\",\"data\":\"drop must be an object\"}}");
    }
    expect(context, "{\"jsonrpc\":\"2.0\",\"method\":\"guestFileDrop\",\"id\":3}",
        "{\"jsonrpc\":\"2.0\",\"id\":3,\"error\":{\"code\":-32602,\"message\":\"Invalid params\",\"data\":\"drop must be an object\"}}");
    for (size_t i = 0; i < sizeof(drops) / sizeof(drops[0]); ++i) {
        drop_request(request, sizeof(request), drops[i]);
        expect(context, request,
            "{\"jsonrpc\":\"2.0\",\"id\":3,\"error\":{\"code\":-32602,\"message\":\"Invalid params\",\"data\":"
            "\"drop.files must be an array of {name: string, size: whole number}\"}}");
    }
    /* Checked before anyone is asked. */
    assert(dropper.calls == calls);
}

static void test_file_drop_unconnected(const jetkvm_control_context_t *context)
{
    char request[256];
    drop_request(request, sizeof(request), "{\"files\":[{\"name\":\"a.txt\",\"size\":5}]}");
    expect(context, request,
        "{\"jsonrpc\":\"2.0\",\"id\":3,\"error\":{\"code\":-32603,\"message\":\"Internal error\",\"data\":"
        "\"no OS Services file transfer is connected\"}}");
}

static void test_file_drop(const jetkvm_control_context_t *context)
{
    char request[512];
    memset(&dropper, 0, sizeof(dropper));
    test_file_drop_invalid(context);
    /* The size may be any whole number up to 2^53; extra fields are left
     * for the component. */
    drop_request(request, sizeof(request),
        "{\"files\":[{\"name\":\"a.txt\",\"size\":5,\"type\":\"text/plain\"},"
        "{\"name\":\"\",\"size\":0},{\"name\":\"big.iso\",\"size\":9007199254740992}],\"extra\":1}");
    expect(context, request,
        "{\"jsonrpc\":\"2.0\",\"id\":3,\"result\":{\"channel\":\"guestfile_7\",\"transfers\":[8,9,10]}}");
    assert(dropper.calls == 1 && dropper.count == 3 && dropper.last_size == 9007199254740992.0);
    /* An empty drop is the component's to judge. */
    drop_request(request, sizeof(request), "{\"files\":[]}");
    expect(context, request,
        "{\"jsonrpc\":\"2.0\",\"id\":3,\"result\":{\"channel\":\"guestfile_7\",\"transfers\":[]}}");
    assert(dropper.calls == 2 && dropper.count == 0);
    /* The component's reason is the error, as for the clipboard. */
    dropper.reason = "a file transfer is already in progress";
    drop_request(request, sizeof(request), "{\"files\":[{\"name\":\"a.txt\",\"size\":5}]}");
    expect(context, request,
        "{\"jsonrpc\":\"2.0\",\"id\":3,\"error\":{\"code\":-32603,\"message\":\"Internal error\",\"data\":"
        "\"a file transfer is already in progress\"}}");
    assert(dropper.calls == 3);
    dropper.reason = NULL;
}

/* guestUpdateStart starts the relay or says why it cannot. */
static void test_update_start(const jetkvm_control_context_t *context)
{
    const unsigned calls = updater.calls;
    expect(context, "{\"jsonrpc\":\"2.0\",\"method\":\"guestUpdateStart\",\"id\":6}",
        "{\"jsonrpc\":\"2.0\",\"id\":6,\"result\":null}");
    updater.reason = "an OS Services update is already in progress";
    expect(context, "{\"jsonrpc\":\"2.0\",\"method\":\"guestUpdateStart\",\"params\":{},\"id\":6}",
        "{\"jsonrpc\":\"2.0\",\"id\":6,\"error\":{\"code\":-32603,\"message\":\"Internal error\",\"data\":"
        "\"an OS Services update is already in progress\"}}");
    updater.reason = NULL;
    assert(updater.calls == calls + 2U);
}

static void test_file_drop_default(void)
{
    jetkvm_guest_file_context_t none = jetkvm_control_default_guest_file_context();
    assert(!none.drop && !none.ctx);
    jetkvm_control_set_default_guest_file_context(&file_drop);
    jetkvm_guest_file_context_t installed = jetkvm_control_default_guest_file_context();
    assert(installed.drop == drop_files && installed.ctx == &sender);
    /* As for the clipboard, the default is the worker's to install. */
    const jetkvm_control_context_t peer = { 0 };
    test_file_drop_unconnected(&peer);
    jetkvm_control_set_default_guest_file_context(NULL);
    none = jetkvm_control_default_guest_file_context();
    assert(!none.drop && !none.ctx);
}

void test_os_services(void)
{
    const jetkvm_control_context_t context = {
        .video = {
            .ready = true,
            .streaming = JETKVM_VIDEO_STREAM_ACTIVE,
            .width = 1920,
            .height = 1080,
            .fps = 60,
        },
    };
    test_without_os_services(&context);

    jetkvm_control_set_default_os_services_context(&os_services);
    test_settings(&context);
    test_capabilities(&context);
    test_video_state(&context);
    test_video_fields();
    test_update_start(&context);

    jetkvm_control_set_default_os_services_context(NULL);
    test_without_os_services(&context);
    puts("OS Services settings, video state and capability vectors: OK");

    test_clipboard_unconnected(&context);
    jetkvm_control_context_t worker = context;
    worker.guest_clipboard = clipboard;
    test_clipboard_get(&worker);
    test_clipboard_get_budget(&worker);
    test_clipboard_set(&worker);
    test_clipboard_default();
    puts("OS Services clipboard vectors, reply budget and UTF-8 checks: OK");

    test_file_drop_invalid(&context);
    test_file_drop_unconnected(&context);
    jetkvm_control_context_t dropping = context;
    dropping.guest_file = file_drop;
    test_file_drop(&dropping);
    test_file_drop_default();
    puts("OS Services file drop vectors: OK");
}
