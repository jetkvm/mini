/* Guest-queue tests with the real JSON-RPC dispatcher, linked into the media
 * test. OS Services clipboard requests and file drops run on their own
 * worker; a reply of up to 128 KiB reaches the peer byte for byte; every job
 * and reply buffer is freed exactly once, whichever way the request ends. */
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "cJSON.h"
#include "freertos/semphr.h"
#include "jetkvm_control.h"
#include "jetkvm_memory_policy.h"
#include "rpc_worker.h"

void run_worker(const char *name); /* test_media.c */
bool worker_pending(const char *name); /* test_media.c */

/* The control and media methods' cap, NUL included, as before. */
#define CONTROL_INPUT_CAPACITY (JETKVM_VIRTUAL_MEDIA_URL_CAPACITY + 512U)
#define GUEST_INPUT_CAPACITY (160U * 1024U)
#define REPLY_CAPACITY JETKVM_MEMORY_RPC_SCRATCH_BYTES

static const rpc_worker_origin_t origin = {
    .session_id = 7, .generation = 9, .stream_id = 11, .access_epoch = 5,
};

/* The OS Services sender. get answers with length bytes of fill. */
typedef struct {
    unsigned gets, sets;
    size_t length;
    char fill;
    size_t set_length;
    unsigned char set_first;
    bool cancel_while_waiting;
} fake_sender_t;
static fake_sender_t sender;

static const char *sender_get(char *text, size_t capacity, size_t *length,
                              bool *truncated, const char **status, void *ctx)
{
    assert(ctx == &sender);
    assert(capacity == JETKVM_GUEST_CLIPBOARD_MAX_TEXT + 1U);
    assert(sender.length <= JETKVM_GUEST_CLIPBOARD_MAX_TEXT);
    ++sender.gets;
    /* The transport stopping while the worker waits for the sender. */
    if (sender.cancel_while_waiting) rpc_worker_cancel_replies();
    memset(text, sender.fill, sender.length);
    *length = sender.length;
    *truncated = false;
    *status = "ok";
    return NULL;
}

static const char *sender_set(const char *text, size_t length,
                              const char **status, void *ctx)
{
    assert(ctx == &sender);
    ++sender.sets;
    sender.set_length = length;
    sender.set_first = length ? (unsigned char)text[0] : 0;
    *status = "ok";
    return NULL;
}

static const jetkvm_guest_clipboard_context_t clipboard = {
    .get = sender_get, .set = sender_set, .ctx = &sender,
};

static bool defer(const char *request)
{
    return rpc_worker_defer((const uint8_t *)request, strlen(request), &origin);
}

static char *reply_buffer(void)
{
    char *buffer = malloc(REPLY_CAPACITY + 1U);
    assert(buffer);
    return buffer;
}

/* The reply as the peer task would render it with the sender installed. */
static size_t direct_reply(const char *request, char *json)
{
    jetkvm_control_context_t context = { .guest_clipboard = clipboard };
    size_t length = 0;
    assert(jetkvm_control_handle_rpc_with_context(&context, (const uint8_t *)request,
        strlen(request), json, REPLY_CAPACITY, &length) == JETKVM_CONTROL_RESPONSE_READY);
    json[length] = '\0';
    return length;
}

/* What the peer task sends when rpc_worker_defer() refuses a request. */
static void peer_reply(const char *request, char *json, size_t capacity)
{
    jetkvm_control_context_t context = { 0 };
    rpc_worker_peer_fallbacks(&context);
    size_t length = 0;
    assert(jetkvm_control_handle_rpc_with_context(&context, (const uint8_t *)request,
        strlen(request), json, capacity, &length) == JETKVM_CONTROL_RESPONSE_READY);
    json[length] = '\0';
}

/* Takes the next reply, checks it is expected, and releases it. */
static void expect_reply(const char *expected, bool large)
{
    rpc_worker_reply_t reply;
    assert(rpc_worker_take_reply(&reply));
    assert(reply.session_id == 7 && reply.generation == 9 && reply.stream_id == 11);
    assert((reply.large != NULL) == large);
    if (reply.length != strlen(expected) ||
        memcmp(rpc_worker_reply_text(&reply), expected, reply.length)) {
        fprintf(stderr, "\nMISMATCH\n  actual:   %.*s\n  expected: %s\n",
                (int)reply.length, rpc_worker_reply_text(&reply), expected);
    }
    assert(reply.length == strlen(expected));
    assert(!memcmp(rpc_worker_reply_text(&reply), expected, reply.length));
    rpc_worker_release_reply(&reply);
    assert(!reply.large);
}

