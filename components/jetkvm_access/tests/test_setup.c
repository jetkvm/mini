#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <strings.h>
#include <stdatomic.h>
#include <setjmp.h>
#include <sys/socket.h>
#include <arpa/inet.h>
#include "cJSON.h"
#include "password_core.h"
#include "access_contract.h"
#include "device_contract.h"

typedef int esp_err_t, nvs_handle_t, portMUX_TYPE, BaseType_t, StaticSemaphore_t;
typedef void *SemaphoreHandle_t, *TaskHandle_t, *httpd_handle_t;
typedef struct { size_t size; } esp_partition_t;
typedef struct { char namespace_name[16]; } nvs_entry_info_t;
typedef unsigned *nvs_iterator_t;
typedef struct { httpd_handle_t handle; const char *uri; int method; size_t content_len;
    void *user_ctx; const char *body, *origin, *cookie; } httpd_req_t;
typedef struct { const char *uri; int method; esp_err_t (*handler)(httpd_req_t *); void *user_ctx; } httpd_uri_t;
#define ESP_OK 0
#define ESP_FAIL 1
#define ESP_ERR_NVS_NOT_FOUND 2
#define ESP_ERR_NOT_FOUND 3
#define ESP_ERR_NVS_INVALID_LENGTH 4
#define ESP_ERR_NVS_TYPE_MISMATCH 5
#define ESP_ERR_INVALID_SIZE 6
#define ESP_ERR_NO_MEM 7
#define ESP_ERR_INVALID_STATE 8
#define ESP_LOGW(...) ((void)0)
#define JETKVM_ACCESS_HOST_NAME_CAPACITY 128U
#define JETKVM_ACCESS_EXTRA_HOST_NAMES 8U
#define EXT_RAM_BSS_ATTR
#define NVS_READONLY 0
#define NVS_READWRITE 1
#define NVS_TYPE_ANY 0
#define ESP_PARTITION_TYPE_DATA 0
#define ESP_PARTITION_SUBTYPE_ANY 0
#define portMUX_INITIALIZER_UNLOCKED 0
#define portENTER_CRITICAL(p) ((void)(p))
#define portEXIT_CRITICAL(p) ((void)(p))
#define pdTRUE 1
#define pdPASS 1
#define portMAX_DELAY 99999
#define pdMS_TO_TICKS(n) (n)
#define MALLOC_CAP_INTERNAL 1
#define MALLOC_CAP_8BIT 2
#define eSetValueWithOverwrite 1
#define configTICK_RATE_HZ 100
#define HTTP_POST 1
#define HTTP_GET 2
#define HTTP_PUT 3
#define HTTP_DELETE 4
#define PSA_SUCCESS 0
static int psa_crypto_init(void) { return 0; }
static unsigned tick = 1;
static unsigned xTaskGetTickCount(void) { return tick; }
static int64_t esp_timer_get_time(void) { return (int64_t)tick * 1000000; }
static void vTaskDelay(unsigned n) { (void)n; }
static void vTaskDeleteWithCaps(void *p) { (void)p; }
static unsigned ulTaskNotifyTake(int clear, unsigned timeout) { (void)clear; (void)timeout; return 1; }
static void xTaskNotify(void *task, unsigned value, int mode) { (void)task; (void)value; (void)mode; }
static SemaphoreHandle_t xSemaphoreCreateBinaryStatic(StaticSemaphore_t *p) { return p; }
static SemaphoreHandle_t xSemaphoreCreateRecursiveMutex(void) { return (void *)1; }
static int xSemaphoreTakeRecursive(void *p, unsigned timeout) { (void)p; (void)timeout; return 1; }
static void xSemaphoreGiveRecursive(void *p) { (void)p; }
static void xSemaphoreGive(void *p) { (void)p; }
static void xSemaphoreTake(void *p, unsigned timeout) { (void)p; (void)timeout; }
static void vSemaphoreDelete(void *p) { (void)p; }
static void esp_fill_random(void *p, size_t n) { memset(p, 0x39, n); }
static void (*during_derive)(void);
static bool jetkvm_password_pbkdf2(const char *password, const uint8_t *salt,
    uint32_t cost, uint8_t *hash, void (*yield)(void)) {
    (void)salt; (void)cost; yield();
    if (during_derive) { void (*hook)(void) = during_derive; during_derive = NULL; hook(); }
    memset(hash, (unsigned char)password[0], 32); return true;
}
static int xTaskCreateWithCaps(void (*fn)(void *), const char *name, unsigned stack,
    void *arg, int priority, void **task, unsigned caps) {
    (void)stack; (void)priority; (void)caps;
    if (task) *task = (void *)1;
    if (!strcmp(name, "password")) fn(arg);
    return pdPASS;
}
static jmp_buf restart;
static void esp_restart(void) { longjmp(restart, 1); }
static const char *names[] = {"jk_reset", "jk_access", "jk_settings"};
static struct { uint8_t data[256]; size_t size; } records[3], staged_record;
static bool fail_save, fail_storage;
static unsigned cursor;
static int nvs_open(const char *name, int mode, int *handle) {
    (void)mode;
    for (unsigned i = 0; i < 3; ++i) if (!strcmp(name, names[i])) { *handle = i; return 0; }
    return ESP_ERR_NOT_FOUND;
}
static void nvs_close(int h) { (void)h; }
static int nvs_set_blob(int h, const char *key, const void *p, size_t n) {
    (void)h; (void)key; assert(n <= sizeof(staged_record.data));
    memcpy(staged_record.data, p, n); staged_record.size = n; return 0;
}
static int nvs_commit(int h) {
    if (fail_save) return ESP_FAIL;
    records[h] = staged_record; return 0;
}
static int nvs_get_blob(int h, const char *key, void *p, size_t *n) {
    (void)key;
    if (!records[h].size) return ESP_ERR_NVS_NOT_FOUND;
    if (*n < records[h].size) return ESP_ERR_NVS_INVALID_LENGTH;
    *n = records[h].size; memcpy(p, records[h].data, *n); return 0;
}
/* The extra host names are the one string the access namespace holds. */
static char stored_names[1100];
static bool names_stored;
static int nvs_set_str(int h, const char *key, const char *value) {
    assert(h == 1 && !strcmp(key, "host_names") && strlen(value) < sizeof(stored_names));
    if (fail_save) return ESP_FAIL;
    strcpy(stored_names, value); names_stored = true; return 0;
}
static int nvs_get_str(int h, const char *key, char *value, size_t *n) {
    assert(h == 1 && !strcmp(key, "host_names"));
    if (!names_stored) return ESP_ERR_NVS_NOT_FOUND;
    if (*n <= strlen(stored_names)) return ESP_ERR_NVS_INVALID_LENGTH;
    strcpy(value, stored_names); *n = strlen(stored_names) + 1; return 0;
}
static int nvs_erase_key(int h, const char *key) { (void)h; (void)key; staged_record.size = 0; return 0; }
static int nvs_erase_all(int h) { (void)h; staged_record.size = 0; return 0; }
static int seek(nvs_iterator_t *p) {
    while (cursor < 3 && !records[cursor].size) ++cursor;
    *p = cursor < 3 ? &cursor : NULL; return *p ? 0 : ESP_ERR_NVS_NOT_FOUND;
}
static int nvs_entry_find(const char *partition, const char *ns, int type, nvs_iterator_t *p) {
    (void)partition; (void)ns; (void)type; cursor = 0; return seek(p);
}
static int nvs_entry_next(nvs_iterator_t *p) { ++cursor; return seek(p); }
static void nvs_entry_info(nvs_iterator_t p, nvs_entry_info_t *info) { strcpy(info->namespace_name, names[*p]); }
static void nvs_release_iterator(nvs_iterator_t p) { (void)p; }
static const esp_partition_t *esp_partition_find_first(int type, int sub, const char *label) {
    (void)type; (void)sub; (void)label; static const esp_partition_t p = {4096}; return &p;
}
static int esp_partition_erase_range(const esp_partition_t *p, size_t start, size_t length) {
    (void)p; (void)start; (void)length; return fail_storage;
}
/* The reset erases only blocks that do not read blank; the fake storage holds
 * data everywhere, so every block goes through esp_partition_erase_range. */