static void get_request(char *out, size_t capacity, int id)
{
    snprintf(out, capacity, "{\"jsonrpc\":\"2.0\",\"id\":%d,\"method\":\"guestClipboardGet\"}", id);
}

/* A guestClipboardSet of exactly total bytes whose text is escapes copies of
 * U+0001, each written as the six bytes \u0001; spaces pad the rest. */
static char *set_request(size_t total, int id, size_t escapes)
{
    char *request = malloc(total + 1U);
    assert(request);
    int prefix = snprintf(request, total + 1U,
        "{\"jsonrpc\":\"2.0\",\"id\":%d,\"method\":\"guestClipboardSet\",\"params\":{\"text\":\"", id);
    assert(prefix > 0);
    size_t at = (size_t)prefix;
    assert(at + escapes * 6U + 3U <= total);
    for (size_t i = 0; i < escapes; ++i, at += 6U) memcpy(request + at, "\\u0001", 6U);
    request[at++] = '"';
    request[at++] = '}';
    memset(request + at, ' ', total - 1U - at);
    request[total - 1U] = '}';
    request[total] = '\0';
    return request;
}

static void test_guest_queue_is_separate(void)
{
    static const char mount[] =
        "{\"jsonrpc\":\"2.0\",\"id\":40,\"method\":\"mountWithHTTP\","
        "\"params\":{\"url\":\"http://192.0.2.1/a.iso\",\"mode\":\"CDROM\"}}";
    char get[128];
    get_request(get, sizeof(get), 41);
    sender = (fake_sender_t){ .length = 5, .fill = 'x' };
    assert(defer(mount));
    assert(defer(get));
    rpc_worker_reply_t reply;
    assert(sender.gets == 0 && !rpc_worker_take_reply(&reply));
    /* The clipboard request does not wait behind the mount. */
    run_worker("jk_guest_rpc");
    assert(sender.gets == 1);
    expect_reply("{\"jsonrpc\":\"2.0\",\"id\":41,\"result\":"
                 "{\"status\":\"ok\",\"text\":\"xxxxx\",\"truncated\":false}}", false);
    assert(!rpc_worker_take_reply(&reply));
    run_worker("jk_media_rpc");
    assert(rpc_worker_take_reply(&reply));
    assert(strstr(reply.json, "\"id\":40") && !reply.large);
    assert(control_arena_blocks == 0);

    static const char set[] =
        "{\"jsonrpc\":\"2.0\",\"id\":42,\"method\":\"guestClipboardSet\",\"params\":{\"text\":\"h\\u00e9\"}}";
    assert(defer(set));
    run_worker("jk_guest_rpc");
    assert(sender.sets == 1 && sender.set_length == 3 && sender.set_first == 'h');
    /* A short reply moved inline: only the job is still held. */
    assert(control_arena_blocks == 1);
    expect_reply("{\"jsonrpc\":\"2.0\",\"id\":42,\"result\":{\"status\":\"ok\"}}", false);
    assert(control_arena_blocks == 0);
    puts("clipboard requests run on their own queue, short replies inline: OK");
}

static void test_large_reply(void)
{
    char *expected = reply_buffer();
    char get[128];
    get_request(get, sizeof(get), 43);
    sender = (fake_sender_t){ .length = JETKVM_GUEST_CLIPBOARD_MAX_TEXT, .fill = 'a' };
    size_t length = direct_reply(get, expected);
    static const char head[] =
        "{\"jsonrpc\":\"2.0\",\"id\":43,\"result\":{\"status\":\"ok\",\"text\":\"";
    static const char tail[] = "\",\"truncated\":false}}";
    assert(length == sizeof(head) - 1U + JETKVM_GUEST_CLIPBOARD_MAX_TEXT + sizeof(tail) - 1U);
    assert(!strncmp(expected, head, sizeof(head) - 1U));
    assert(!strcmp(expected + length - (sizeof(tail) - 1U), tail));

    assert(defer(get));
    run_worker("jk_guest_rpc");
    assert(sender.gets == 2);
    rpc_worker_reply_t reply;
    assert(rpc_worker_take_reply(&reply));
    /* The peer holds the reply buffer; the job is gone. */
    assert(reply.large && control_arena_blocks == 1);
    assert(reply.length == length && !memcmp(reply.large, expected, length));
    rpc_worker_release_reply(&reply);
    assert(!reply.large && control_arena_blocks == 0);
    rpc_worker_release_reply(&reply); /* already released: nothing to free */
    assert(control_arena_blocks == 0);

    /* The inline buffer holds a reply up to 2047 bytes. */
    sender = (fake_sender_t){ .length = 0, .fill = 'b' };
    size_t envelope = direct_reply(get, expected);
    for (size_t total = 2046; total <= 2049; ++total) {
        sender.length = total - envelope;
        length = direct_reply(get, expected);
        assert(length == total);
        assert(defer(get));
        run_worker("jk_guest_rpc");
        expect_reply(expected, total >= 2048);
    }
    free(expected);
    puts("a 64 KiB clipboard reply round-trips through take and release: OK");
}

static void test_cancelled_replies_free_their_buffer(void)
{
    char get[128];
    get_request(get, sizeof(get), 44);
    sender = (fake_sender_t){ .length = 60000, .fill = 'c' };
    rpc_worker_reply_t reply;

    /* Finished, then the transport stops before the peer takes it. */
    assert(defer(get));
    run_worker("jk_guest_rpc");
    assert(control_arena_blocks == 2);
    rpc_worker_cancel_replies();
    assert(control_arena_blocks == 0 && !rpc_worker_take_reply(&reply));

    /* Cancelled before it ran: it still runs, and the worker frees it. */
    assert(defer(get));
    rpc_worker_cancel_replies();
    run_worker("jk_guest_rpc");
    assert(sender.gets == 2);
    assert(control_arena_blocks == 0 && !rpc_worker_take_reply(&reply));

    /* Cancelled while the worker waits for the sender. */
    sender.cancel_while_waiting = true;
    assert(defer(get));
    run_worker("jk_guest_rpc");
    assert(sender.gets == 3);
    assert(control_arena_blocks == 0 && !rpc_worker_take_reply(&reply));
    sender.cancel_while_waiting = false;

    /* Taken, but the session went away before the send. */
    assert(defer(get));
    run_worker("jk_guest_rpc");
    assert(rpc_worker_take_reply(&reply) && reply.large);
    rpc_worker_cancel_replies();
    assert(control_arena_blocks == 1);
    rpc_worker_release_reply(&reply);
    assert(control_arena_blocks == 0);
    puts("cancelled clipboard replies free their buffer once: OK");
}

/* cJSON's allocations, to tell whether rpc_worker_defer() parsed a request. */
static unsigned json_allocations;
static void *counting_malloc(size_t size)
{
    ++json_allocations;
    return malloc(size);
}

static void test_input_caps(void)
{
    char *json = reply_buffer();
    unsigned sets = sender.sets;
    /* Just under the cap: 27,000 characters, escaped to 162,000 bytes. */
    char *request = set_request(GUEST_INPUT_CAPACITY - 1U, 45, 27000);
    assert(defer(request));
    run_worker("jk_guest_rpc");
    assert(sender.sets == sets + 1 && sender.set_length == 27000 && sender.set_first == 1);
    expect_reply("{\"jsonrpc\":\"2.0\",\"id\":45,\"result\":{\"status\":\"ok\"}}", false);
    free(request);

    /* At the cap the peer answers inline that the request is too large, and
     * the sender is not asked. The worker refuses before it parses. */
    request = set_request(GUEST_INPUT_CAPACITY, 46, 27000);
    cJSON_Hooks hooks = { .malloc_fn = counting_malloc, .free_fn = free };
    cJSON_InitHooks(&hooks);
    json_allocations = 0;
    assert(!defer(request));
    assert(json_allocations == 0);
    cJSON_InitHooks(NULL);
    peer_reply(request, json, REPLY_CAPACITY);
    assert(!strcmp(json, "{\"jsonrpc\":\"2.0\",\"id\":46,\"error\":{\"code\":-32603,"
        "\"message\":\"Internal error\",\"data\":\"OS Services clipboard request is too large\"}}"));
    free(request);
    /* Text over the limit gets the handler's own error. */
    request = set_request(GUEST_INPUT_CAPACITY + 400000U, 47, 65529);
    assert(!defer(request));
    peer_reply(request, json, REPLY_CAPACITY);
    assert(!strcmp(json, "{\"jsonrpc\":\"2.0\",\"id\":47,\"error\":{\"code\":-32603,"
        "\"message\":\"Internal error\",\"data\":\"clipboard text exceeds 65528 bytes\"}}"));
    free(request);
    assert(sender.sets == sets + 1);

    /* The control and media methods keep their 2560-byte cap, even when a
     * guest method's name in the request gets it parsed. */
    char mount[CONTROL_INPUT_CAPACITY + 1U];
    for (int named = 0; named < 2; ++named) {
        for (size_t total = CONTROL_INPUT_CAPACITY - 1U; total <= CONTROL_INPUT_CAPACITY; ++total) {
            int head = snprintf(mount, sizeof(mount), "{\"jsonrpc\":\"2.0\",\"id\":48,"
                "\"method\":\"mountWithHTTP\",\"params\":{\"url\":\"http://192.0.2.1/a.iso\","
                "\"mode\":\"CDROM\"%s}", named ? ",\"guestClipboardSet\":0" : "");
            memset(mount + head, ' ', total - 1U - (size_t)head);
            mount[total - 1U] = '}';
            mount[total] = '\0';
            bool deferred = defer(mount);
            assert(deferred == (total < CONTROL_INPUT_CAPACITY));
            if (deferred) {
                run_worker("jk_media_rpc");
                rpc_worker_reply_t reply;
                assert(rpc_worker_take_reply(&reply) && strstr(reply.json, "\"id\":48"));
            }
        }
    }
    free(json);
    assert(control_arena_blocks == 0);
    puts("clipboard input cap 160 KiB, control and media cap unchanged: OK");
}