static int esp_partition_read(const esp_partition_t *p, size_t offset, void *out, size_t size) {
    (void)p; (void)offset; memset(out, 0, size); return 0;
}
static void *heap_caps_malloc(size_t size, unsigned caps) { (void)caps; return malloc(size); }
static void heap_caps_free(void *ptr) { free(ptr); }
static httpd_uri_t routes[16];
static unsigned route_count;
static char response[1024], response_status[64], response_cookie[192];
static bool response_close; static esp_err_t last_result;
static esp_err_t httpd_register_uri_handler(void *server, const httpd_uri_t *route) {
    (void)server; assert(route_count < 16); routes[route_count++] = *route; return 0;
}
static esp_err_t httpd_resp_set_status(httpd_req_t *req, const char *value) { (void)req; strcpy(response_status, value); return 0; }
static esp_err_t httpd_resp_set_type(httpd_req_t *req, const char *value) { (void)req; (void)value; return 0; }
static esp_err_t httpd_resp_set_hdr(httpd_req_t *req, const char *name, const char *value) {
    (void)req; if (!strcmp(name, "Set-Cookie")) strcpy(response_cookie, value);
    if (!strcmp(name, "Connection") && !strcmp(value, "close")) response_close = true;
    return 0;
}
static esp_err_t httpd_resp_sendstr(httpd_req_t *req, const char *value) { (void)req; strcpy(response, value); return 0; }
static size_t httpd_req_get_hdr_value_len(httpd_req_t *req, const char *name) {
    return !strcmp(name, "Origin") && req->origin ? strlen(req->origin) : 0;
}
static const char *host_header = "mini.local";
static esp_err_t httpd_req_get_hdr_value_str(httpd_req_t *req, const char *name, char *buffer, size_t capacity) {
    const char *value = !strcmp(name, "Host") ? host_header : !strcmp(name, "Origin") ? req->origin : req->cookie;
    if (!value || strlen(value) >= capacity) return ESP_FAIL;
    strcpy(buffer, value); return 0;
}
static int httpd_req_recv(httpd_req_t *req, char *buffer, size_t n) { memcpy(buffer, req->body, n); return n; }
static const char *client_ip = "192.0.2.1";
static int httpd_req_to_sockfd(httpd_req_t *req) { (void)req; return 1; }
static int test_getpeername(int fd, struct sockaddr *peer, socklen_t *size) {
    (void)fd; (void)size;
    struct sockaddr_in6 *v6 = (struct sockaddr_in6 *)peer;
    struct sockaddr_in *v4 = (struct sockaddr_in *)peer;
    memset(peer, 0, sizeof(struct sockaddr_storage));
    peer->sa_family = strchr(client_ip, ':') ? AF_INET6 : AF_INET;
    return inet_pton(peer->sa_family, client_ip, peer->sa_family == AF_INET ?
        (void *)&v4->sin_addr : (void *)&v6->sin6_addr) == 1 ? 0 : -1;
}
#define getpeername test_getpeername
static const char *server_ip = "192.0.2.10";
static int test_getsockname(int fd, struct sockaddr *local, socklen_t *size) {
    (void)fd; (void)size;
    memset(local, 0, sizeof(struct sockaddr_storage));
    local->sa_family = strchr(server_ip, ':') ? AF_INET6 : AF_INET;
    return inet_pton(local->sa_family, server_ip, local->sa_family == AF_INET ?
        (void *)&((struct sockaddr_in *)local)->sin_addr :
        (void *)&((struct sockaddr_in6 *)local)->sin6_addr) == 1 ? 0 : -1;
}
#define getsockname test_getsockname
/* PRODUCTION_IMPLEMENTATIONS */