static void test_long_requests_skip_the_parse(void)
{
    /* A long request for an inline method is refused before it is parsed;
     * the peer parses it once, as before. */
    enum { LONG = 100 * 1024 };
    char *request = malloc(LONG + 1);
    assert(request);
    int head = snprintf(request, LONG + 1, "{\"jsonrpc\":\"2.0\",\"id\":49,"
        "\"method\":\"setKeyboardMacros\",\"params\":{\"macros\":[]}");
    memset(request + head, ' ', LONG - 1 - (size_t)head);
    request[LONG - 1] = '}';
    request[LONG] = '\0';
    cJSON_Hooks hooks = { .malloc_fn = counting_malloc, .free_fn = free };
    cJSON_InitHooks(&hooks);
    json_allocations = 0;
    assert(!defer(request));
    assert(json_allocations == 0);
    /* The name is how the worker tells: a long guest request is parsed. */
    memcpy(strstr(request, "setKeyboardMacros"), "guestClipboardSet", 17);
    assert(defer(request));
    assert(json_allocations > 0);
    cJSON_InitHooks(NULL);
    run_worker("jk_guest_rpc");
    expect_reply("{\"jsonrpc\":\"2.0\",\"id\":49,\"error\":{\"code\":-32602,"
                 "\"message\":\"Invalid params\",\"data\":\"text must be a string\"}}", false);
    free(request);
    puts("long requests for inline methods are not parsed twice: OK");
}

static void test_busy_and_unavailable(void)
{
    char *json = reply_buffer();
    char get[128];
    sender = (fake_sender_t){ .length = 1, .fill = 'd' };
    get_request(get, sizeof(get), 50);
    assert(defer(get));
    assert(defer(get));
    get_request(get, sizeof(get), 51);
    assert(!defer(get));
    peer_reply(get, json, REPLY_CAPACITY);
    assert(!strcmp(json, "{\"jsonrpc\":\"2.0\",\"id\":51,\"error\":{\"code\":-32603,"
        "\"message\":\"Internal error\",\"data\":\"another OS Services clipboard request is active\"}}"));
    assert(sender.gets == 0);
    run_worker("jk_guest_rpc");
    assert(sender.gets == 2);
    static const char done[] = "{\"jsonrpc\":\"2.0\",\"id\":50,\"result\":"
        "{\"status\":\"ok\",\"text\":\"d\",\"truncated\":false}}";
    expect_reply(done, false);
    expect_reply(done, false);
    peer_reply(get, json, REPLY_CAPACITY);
    assert(strstr(json, "\"data\":\"OS Services clipboard request could not start\""));
    free(json);
    puts("a full clipboard queue answers busy without asking the sender: OK");
}

static void test_out_of_memory(void)
{
    char get[128];
    get_request(get, sizeof(get), 52);
    static const char set[] =
        "{\"jsonrpc\":\"2.0\",\"id\":53,\"method\":\"guestClipboardSet\",\"params\":{\"text\":\"x\"}}";
    sender = (fake_sender_t){ .length = 1, .fill = 'e' };
    control_arena_fail_size = REPLY_CAPACITY;
    assert(defer(get));
    assert(defer(set));
    run_worker("jk_guest_rpc");
    /* Without a reply buffer the clipboard is left alone, but answered. */
    assert(sender.gets == 0 && sender.sets == 0);
    expect_reply("{\"jsonrpc\":\"2.0\",\"id\":52,\"error\":{\"code\":-32603,"
                 "\"message\":\"Internal error\",\"data\":\"out of memory\"}}", false);
    expect_reply("{\"jsonrpc\":\"2.0\",\"id\":53,\"error\":{\"code\":-32603,"
                 "\"message\":\"Internal error\",\"data\":\"out of memory\"}}", false);
    control_arena_fail_size = 0;
    assert(control_arena_blocks == 0);
    puts("no memory for the reply buffer still answers the request: OK");
}

static void test_without_clipboard(void)
{
    char get[128];
    get_request(get, sizeof(get), 54);
    jetkvm_control_set_default_guest_clipboard_context(NULL);
    assert(defer(get));
    run_worker("jk_guest_rpc");
    expect_reply("{\"jsonrpc\":\"2.0\",\"id\":54,\"error\":{\"code\":-32603,"
                 "\"message\":\"Internal error\",\"data\":\"no OS Services clipboard is connected\"}}",
                 false);
    assert(control_arena_blocks == 0);
    puts("without OS Services the worker answers that no clipboard is connected: OK");
}

/* The OS Services file drop: answers with one transfer per file. */
static unsigned drops;
static int drop_count;

static const char *drop_files(const cJSON *files, cJSON *result, void *ctx)
{
    assert(ctx == &drops);
    ++drops;
    drop_count = cJSON_GetArraySize(files);
    cJSON *transfers = cJSON_AddArrayToObject(result, "transfers");
    for (int i = 0; i < drop_count; ++i) {
        cJSON_AddItemToArray(transfers, cJSON_CreateNumber(4 + i));
    }
    cJSON_AddStringToObject(result, "channel", "guestfile_3");
    return NULL;
}

static const jetkvm_guest_file_context_t file_drop = { .drop = drop_files, .ctx = &drops };

static void test_file_drop(void)
{
    char *json = reply_buffer();
    static const char drop[] = "{\"jsonrpc\":\"2.0\",\"id\":60,\"method\":\"guestFileDrop\","
        "\"params\":{\"drop\":{\"files\":[{\"name\":\"a.txt\",\"size\":5}]}}}";
    static const char unconnected[] = "{\"jsonrpc\":\"2.0\",\"id\":60,\"error\":{\"code\":-32603,"
        "\"message\":\"Internal error\",\"data\":\"no OS Services file transfer is connected\"}}";
    /* Without OS Services the worker says so. */
    assert(defer(drop));
    run_worker("jk_guest_rpc");
    expect_reply(unconnected, false);

    jetkvm_control_set_default_guest_file_context(&file_drop);
    assert(defer(drop));
    run_worker("jk_guest_rpc");
    assert(drops == 1 && drop_count == 1);
    expect_reply("{\"jsonrpc\":\"2.0\",\"id\":60,\"result\":"
                 "{\"transfers\":[4],\"channel\":\"guestfile_3\"}}", false);

    /* A drop of 200 files is longer than the control cap, but a guest
     * request: it is parsed and runs on the worker. */
    enum { FILES = 200 };
    char *many = malloc(FILES * 64U + 256U);
    assert(many);
    size_t at = (size_t)sprintf(many, "{\"jsonrpc\":\"2.0\",\"id\":61,\"method\":\"guestFileDrop\","
                                "\"params\":{\"drop\":{\"files\":[");
    for (int i = 0; i < FILES; ++i) {
        at += (size_t)sprintf(many + at, "%s{\"name\":\"file-%03d-with-a-longer-name.bin\",\"size\":%d}",
                              i ? "," : "", i, i * 1000);
    }
    strcpy(many + at, "]}}}");
    assert(strlen(many) > CONTROL_INPUT_CAPACITY);
    assert(defer(many));
    run_worker("jk_guest_rpc");
    assert(drops == 2 && drop_count == FILES);
    rpc_worker_reply_t reply;
    assert(rpc_worker_take_reply(&reply));
    assert(strstr(rpc_worker_reply_text(&reply), "\"id\":61,\"result\":{\"transfers\":[4,5,"));
    rpc_worker_release_reply(&reply);
    free(many);

    /* With the guest queue full, the peer answers busy without a drop. */
    jetkvm_control_set_default_guest_clipboard_context(&clipboard);
    char get[128];
    sender = (fake_sender_t){ .length = 1, .fill = 'f' };
    get_request(get, sizeof(get), 62);
    assert(defer(get));
    assert(defer(get));
    assert(!defer(drop));
    peer_reply(drop, json, REPLY_CAPACITY);
    assert(!strcmp(json, "{\"jsonrpc\":\"2.0\",\"id\":60,\"error\":{\"code\":-32603,"
        "\"message\":\"Internal error\",\"data\":\"another OS Services request is active\"}}"));
    assert(drops == 2);
    run_worker("jk_guest_rpc");
    expect_reply("{\"jsonrpc\":\"2.0\",\"id\":62,\"result\":"
                 "{\"status\":\"ok\",\"text\":\"f\",\"truncated\":false}}", false);
    expect_reply("{\"jsonrpc\":\"2.0\",\"id\":62,\"result\":"
                 "{\"status\":\"ok\",\"text\":\"f\",\"truncated\":false}}", false);
    /* An empty queue that could not take the job says it could not start. */
    peer_reply(drop, json, REPLY_CAPACITY);
    assert(strstr(json, "\"data\":\"OS Services file drop could not start\""));

    /* Without a reply buffer the drop is not made, but answered. */
    control_arena_fail_size = REPLY_CAPACITY;
    assert(defer(drop));
    run_worker("jk_guest_rpc");
    assert(drops == 2);
    expect_reply("{\"jsonrpc\":\"2.0\",\"id\":60,\"error\":{\"code\":-32603,"
                 "\"message\":\"Internal error\",\"data\":\"out of memory\"}}", false);
    control_arena_fail_size = 0;
    jetkvm_control_set_default_guest_file_context(NULL);
    jetkvm_control_set_default_guest_clipboard_context(NULL);
    assert(control_arena_blocks == 0);
    free(json);
    puts("file drops run on the guest queue, busy and out of memory answered: OK");
}