static void assert_status(bool setup, bool pending, bool error) {
    char json[512];
    assert(p4_device_status_json(jetkvm_access_is_setup(), jetkvm_factory_reset_pending(),
                                jetkvm_factory_reset_error(), "jetkvm-mini-ethernet",
                                json, sizeof(json)));
    cJSON *status = cJSON_Parse(json); assert(status);
    assert(cJSON_IsTrue(cJSON_GetObjectItem(status, "isSetup")) == setup);
    assert(cJSON_IsTrue(cJSON_GetObjectItem(status, "factoryResetPending")) == pending);
    assert(cJSON_IsString(cJSON_GetObjectItem(status, "factoryResetError")) == error);
    cJSON_Delete(status);
}
static void reboot_runtime(void) {
    s_mutex = NULL; memset(s_listeners, 0, sizeof(s_listeners));
    s_password_busy = false; memset(s_login_limits, 0, sizeof(s_login_limits));
    memset(s_token, 0, sizeof(s_token)); route_count = 0;
    s_busy = false; s_restart_task = NULL;
    memset(&s_intent, 0, sizeof(s_intent));
    atomic_store(&s_state, RESET_IDLE);
}
static size_t own_names(char (*names)[JETKVM_ACCESS_HOST_NAME_CAPACITY], size_t capacity) {
    static const char *const own[] = {"mini", "mini-ts.tail0.ts.net."};
    size_t count = 0;
    for (; count < capacity && count < 2; ++count) strcpy(names[count], own[count]);
    return count;
}
static void start_access(void) {
    jetkvm_access_set_host_names(own_names);
    assert(jetkvm_access_init(NULL) == ESP_OK);
    assert(jetkvm_access_attach((void *)1, false) == ESP_OK);
}
static int request(const char *uri, int method, const char *body, const char *origin, const char *cookie) {
    strcpy(response_status, "200 OK"); response[0] = response_cookie[0] = 0; response_close = false;
    for (unsigned i = 0; i < route_count; ++i) if (strlen(routes[i].uri) == strcspn(uri, "?") &&
        !strncmp(routes[i].uri, uri, strlen(routes[i].uri)) && routes[i].method == method) {
        httpd_req_t req = {.handle = (void *)1, .uri = uri, .method = method,
            .content_len = strlen(body), .body = body, .origin = origin, .cookie = cookie,
            .user_ctx = routes[i].user_ctx};
        last_result = routes[i].handler(&req); return atoi(response_status);
    }
    return 404;
}
#define SETUP(body) request("/device/setup", HTTP_POST, body, "http://mini.local", NULL)
static void competing_setup(void) {
    assert(SETUP("{\"localAuthMode\":\"noPassword\"}") == 429);
}
static void reset_during_setup(void) {
    assert(!jetkvm_factory_reset_request(NULL));
}
static esp_err_t answer_ok(httpd_req_t *req) { return httpd_resp_sendstr(req, "{}"); }
static int device_request(void) {
    const httpd_uri_t device = {.uri = "/device", .method = HTTP_GET, .handler = answer_ok};
    if (route_count == 6) assert(jetkvm_access_register((void *)1, &device) == ESP_OK);
    return request("/device", HTTP_GET, "", NULL, NULL);
}
/* A DNS-rebinding page sends its own name as Host and as Origin, or no
 * Origin from a non-browser client. Each request is answered or refused 421
 * on a guarded route, a public route and the signaling WebSocket alike. */