/* The second drop of a stream, sent while the guest worker waits after the
 * first: it joins the queue of the running worker, which starts no other. */
static const char second_drop[] = "{\"jsonrpc\":\"2.0\",\"id\":71,\"method\":\"guestFileDrop\","
    "\"params\":{\"drop\":{\"files\":[{\"name\":\"b.txt\",\"size\":6}]}}}";
static void send_second_drop(void)
{
    assert(defer(second_drop));
    assert(!worker_pending("jk_guest_rpc"));
}

static void test_drops_share_one_worker(void)
{
    static const char first_drop[] = "{\"jsonrpc\":\"2.0\",\"id\":70,\"method\":\"guestFileDrop\","
        "\"params\":{\"drop\":{\"files\":[{\"name\":\"a.txt\",\"size\":5}]}}}";
    jetkvm_control_set_default_guest_file_context(&file_drop);
    const unsigned before = drops;
    assert(defer(first_drop));
    semaphore_wait_hook = send_second_drop;
    run_worker("jk_guest_rpc");
    /* One worker ran both drops and then ended. */
    assert(drops == before + 2U);
    assert(!semaphore_wait_hook);
    assert(!worker_pending("jk_guest_rpc"));
    rpc_worker_reply_t reply;
    assert(rpc_worker_take_reply(&reply));
    assert(strstr(rpc_worker_reply_text(&reply), "\"id\":70,\"result\""));
    rpc_worker_release_reply(&reply);
    assert(rpc_worker_take_reply(&reply));
    assert(strstr(rpc_worker_reply_text(&reply), "\"id\":71,\"result\""));
    rpc_worker_release_reply(&reply);
    /* A drop after the wait starts a new worker. */
    assert(defer(first_drop));
    assert(worker_pending("jk_guest_rpc"));
    run_worker("jk_guest_rpc");
    assert(drops == before + 3U);
    assert(rpc_worker_take_reply(&reply));
    rpc_worker_release_reply(&reply);
    jetkvm_control_set_default_guest_file_context(NULL);
    assert(control_arena_blocks == 0);
    puts("a drop sent while the guest worker waits runs on that worker: OK");
}

void test_guest_queue(void)
{
    jetkvm_control_set_default_guest_clipboard_context(&clipboard);
    test_guest_queue_is_separate();
    test_large_reply();
    test_cancelled_replies_free_their_buffer();
    test_input_caps();
    test_long_requests_skip_the_parse();
    test_busy_and_unavailable();
    test_out_of_memory();
    test_without_clipboard();
    test_file_drop();
    test_drops_share_one_worker();
}