static void check_host(const char *host, bool answered) {
    char origin[300];
    snprintf(origin, sizeof(origin), "http://%s", host);
    host_header = host;
    httpd_req_t socket = {.handle = (void *)1, .uri = "/webrtc/signaling/client",
        .method = HTTP_GET, .origin = origin};
    assert((request("/device", HTTP_GET, "", origin, NULL) == 200) == answered);
    assert((request("/device", HTTP_GET, "", NULL, NULL) == 421) == !answered);
    assert((request("/auth/login-local", HTTP_POST, "{\"password\":\"12345678\"}", NULL, NULL) == 421) == !answered);
    assert((jetkvm_access_check_request(&socket, true) == ESP_OK) == answered);
    host_header = "mini.local";
}
static void complete_reset(void) {
    reboot_runtime();
    assert(jetkvm_factory_reset_boot_begin(NULL) == ESP_OK);
    assert(jetkvm_factory_reset_finish(ESP_OK) == ESP_OK);
    start_access();
    assert_status(false, false, false);
    assert(!records[1].size && !records[2].size);
}

int main(void) {
    reboot_runtime(); assert(!jetkvm_factory_reset_boot_begin(NULL)); start_access();
    assert_status(false, false, false);
    /* Before setup only the setup routes answer: every other guarded route,
     * the signaling WebSocket and a password change are refused. */
    assert(device_request() == 401 && strstr(response, "Setup required"));
    httpd_req_t socket = {.handle = (void *)1, .uri = "/webrtc/signaling/client",
        .method = HTTP_GET, .origin = "http://mini.local"};
    assert(jetkvm_access_check_request(&socket, true) == ESP_ERR_INVALID_STATE);
    assert(request("/auth/password-local", HTTP_POST, "{\"password\":\"12345678\"}", NULL, NULL) == 401);
    assert_status(false, false, false);
    assert(SETUP("{}") == 400);
    assert(SETUP("{\"localAuthMode\":\"invalid\"}") == 400);
    assert(SETUP("{\"localAuthMode\":\"password\",\"password\":\"short\"}") == 400);
    assert(SETUP("{\"localAuthMode\":\"password\",\"password\":\"12345678\\u0000tail\"}") == 400);
    assert(SETUP("{\"localAuthMode\":\"noPassword\",\"password\":\"12345678\"}") == 400);
    assert(request("/device/setup", HTTP_POST, "{\"localAuthMode\":\"noPassword\"}", "https://evil.test", NULL) == 403);
    assert_status(false, false, false);
    fail_save = true;
    assert(SETUP("{\"localAuthMode\":\"noPassword\"}") == 500);
    assert_status(false, false, false);
    fail_save = false;
    assert(SETUP("{\"localAuthMode\":\"noPassword\"}") == 201);
    assert(!jetkvm_access_password_enabled()); assert_status(true, false, false);
    assert(device_request() == 200);
    assert(jetkvm_access_check_request(&socket, true) == ESP_OK);
    assert(SETUP("{\"localAuthMode\":\"password\",\"password\":\"12345678\"}") == 409);
    reboot_runtime(); start_access(); assert_status(true, false, false);
    records[2].size = 8; /* A saved non-default device setting. */
    assert(!jetkvm_factory_reset_request(NULL)); assert_status(true, true, false);
    reboot_runtime(); assert(!jetkvm_factory_reset_boot_begin(NULL));
    assert(jetkvm_factory_reset_finish(ESP_FAIL) != ESP_OK); start_access();
    assert_status(false, true, true); assert(!records[2].size);
    assert(SETUP("{\"localAuthMode\":\"noPassword\"}") == 409);
    complete_reset();
    assert(SETUP("{\"localAuthMode\":\"password\",\"password\":\"12345678\"}") == 201);
    assert(jetkvm_access_password_enabled()); assert_status(true, false, false);
    assert(response_cookie[0]);
    assert(SETUP("{\"localAuthMode\":\"noPassword\"}") == 409);
    reboot_runtime(); start_access(); assert_status(true, false, false);
    assert(jetkvm_access_password_enabled());
    /* Public login must reject deep JSON before password verification, even
     * if the nested member is unused. The body fits the 1024-byte buffer. */
    char nested[1024];
    const int depths[] = {CJSON_NESTING_LIMIT, 400};
    for (size_t i = 0; i < sizeof(depths) / sizeof(depths[0]); ++i) {
        int depth = depths[i];
        size_t used = (size_t)sprintf(nested, "{\"password\":\"12345678\",\"unused\":");
        memset(nested + used, '[', depth); used += depth;
        nested[used++] = '0';
        memset(nested + used, ']', depth); used += depth;
        strcpy(nested + used, "}");
        assert(request("/auth/login-local", HTTP_POST, nested, NULL, NULL) == 400);
        assert(!response_cookie[0]);
    }
    /* Upstream contract: five wrong passwords in a row answer 401 without any
     * delay, the sixth attempt is locked out for fifteen minutes, and the
     * lockout also blocks the correct password until it expires. */
    for (int i = 0; i < 5; ++i)
        assert(request("/auth/login-local", HTTP_POST, "{\"password\":\"wrongpass\"}", NULL, NULL) == 401);
    assert(request("/auth/login-local", HTTP_POST, "{\"password\":\"wrongpass\"}", NULL, NULL) == 429);
    assert(request("/auth/login-local", HTTP_POST, "{\"password\":\"12345678\"}", NULL, NULL) == 429);
    tick += 15 * 60 - 1;
    assert(request("/auth/login-local", HTTP_POST, "{\"password\":\"12345678\"}", NULL, NULL) == 429);
    tick += 1;
    /* The close header and the handler result agree: a rejected body closes,
     * a bodiless 401 keeps its connection. */
    assert(request("/auth/login-local", HTTP_POST, "{\"password\":\"wrongpass\"}", NULL, NULL) == 401);
    assert(response_close && last_result == ESP_FAIL);
    assert(request("/auth/local-password", HTTP_DELETE, "", NULL, NULL) == 401);
    assert(!response_close && last_result == ESP_OK);
    assert(request("/auth/login-local", HTTP_POST, "{\"password\":\"wrongpass\"}", NULL, NULL) == 401);
    assert(request("/auth/login-local", HTTP_POST, "{\"password\":\"12345678\"}", NULL, NULL) == 200);
    char cookie[192]; strcpy(cookie, response_cookie);
    assert(request("/auth/local-password", HTTP_DELETE, "{\"password\":\"12345678\"}", NULL, cookie) == 200);
    assert_status(true, false, false); assert(!jetkvm_access_password_enabled());
    reboot_runtime(); start_access(); assert_status(true, false, false);
    assert(!jetkvm_factory_reset_request(NULL)); complete_reset();
    assert(!jetkvm_access_password_enabled());
    /* Incomplete final cleanup must not allow new setup after access data is erased. */
    assert(SETUP("{\"localAuthMode\":\"password\",\"password\":\"12345678\"}") == 201);
    assert(!jetkvm_factory_reset_request(NULL));
    reboot_runtime(); assert(!jetkvm_factory_reset_boot_begin(NULL));
    assert(jetkvm_factory_reset_finish(ESP_FAIL) != ESP_OK);
    start_access(); assert_status(false, true, true);
    fail_storage = true; reboot_runtime(); assert(jetkvm_factory_reset_boot_begin(NULL) != ESP_OK);
    start_access(); assert_status(false, true, true);
    assert(SETUP("{\"localAuthMode\":\"noPassword\"}") == 409);
    listener((void *)1)->request_epoch = atomic_load(&s_epoch);
    assert(request("/device/setup?retry=1", HTTP_POST, "{\"localAuthMode\":\"noPassword\"}", NULL, NULL) == 409);
    fail_storage = false; reboot_runtime(); assert(!jetkvm_factory_reset_boot_begin(NULL));
    assert(!jetkvm_factory_reset_finish(ESP_OK));
    start_access(); assert_status(false, false, false);
    during_derive = competing_setup;
    assert(SETUP("{\"localAuthMode\":\"password\",\"password\":\"12345678\"}") == 201);
    assert(jetkvm_access_password_enabled());
    assert(!jetkvm_factory_reset_request(NULL)); complete_reset();
    during_derive = reset_during_setup;
    assert(SETUP("{\"localAuthMode\":\"password\",\"password\":\"12345678\"}") == 400);
    assert_status(false, true, false);
    complete_reset();
    fail_save = true;
    assert(SETUP("{\"localAuthMode\":\"password\",\"password\":\"12345678\"}") == 500);
    assert_status(false, false, false);
    assert(!jetkvm_access_password_enabled());
    fail_save = false;
    /* Lockout is per peer for IPv4 and IPv6. A retained session can change
     * and disable its password without clearing another client's lockout. */
    for (int family = 0; family < 2; ++family) {
        memset(records, 0, sizeof(records)); reboot_runtime(); start_access();
        client_ip = family ? "2001:db8::1" : "192.0.2.1";
        assert(SETUP("{\"localAuthMode\":\"password\",\"password\":\"12345678\"}") == 201);
        strcpy(cookie, response_cookie);
        for (int i = 0; i < 5; ++i)
            assert(request("/auth/login-local?test=1", HTTP_POST, "{\"password\":\"wrongpass\"}", NULL, NULL) == 401);
        assert(request("/auth/login-local", HTTP_POST, "{\"password\":\"12345678\"}", NULL, NULL) == 429);
        assert(request("/auth/password-local", HTTP_PUT,
            "{\"oldPassword\":\"12345678\",\"newPassword\":\"23456789\"}", NULL, cookie) == 200);
        strcpy(cookie, response_cookie);
        assert(request("/auth/login-local", HTTP_POST, "{\"password\":\"23456789\"}", NULL, NULL) == 429);
        if (!family) {
            client_ip = "::ffff:192.0.2.1";
            assert(request("/auth/login-local", HTTP_POST, "{\"password\":\"23456789\"}", NULL, NULL) == 429);
        }
        client_ip = family ? "2001:db8::2" : "192.0.2.2";
        assert(request("/auth/login-local", HTTP_POST, "{\"password\":\"23456789\"}", NULL, NULL) == 200);
        strcpy(cookie, response_cookie);
        client_ip = family ? "2001:db8::1" : "192.0.2.1";
        assert(request("/auth/login-local", HTTP_POST, "{\"password\":\"23456789\"}", NULL, NULL) == 429);
        assert(request("/auth/local-password", HTTP_DELETE, "{\"password\":\"23456789\"}", NULL, cookie) == 200);
        assert(!jetkvm_access_password_enabled());
    }
    memset(s_login_limits, 0, sizeof(s_login_limits));
    char ip[32];
    for (unsigned i = 0; i < 32; ++i) {
        snprintf(ip, sizeof(ip), "192.0.2.%u", i + 1); client_ip = ip;
        login_limit_t *entry = login_limit(NULL); assert(entry);
        entry->next_attempt_us = esp_timer_get_time() + LOGIN_LOCKOUT_US;
    }
    client_ip = "192.0.2.99"; assert(!login_limit(NULL));
    client_ip = "192.0.2.1"; assert(login_limit(NULL)->next_attempt_us > esp_timer_get_time());
    tick += 60 * 60;
    client_ip = "192.0.2.99"; assert(login_limit(NULL));
    /* Legacy password records remain configured; corruption never opens setup. */
    jetkvm_password_record_t legacy; jetkvm_password_defaults(&legacy); legacy.enabled = 1;
    memcpy(records[1].data, &legacy, sizeof(legacy)); records[1].size = sizeof(legacy);
    reboot_runtime(); start_access(); assert_status(true, false, false);
    records[1].data[0] ^= 1; reboot_runtime(); assert(jetkvm_access_init(NULL) != ESP_OK);
    memset(records, 0, sizeof(records)); reboot_runtime(); start_access();
    assert(SETUP("{\"localAuthMode\":\"noPassword\"}") == 201);
    const httpd_uri_t device = {.uri = "/device", .method = HTTP_GET, .handler = answer_ok};
    assert(jetkvm_access_register((void *)1, &device) == ESP_OK);
    const char *const answered[] = {"mini.local", "MINI.Local.", "mini", "mini:8080",
        "mini-ts.tail0.ts.net", "192.0.2.10", "192.0.2.10:80"};
    const char *const refused[] = {"evil.example", "evil.example:80", "mini.local.evil.example",
        "mini.evil.example", "evil-mini.local", "192.0.2.11", "192.0.2.10:", "192.0.2.10:8x",
        "[192.0.2.10]", "", "mini.local:80:80"};
    for (size_t i = 0; i < sizeof(answered) / sizeof(answered[0]); ++i) check_host(answered[i], true);
    for (size_t i = 0; i < sizeof(refused) / sizeof(refused[0]); ++i) check_host(refused[i], false);
    server_ip = "::ffff:192.0.2.10"; check_host("192.0.2.10", true);
    server_ip = "192.0.2.10"; check_host("[::ffff:192.0.2.10]", true); check_host("[::ffff:192.0.2.11]", false);
    server_ip = "2001:db8::10";
    check_host("[2001:db8::10]", true); check_host("[2001:db8::10]:80", true);
    check_host("2001:db8::10", false); check_host("[2001:db8::11]", false);
    check_host("[2001:db8::10", false); check_host("192.0.2.10", false);
    /* With a password after setup the Host is not checked: a page under
     * another name has no login cookie. A Mini without a password, and one
     * that is not set up, answer their own names only. */
    char password_cookie[192];
    memset(records, 0, sizeof(records)); reboot_runtime(); start_access();
    assert(jetkvm_access_register((void *)1, &device) == ESP_OK);
    host_header = "evil.example";
    assert(request("/device/setup", HTTP_POST, "{\"localAuthMode\":\"noPassword\"}", "http://evil.example", NULL) == 421);
    assert(request("/device", HTTP_GET, "", NULL, NULL) == 421);
    host_header = "mini.local";
    assert(SETUP("{\"localAuthMode\":\"password\",\"password\":\"12345678\"}") == 201);
    strcpy(password_cookie, response_cookie);
    host_header = "evil.example";
    assert(request("/device", HTTP_GET, "", "http://evil.example", NULL) == 401);
    assert(request("/device", HTTP_GET, "", NULL, password_cookie) == 200);
    assert(request("/auth/login-local", HTTP_POST, "{\"password\":\"12345678\"}", NULL, NULL) == 200);
    strcpy(password_cookie, response_cookie);
    httpd_req_t foreign_socket = {.handle = (void *)1, .uri = "/webrtc/signaling/client",
        .method = HTTP_GET, .origin = "http://evil.example", .cookie = password_cookie};
    assert(jetkvm_access_check_request(&foreign_socket, true) == ESP_OK);
    host_header = "mini.local";
    assert(request("/auth/local-password", HTTP_DELETE, "{\"password\":\"12345678\"}", NULL, password_cookie) == 200);
    check_host("evil.example", false);
    /* Other names for the Mini: valid DNS host names, compared without case
     * and trailing dot, stored across a restart. An invalid list changes
     * nothing. */
    char names[JETKVM_ACCESS_EXTRA_HOST_NAMES + 1][JETKVM_ACCESS_HOST_NAME_CAPACITY];
    const char *extra[] = {"KVM.Lab.Example.", "rack-3"};
    assert(jetkvm_access_get_extra_host_names(names, JETKVM_ACCESS_EXTRA_HOST_NAMES) == 0);
    assert(!jetkvm_access_set_extra_host_names(extra, 2));
    assert(jetkvm_access_get_extra_host_names(names, JETKVM_ACCESS_EXTRA_HOST_NAMES) == 2);
    assert(!strcmp(names[0], "kvm.lab.example") && !strcmp(names[1], "rack-3"));
    check_host("kvm.lab.example", true); check_host("KVM.LAB.EXAMPLE.:8080", true); check_host("rack-3", true);
    check_host("rack-3.local", false); check_host("lab.example", false); check_host("x.kvm.lab.example", false);
    char long_label[80], long_name[140];
    memset(long_label, 'a', 64); long_label[64] = 0;
    for (int i = 0; i < 128; ++i) long_name[i] = i % 2 ? '.' : 'a';
    long_name[127] = 'a'; long_name[128] = 0;
    const char *invalid[] = {"", ".", "bad_name", "-rack", "rack-", "a..b", "kvm.lab..", "a b", "kvm/x", "192.0.2.11",
        "kvm.local:80", long_label, long_name};
    for (size_t i = 0; i < sizeof(invalid) / sizeof(invalid[0]); ++i) {
        const char *one[] = {"good", invalid[i]};
        assert(jetkvm_access_set_extra_host_names(one, 2));
    }
    const char *nine[JETKVM_ACCESS_EXTRA_HOST_NAMES + 1];
    for (size_t i = 0; i <= JETKVM_ACCESS_EXTRA_HOST_NAMES; ++i) nine[i] = "rack-3";
    assert(jetkvm_access_set_extra_host_names(nine, JETKVM_ACCESS_EXTRA_HOST_NAMES + 1));
    assert(!jetkvm_access_set_extra_host_names(nine, JETKVM_ACCESS_EXTRA_HOST_NAMES));
    assert(!jetkvm_access_set_extra_host_names(extra, 2));
    fail_save = true;
    assert(jetkvm_access_set_extra_host_names(extra, 1));
    fail_save = false;
    assert(jetkvm_access_get_extra_host_names(names, JETKVM_ACCESS_EXTRA_HOST_NAMES) == 2);
    reboot_runtime(); start_access();
    assert(jetkvm_access_register((void *)1, &device) == ESP_OK);
    assert(jetkvm_access_get_extra_host_names(names, 1) == 1 && !strcmp(names[0], "kvm.lab.example"));
    check_host("kvm.lab.example", true); check_host("rack-3", true);
    assert(!jetkvm_access_set_extra_host_names(NULL, 0));
    check_host("kvm.lab.example", false);
    reboot_runtime(); start_access();
    assert(jetkvm_access_get_extra_host_names(names, JETKVM_ACCESS_EXTRA_HOST_NAMES) == 0);
    puts("Setup/reset integration: HTTP modes, persistence, rejection, login, legacy records, erasure and failed-phase recovery: PASS");
}
