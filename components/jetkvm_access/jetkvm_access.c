#include "password_pbkdf2.h"
#include "esp_attr.h"
#include "esp_heap_caps.h"
#include "access_contract.h"
#include "jetkvm_access.h"
#include "jetkvm_factory_reset.h"
#include "password_core.h"
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include "cJSON.h"
#include "esp_random.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "freertos/idf_additions.h"
#include "nvs.h"
#include "lwip/sockets.h"
#include "lwip/inet.h"
#include "psa/crypto.h"

#define TOKEN_SECONDS (7U * 24U * 60U * 60U)
#define ROUTES_PER_SERVER 36
typedef struct { httpd_uri_t original; } guarded_route_t;
typedef struct {
    httpd_handle_t server;
    bool secure;
    uint32_t request_epoch;
    size_t count;
    guarded_route_t routes[ROUTES_PER_SERVER];
} access_listener_t;
static access_listener_t s_listeners[2];
static SemaphoreHandle_t s_mutex;
static jetkvm_password_record_t s_password;
static char s_token[65];
static atomic_uint s_epoch;
static atomic_uint s_expiry;
static atomic_bool s_enabled;
static atomic_bool s_setup;
typedef struct {
    char ip[INET6_ADDRSTRLEN];
    int64_t next_attempt_us, expires_us;
    unsigned failures, lockouts;
} login_limit_t;
static EXT_RAM_BSS_ATTR login_limit_t s_login_limits[32];
static bool s_password_busy;

/* Same numbers as upstream ratelimit.go so the UI's login flow sees identical behaviour. */
#define LOGIN_MAX_ATTEMPTS 5U
#define LOGIN_LOCKOUT_US (INT64_C(15) * 60 * 1000000)
/* Doublings of the 15 minute lockout: 15, 30, then 60 minutes at most. */
#define LOGIN_MAX_BACKOFF 2U
static void (*s_revoke_local)(void);
static size_t (*s_host_names)(char (*names)[JETKVM_ACCESS_HOST_NAME_CAPACITY], size_t capacity);
static char s_extra_names[JETKVM_ACCESS_EXTRA_HOST_NAMES][JETKVM_ACCESS_HOST_NAME_CAPACITY];
static size_t s_extra_count;

/* Use the socket peer, never a caller-supplied forwarding header. Keep active
 * lockouts when the bounded table fills; reclaim only idle entries. */
static login_limit_t *login_limit(httpd_req_t *req)
{
    struct sockaddr_storage peer;
    socklen_t size = sizeof(peer);
    char ip[INET6_ADDRSTRLEN];
    if (getpeername(httpd_req_to_sockfd(req), (struct sockaddr *)&peer, &size) != 0)
        return NULL;
    const void *address = peer.ss_family == AF_INET ?
        (const void *)&((struct sockaddr_in *)&peer)->sin_addr :
        (const void *)&((struct sockaddr_in6 *)&peer)->sin6_addr;
    int family = peer.ss_family;
    if (family == AF_INET6 && IN6_IS_ADDR_V4MAPPED((const struct in6_addr *)address)) {
        address = (const unsigned char *)address + 12;
        family = AF_INET;
    }
    if (!inet_ntop(family, address, ip, sizeof(ip))) return NULL;
    int64_t now = esp_timer_get_time();
    login_limit_t *available = NULL;
    for (size_t i = 0; i < sizeof(s_login_limits) / sizeof(s_login_limits[0]); ++i) {
        login_limit_t *entry = &s_login_limits[i];
        if (entry->expires_us <= now) memset(entry, 0, sizeof(*entry));
        if (strcmp(entry->ip, ip) == 0) return entry;
        if (!entry->ip[0]) available = entry;
    }
    if (available) {
        snprintf(available->ip, sizeof(available->ip), "%s", ip);
        available->expires_us = now + (LOGIN_LOCKOUT_US << LOGIN_MAX_BACKOFF);
    }
    return available;
}

static void wipe(void *p, size_t n)
{
    volatile uint8_t *b = p;
    while (n--) *b++ = 0;
}
typedef struct {
    const char *password;
    const uint8_t *salt;
    uint32_t cost;
    uint8_t *hash;
    SemaphoreHandle_t done;
    bool result;
} derivation_t;
static void derivation_yield(void) { vTaskDelay(1); }
static void derive_worker(void *argument)
{
    derivation_t *job = argument;
    job->result = jetkvm_password_pbkdf2(job->password, job->salt, job->cost, job->hash, derivation_yield);
    xSemaphoreGive(job->done);
    vTaskDeleteWithCaps(NULL);
}
static bool derive(const char *password, const uint8_t salt[16], uint32_t cost,
                   uint8_t hash[32], void *ctx)
{
    (void)ctx;
    StaticSemaphore_t storage;
    derivation_t job = {.password=password, .salt=salt, .cost=cost, .hash=hash,
        .done=xSemaphoreCreateBinaryStatic(&storage)};
    /* This task never writes flash, so its small working stack can use normal
     * internal SRAM. The HTTP task retains its cache-safe stack for NVS. */
    bool created = xTaskCreateWithCaps(derive_worker, "password", 4096, &job, 1, NULL,
                                      MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT) == pdPASS;
    if (created) xSemaphoreTake(job.done, portMAX_DELAY);
    vSemaphoreDelete(job.done);
    return created && job.result;
}
static bool random_bytes(uint8_t *bytes, size_t size, void *ctx)
{
    (void)ctx;
    esp_fill_random(bytes, size);
    return true;
}
static bool save_password(const jetkvm_password_record_t *record, void *ctx)
{
    (void)ctx;
    nvs_handle_t handle;
    if (nvs_open("jk_access", NVS_READWRITE, &handle) != ESP_OK) return false;
    esp_err_t err = nvs_set_blob(handle, "password", record, sizeof(*record));
    if (err == ESP_OK) err = nvs_commit(handle);
    nvs_close(handle);
    return err == ESP_OK;
}
static bool stage_password(const jetkvm_password_record_t *record, void *ctx)
{
    (void)record; (void)ctx;
    return true; /* Publication and NVS commit happen after the epoch check. */
}
static const jetkvm_password_backend_t s_backend = {
    .derive = derive, .random = random_bytes, .save = stage_password,
};
bool jetkvm_access_password_enabled(void) { return atomic_load(&s_enabled); }
bool jetkvm_access_is_setup(void) { return atomic_load(&s_setup); }
/* A session (signaling socket, peer, queued control) stays valid for the
 * epoch it was admitted under. Upstream keeps a connected page working after
 * its cookie is cleared or expires; only a password change, setup or login
 * (a new epoch) revokes it. Cookie admission also needs a fresh token. */
bool jetkvm_access_epoch_valid(uint32_t epoch)
{
    return epoch && epoch == atomic_load(&s_epoch);
}
static bool token_fresh(void)
{
    return !atomic_load(&s_enabled) ||
        (int32_t)(atomic_load(&s_expiry) - xTaskGetTickCount()) > 0;
}
bool jetkvm_access_lock_epoch(uint32_t epoch)
{
    if (!s_mutex || xSemaphoreTakeRecursive(s_mutex, portMAX_DELAY) != pdTRUE) return false;
    if (jetkvm_access_epoch_valid(epoch)) return true;
    xSemaphoreGiveRecursive(s_mutex);
    return false;
}
void jetkvm_access_unlock(void) { xSemaphoreGiveRecursive(s_mutex); }

static access_listener_t *listener(httpd_handle_t server)
{
    for (size_t i = 0; i < 2; ++i) if (s_listeners[i].server == server) return &s_listeners[i];
    return NULL;
}
uint32_t jetkvm_access_request_epoch(httpd_req_t *req)
{
    access_listener_t *owner = listener(req->handle);
    return owner ? owner->request_epoch : 0;
}
/* JSON API responses keep their connection like upstream's Go server: the
 * UI's route loader then re-uses a warm socket and its 401 redirect lands
 * before a rejected WebSocket upgrade makes the page reload itself. A
 * rejection with an unread body still closes, which drains that body. The
 * close header and the handler result must agree: HTTPD closes only on a
 * failed handler, and a client that saw no close header would reuse the
 * socket and get a reset instead of its next reply. */
static bool rejection_closes(httpd_req_t *req)
{
    /* An unread body would poison the next request on the connection, and a
     * refused WebSocket upgrade ends the connection: the pre-handshake hook
     * can only fail the upgrade, after which HTTPD closes the socket. */
    return req->content_len > 0 || httpd_req_get_hdr_value_len(req, "Upgrade") > 0;
}
esp_err_t jetkvm_access_rejection_result(httpd_req_t *req)
{
    return rejection_closes(req) ? ESP_FAIL : ESP_OK;
}
static esp_err_t json_error(httpd_req_t *req, const char *status, const char *message)
{
    httpd_resp_set_status(req, status);
    if (rejection_closes(req)) httpd_resp_set_hdr(req, "Connection", "close");
    httpd_resp_set_type(req, "application/json");
    /* no-cache, not no-store: Chromium only drains an unread fetch body into
     * its cache, and the shared UI never reads the /device 401 body it
     * redirects on.  With no-store that fetch stays in flight and pages
     * waiting for network idle never settle.  Upstream sends no directive. */
    httpd_resp_set_hdr(req, "Cache-Control", "no-cache");
    char body[128];
    snprintf(body, sizeof(body), "{\"error\":\"%s\"}", message);
    (void)httpd_resp_sendstr(req, body);
    return jetkvm_access_rejection_result(req);
}
void jetkvm_access_set_host_names(size_t (*names)(char (*names)[JETKVM_ACCESS_HOST_NAME_CAPACITY],
                                                  size_t capacity))
{
    s_host_names = names;
}
static bool is_local_address(httpd_req_t *req, int family, const uint8_t *address)
{
    struct sockaddr_storage local;
    socklen_t size = sizeof(local);
    if (getsockname(httpd_req_to_sockfd(req), (struct sockaddr *)&local, &size) != 0) return false;
    const uint8_t *own = local.ss_family == AF_INET ?
        (const uint8_t *)&((struct sockaddr_in *)&local)->sin_addr :
        (const uint8_t *)&((struct sockaddr_in6 *)&local)->sin6_addr;
    int own_family = local.ss_family;
    if (own_family == AF_INET6 && IN6_IS_ADDR_V4MAPPED((const struct in6_addr *)own)) {
        own += 12;
        own_family = AF_INET;
    }
    return own_family == family && memcmp(own, address, family == AF_INET ? 4 : 16) == 0;
}
/* A DNS-rebinding page reaches the device under the attacker's name, which
 * the browser sends as Host and as Origin. Answer only the address the client
 * connected to and the device's own names, with or without an Origin. */
/* A DNS host name of dot-separated labels of letters, digits and hyphens,
 * in lower case and without a trailing dot. An IPv4 address is not one. */
static bool normalize_host_name(const char *name, char normalized[JETKVM_ACCESS_HOST_NAME_CAPACITY])
{
    size_t length = name ? strlen(name) : 0;
    if (length && name[length - 1] == '.') --length;
    if (!length || length >= JETKVM_ACCESS_HOST_NAME_CAPACITY) return false;
    size_t label = 0;
    for (size_t i = 0; i < length; ++i) {
        char c = name[i];
        if (c >= 'A' && c <= 'Z') c = (char)(c - 'A' + 'a');
        bool end = i + 1 == length || name[i + 1] == '.';
        if (c == '.') {
            label = 0;
        } else if ((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || (c == '-' && label && !end)) {
            if (++label > 63) return false;
        } else {
            return false;
        }
        if (c == '.' && (i == 0 || name[i - 1] == '.')) return false;
        normalized[i] = c;
    }
    normalized[length] = '\0';
    if (normalized[length - 1] == '.') return false;
    struct in_addr ipv4;
    return inet_pton(AF_INET, normalized, &ipv4) != 1;
}
size_t jetkvm_access_get_extra_host_names(char (*names)[JETKVM_ACCESS_HOST_NAME_CAPACITY], size_t capacity)
{
    if (!s_mutex) return 0;
    xSemaphoreTakeRecursive(s_mutex, portMAX_DELAY);
    size_t count = s_extra_count < capacity ? s_extra_count : capacity;
    memcpy(names, s_extra_names, count * sizeof(s_extra_names[0]));
    jetkvm_access_unlock();
    return count;
}
const char *jetkvm_access_set_extra_host_names(const char *const *names, size_t count)
{
    /* Static, as the caller's stack may be small; the mutex orders setters. */
    static char staged[JETKVM_ACCESS_EXTRA_HOST_NAMES][JETKVM_ACCESS_HOST_NAME_CAPACITY];
    static char stored[sizeof(staged)];
    if (count > JETKVM_ACCESS_EXTRA_HOST_NAMES) return "At most 8 other names";
    if (!s_mutex) return "Access control unavailable";
    xSemaphoreTakeRecursive(s_mutex, portMAX_DELAY);
    stored[0] = '\0';
    for (size_t i = 0; i < count; ++i) {
        if (!normalize_host_name(names[i], staged[i])) {
            jetkvm_access_unlock();
            return "A name has letters, digits and hyphens in dot-separated labels of at most 63 characters, "
                   "at most 127 characters in all, and is not an IP address";
        }
        if (i) strcat(stored, "\n");
        strcat(stored, staged[i]);
    }
    nvs_handle_t handle;
    esp_err_t err = nvs_open("jk_access", NVS_READWRITE, &handle);
    if (err == ESP_OK) {
        err = nvs_set_str(handle, "host_names", stored);
        if (err == ESP_OK) err = nvs_commit(handle);
        nvs_close(handle);
    }
    if (err == ESP_OK) {
        memcpy(s_extra_names, staged, count * sizeof(staged[0]));
        s_extra_count = count;
    }
    jetkvm_access_unlock();
    return err == ESP_OK ? NULL : "The names could not be stored";
}
static bool extra_host_name(const char *name)
{
    if (!s_mutex) return false;
    bool match = false;
    xSemaphoreTakeRecursive(s_mutex, portMAX_DELAY);
    for (size_t i = 0; i < s_extra_count; ++i) match |= strcasecmp(name, s_extra_names[i]) == 0;
    jetkvm_access_unlock();
    return match;
}
static void load_extra_host_names(nvs_handle_t handle)
{
    static char stored[JETKVM_ACCESS_EXTRA_HOST_NAMES * JETKVM_ACCESS_HOST_NAME_CAPACITY];
    size_t size = sizeof(stored);
    s_extra_count = 0;
    if (nvs_get_str(handle, "host_names", stored, &size) != ESP_OK) return;
    for (char *line = stored, *next; line && s_extra_count < JETKVM_ACCESS_EXTRA_HOST_NAMES; line = next) {
        next = strchr(line, '\n');
        if (next) *next++ = '\0';
        if (normalize_host_name(line, s_extra_names[s_extra_count])) ++s_extra_count;
    }
}
static bool host_allowed(httpd_req_t *req)
{
    char host[256];
    if (httpd_req_get_hdr_value_str(req, "Host", host, sizeof(host)) != ESP_OK) return false;
    char *name = host, *rest;
    if (*name == '[') {
        rest = strchr(++name, ']');
        if (!rest) return false;
        *rest++ = '\0';
    } else {
        rest = name + strcspn(name, ":");
    }
    if (*rest == ':') {
        *rest++ = '\0';
        if (!*rest || rest[strspn(rest, "0123456789")]) return false;
    } else if (*rest) {
        return false;
    }
    uint8_t address[16];
    if (host[0] == '[') {
        static const uint8_t mapped[12] = {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0xff, 0xff};
        if (inet_pton(AF_INET6, name, address) != 1) return false;
        if (memcmp(address, mapped, sizeof(mapped)) != 0) return is_local_address(req, AF_INET6, address);
        memmove(address, address + sizeof(mapped), 4);
        return is_local_address(req, AF_INET, address);
    }
    if (inet_pton(AF_INET, name, address) == 1) return is_local_address(req, AF_INET, address);
    size_t length = strlen(name);
    if (length && name[length - 1] == '.') name[--length] = '\0';
    if (!length) return false;
    char names[4][JETKVM_ACCESS_HOST_NAME_CAPACITY];
    size_t count = s_host_names ? s_host_names(names, sizeof(names) / sizeof(names[0])) : 0;
    if (extra_host_name(name)) return true;
    for (size_t i = 0; i < count; ++i) {
        size_t own = strlen(names[i]);
        if (own && names[i][own - 1] == '.') --own;
        if (own && strncasecmp(name, names[i], own) == 0 &&
            (!name[own] || strcasecmp(name + own, ".local") == 0)) return true;
    }
    return false;
}
esp_err_t jetkvm_access_check_host(httpd_req_t *req)
{
    /* With a password after setup, a page under another name has no login
     * cookie and gets 401, so only the setup and a Mini without a password
     * need the Host check. */
    if ((atomic_load(&s_setup) && atomic_load(&s_enabled)) || host_allowed(req)) return ESP_OK;
    (void)json_error(req, "421 Misdirected Request", "Unknown host");
    return ESP_ERR_INVALID_STATE;
}
static bool origin_allowed(httpd_req_t *req, bool required)
{
    access_listener_t *owner = listener(req->handle);
    if (!owner) return false;
    if (!httpd_req_get_hdr_value_len(req, "Origin")) return !required;
    char host[256], origin[272], expected[272];
    if (httpd_req_get_hdr_value_str(req, "Host", host, sizeof(host)) != ESP_OK ||
        httpd_req_get_hdr_value_str(req, "Origin", origin, sizeof(origin)) != ESP_OK) return false;
    snprintf(expected, sizeof(expected), "%s://%s", owner->secure ? "https" : "http", host);
    return strcmp(origin, expected) == 0;
}
static bool cookie_matches(httpd_req_t *req)
{
    char cookie[1024];
    if (!s_token[0] || httpd_req_get_hdr_value_str(req, "Cookie", cookie, sizeof(cookie)) != ESP_OK) return false;
    bool match = false;
    char *part = cookie;
    while (part && *part) {
        while (*part == ' ' || *part == ';') ++part;
        char *end = strchr(part, ';');
        size_t length = end ? (size_t)(end - part) : strlen(part);
        if (length == 10 + 64 && memcmp(part, "authToken=", 10) == 0 &&
            jetkvm_password_equal(part + 10, s_token, 64)) match = true;
        part = end ? end + 1 : NULL;
    }
    wipe(cookie, sizeof(cookie));
    return match;
}
esp_err_t jetkvm_access_check_request(httpd_req_t *req, bool require_origin)
{
    access_listener_t *owner = listener(req->handle);
    if (!owner || !s_mutex) return ESP_FAIL;
    owner->request_epoch = 0;
    if (jetkvm_access_check_host(req) != ESP_OK) return ESP_ERR_INVALID_STATE;
    if (!origin_allowed(req, require_origin)) {
        (void)json_error(req, "403 Forbidden", "Device UI origin required");
        return ESP_ERR_INVALID_STATE;
    }
    xSemaphoreTakeRecursive(s_mutex, portMAX_DELAY);
    uint32_t epoch = atomic_load(&s_epoch);
    bool setup = atomic_load(&s_setup);
    bool allowed = setup && jetkvm_access_epoch_valid(epoch) &&
                   (!s_password.enabled || (token_fresh() && cookie_matches(req)));
    if (allowed) owner->request_epoch = epoch;
    jetkvm_access_unlock();
    if (allowed) return ESP_OK;
    (void)json_error(req, "401 Unauthorized", setup ? "Authentication required" : "Setup required");
    return ESP_ERR_INVALID_STATE;
}
static void rotate_token(bool issue)
{
    /* Called under the authority mutex; admission and mutations cannot cross
     * this boundary. Invalidate queued local control before issuing a token. */
    unsigned epoch = atomic_fetch_add(&s_epoch, 1) + 1;
    if (!epoch) atomic_fetch_add(&s_epoch, 1);
    atomic_store(&s_expiry, xTaskGetTickCount());
    wipe(s_token, sizeof(s_token));
    if (s_revoke_local) s_revoke_local();
    if (issue) {
        uint8_t bytes[32];
        esp_fill_random(bytes, sizeof(bytes));
        for (size_t i = 0; i < sizeof(bytes); ++i) snprintf(s_token + 2*i, 3, "%02x", bytes[i]);
        wipe(bytes, sizeof(bytes));
        atomic_store(&s_expiry, xTaskGetTickCount() + jetkvm_access_lifetime_ticks(TOKEN_SECONDS, configTICK_RATE_HZ));
    }
    atomic_store(&s_enabled, s_password.enabled != 0);
}
/* Logout forgets the cookie token so the next page load must log in again,
 * but it does not start a new epoch: the page that logged out keeps its
 * live session until it navigates, as upstream JetKVM does. Revoking here
 * would close the signaling socket; the UI would then probe /device, get 401,
 * and reload itself during its own navigation. */
static void clear_token(void)
{
    atomic_store(&s_expiry, xTaskGetTickCount());
    wipe(s_token, sizeof(s_token));
    atomic_store(&s_enabled, s_password.enabled != 0);
}
static bool request_path_is(const httpd_req_t *req, const char *path)
{
    /* HTTPD matches the path without its query string; auth decisions must
     * use that same path even though req->uri includes the query. */
    size_t length = strlen(path);
    return strncmp(req->uri, path, length) == 0 &&
        (req->uri[length] == '\0' || req->uri[length] == '?');
}
static esp_err_t auth_handler(httpd_req_t *req)
{
    if (!origin_allowed(req, false)) return json_error(req, "403 Forbidden", "Device UI origin required");
    char body[1024] = {0};
    cJSON *json = NULL;
    bool logout = request_path_is(req, "/auth/logout");
    if (!logout) {
        if (req->content_len == 0 || req->content_len >= sizeof(body)) return json_error(req, "400 Bad Request", "Invalid password request");
        size_t received = 0;
        int64_t deadline = esp_timer_get_time() + 10000000;
        while (received < req->content_len) {
            if (esp_timer_get_time() >= deadline) { wipe(body, sizeof(body)); return json_error(req, "408 Request Timeout", "Incomplete request"); }
            int count = httpd_req_recv(req, body + received, req->content_len - received);
            if (count <= 0) { wipe(body, sizeof(body)); return json_error(req, "400 Bad Request", "Incomplete request"); }
            received += count;
        }
        /* cJSON represents strings as C strings. Reject encoded NULs before
         * decoding so credentials cannot be silently truncated. */
        if (!memchr(body, 0, received) && !jetkvm_access_has_encoded_nul(body)) {
            const char *end = NULL;
            json = cJSON_ParseWithLengthOpts(body, received + 1, &end, true);
        }
        wipe(body, sizeof(body));
        if (!cJSON_IsObject(json)) { cJSON_Delete(json); return json_error(req, "400 Bad Request", "Invalid JSON"); }
    }
    const cJSON *password = cJSON_GetObjectItemCaseSensitive(json, "password");
    const cJSON *old = cJSON_GetObjectItemCaseSensitive(json, "oldPassword");
    const cJSON *next = cJSON_GetObjectItemCaseSensitive(json, "newPassword");
    bool login = request_path_is(req, "/auth/login-local");
    bool setup = request_path_is(req, "/device/setup");
    const cJSON *mode = cJSON_GetObjectItemCaseSensitive(json, "localAuthMode");
    bool setup_password = cJSON_IsString(mode) && strcmp(mode->valuestring, "password") == 0;
    bool setup_no_password = cJSON_IsString(mode) && strcmp(mode->valuestring, "noPassword") == 0;
    xSemaphoreTakeRecursive(s_mutex, portMAX_DELAY);
    bool authorized = setup || login || jetkvm_access_epoch_valid(jetkvm_access_request_epoch(req));
    uint32_t operation_epoch = atomic_load(&s_epoch);
    jetkvm_password_record_t staged = s_password;
    jetkvm_password_result_t result = JETKVM_PASSWORD_OK;
    const char *status = "200 OK", *error = NULL;
    bool issue = false;
    login_limit_t *limit = (login || setup) && !s_password_busy ? login_limit(req) : NULL;
    if (setup && (atomic_load(&s_setup) || jetkvm_factory_reset_pending())) {
        status = "409 Conflict"; error = "Setup already completed or factory reset incomplete";
    } else if (setup && (!setup_password && !setup_no_password)) {
        status = "400 Bad Request"; error = "Invalid local authentication mode";
    } else if (setup && setup_no_password && password) {
        status = "400 Bad Request"; error = "Password is not allowed in noPassword mode";
    } else if (!authorized) {
        status = "401 Unauthorized"; error = "Authentication expired";
    } else if (!logout && (s_password_busy || ((login || setup) &&
               (!limit || esp_timer_get_time() < limit->next_attempt_us)))) {
        status = "429 Too Many Requests"; error = "Try again later";
    } else if (login) {
        s_password_busy = true;
        jetkvm_access_unlock();
        result = jetkvm_password_verify(&staged, cJSON_IsString(password) ? password->valuestring : NULL, &s_backend);
        xSemaphoreTakeRecursive(s_mutex, portMAX_DELAY);
        s_password_busy = false;
        if (operation_epoch != atomic_load(&s_epoch)) result = JETKVM_PASSWORD_WRONG_MODE;
        issue = result == JETKVM_PASSWORD_OK;
    } else if (!logout) {
        jetkvm_password_action_t action = req->method == HTTP_POST ? JETKVM_PASSWORD_CREATE :
            req->method == HTTP_PUT ? JETKVM_PASSWORD_UPDATE : JETKVM_PASSWORD_DISABLE;
        s_password_busy = true;
        jetkvm_access_unlock();
        result = setup && setup_no_password ? JETKVM_PASSWORD_OK : jetkvm_password_change(&staged, action,
            cJSON_IsString(action == JETKVM_PASSWORD_UPDATE ? old : password) ?
                (action == JETKVM_PASSWORD_UPDATE ? old : password)->valuestring : NULL,
            cJSON_IsString(action == JETKVM_PASSWORD_CREATE ? password : next) ?
                (action == JETKVM_PASSWORD_CREATE ? password : next)->valuestring : NULL, &s_backend);
        xSemaphoreTakeRecursive(s_mutex, portMAX_DELAY);
        s_password_busy = false;
        if (!jetkvm_access_epoch_valid(operation_epoch)) result = JETKVM_PASSWORD_WRONG_MODE;
        if (setup && (atomic_load(&s_setup) || jetkvm_factory_reset_pending()))
            result = JETKVM_PASSWORD_WRONG_MODE;
        if (result == JETKVM_PASSWORD_OK) {
            if (!save_password(&staged, NULL)) result = JETKVM_PASSWORD_STORAGE_ERROR;
            else {
                s_password = staged;
                /* A valid persisted record is the setup marker, including an
                 * explicit noPassword choice. Reset erases both atomically. */
                atomic_store(&s_setup, true);
            }
        }
        issue = result == JETKVM_PASSWORD_OK && staged.enabled;
        if (action == JETKVM_PASSWORD_CREATE) status = "201 Created";
    }
    if (result == JETKVM_PASSWORD_DENIED) {
        /* Upstream ratelimit.go: five failures are answered with 401, the
         * next ones lock the endpoint for 15 minutes, doubling per lockout up
         * to one hour. A successful login clears the counters. */
        if (limit) {
            limit->expires_us = esp_timer_get_time() + (LOGIN_LOCKOUT_US << LOGIN_MAX_BACKOFF);
            if (++limit->failures >= LOGIN_MAX_ATTEMPTS) {
                limit->next_attempt_us = esp_timer_get_time() +
                    (LOGIN_LOCKOUT_US << limit->lockouts);
                if (limit->lockouts < LOGIN_MAX_BACKOFF) ++limit->lockouts;
                limit->failures = 0;
            }
        }
        status = "401 Unauthorized"; error = "Invalid password";
    } else if (result != JETKVM_PASSWORD_OK) {
        status = result == JETKVM_PASSWORD_STORAGE_ERROR || result == JETKVM_PASSWORD_CRYPTO_ERROR ?
            "500 Internal Server Error" : "400 Bad Request";
        error = "Password operation failed";
    }
    wipe(&staged, sizeof(staged));
    char cookie[192] = {0};
    if (!error) {
        if (limit) memset(limit, 0, sizeof(*limit));
        if (logout) clear_token();
        else rotate_token(issue);
        /* Match upstream's shared HTTP/HTTPS cookie so disabling TLS cannot
         * leave a Secure cookie that the HTTP login flow cannot replace. */
        snprintf(cookie, sizeof(cookie), "authToken=%s; Path=/; Max-Age=%u; HttpOnly; SameSite=Strict",
                 s_token, issue ? TOKEN_SECONDS : 0);
    }
    /* Password strings are never logged or retained after derivation. */
    for (cJSON *field = json ? json->child : NULL; field; field = field->next)
        if (cJSON_IsString(field) && field->valuestring) wipe(field->valuestring, strlen(field->valuestring));
    cJSON_Delete(json);
    esp_err_t err;
    if (error) err = json_error(req, status, error);
    else {
        httpd_resp_set_status(req, status);
        httpd_resp_set_type(req, "application/json");
        httpd_resp_set_hdr(req, "Cache-Control", "no-cache");
        httpd_resp_set_hdr(req, "Set-Cookie", cookie);
        if (logout) httpd_resp_set_hdr(req, "Connection", "close");
        err = httpd_resp_sendstr(req, "{}");
    }
    wipe(cookie, sizeof(cookie));
    jetkvm_access_unlock();
    return logout ? ESP_FAIL : err;
}
static esp_err_t guarded_handler(httpd_req_t *req)
{
    guarded_route_t *route = req->user_ctx;
    /* A handler compiled against a different httpd_req_t layout reads a
     * stale field here; fail the request instead of dereferencing NULL. */
    if (!route || !route->original.uri)
        return json_error(req, "500 Internal Server Error", "Route registration incomplete");
    bool public = strcmp(route->original.uri, "/device/status") == 0 ||
                  strcmp(route->original.uri, "/device/setup") == 0 ||
                  strcmp(route->original.uri, "/auth/login-local") == 0;
    bool upload = strcmp(route->original.uri, "/storage/upload") == 0;
    if ((public ? jetkvm_access_check_host(req) : jetkvm_access_check_request(req, false)) != ESP_OK)
        return jetkvm_access_rejection_result(req);
    bool credential = strncmp(route->original.uri, "/auth/", 6) == 0;
    bool signal = strcmp(route->original.uri, "/webrtc/signal") == 0;
    bool storage_control = !upload && req->method != HTTP_GET &&
                           strncmp(route->original.uri, "/storage/", 9) == 0;
    /* Storage control responses close: their handlers may leave stalled
     * bodies unread. Other JSON responses keep their connection. */
    bool close_response = storage_control;
    if (close_response) httpd_resp_set_hdr(req, "Connection", "close");
    bool locked = !public && !upload && !credential && !signal;
    if (locked && !jetkvm_access_lock_epoch(jetkvm_access_request_epoch(req)))
        return json_error(req, "401 Unauthorized", "Authentication expired");
    req->user_ctx = route->original.user_ctx;
    esp_err_t err = route->original.handler(req);
    req->user_ctx = route;
    if (locked) jetkvm_access_unlock();
    /* Storage errors must not drain stalled bodies. */
    return close_response ? ESP_FAIL : err;
}
esp_err_t jetkvm_access_register(httpd_handle_t server, const httpd_uri_t *uri)
{
    access_listener_t *owner = listener(server);
    if (!owner || owner->count == ROUTES_PER_SERVER) return ESP_ERR_NO_MEM;
    guarded_route_t *route = &owner->routes[owner->count];
    route->original = *uri;
    httpd_uri_t guarded = *uri;
    guarded.handler = guarded_handler;
    guarded.user_ctx = route;
    esp_err_t err = httpd_register_uri_handler(server, &guarded);
    if (err == ESP_OK) ++owner->count;
    return err;
}
esp_err_t jetkvm_access_attach(httpd_handle_t server, bool secure)
{
    access_listener_t *owner = NULL;
    for (size_t i = 0; i < 2; ++i) if (!s_listeners[i].server) { owner = &s_listeners[i]; break; }
    if (!owner) return ESP_ERR_NO_MEM;
    *owner = (access_listener_t){.server = server, .secure = secure};
    const httpd_uri_t routes[] = {
        {.uri="/device/setup", .method=HTTP_POST, .handler=auth_handler},
        {.uri="/auth/login-local", .method=HTTP_POST, .handler=auth_handler},
        {.uri="/auth/logout", .method=HTTP_POST, .handler=auth_handler},
        {.uri="/auth/password-local", .method=HTTP_POST, .handler=auth_handler},
        {.uri="/auth/password-local", .method=HTTP_PUT, .handler=auth_handler},
        {.uri="/auth/local-password", .method=HTTP_DELETE, .handler=auth_handler},
    };
    for (size_t i = 0; i < sizeof(routes)/sizeof(routes[0]); ++i) {
        esp_err_t err = jetkvm_access_register(server, &routes[i]);
        if (err != ESP_OK) return err;
    }
    return ESP_OK;
}
void jetkvm_access_detach(httpd_handle_t server)
{
    access_listener_t *owner = listener(server);
    if (owner) memset(owner, 0, sizeof(*owner)); /* Only after HTTPD joined. */
}
esp_err_t jetkvm_access_init(void (*revoke_local)(void))
{
    if (s_mutex) return ESP_OK;
    if (psa_crypto_init() != PSA_SUCCESS) return ESP_FAIL;
    jetkvm_password_defaults(&s_password);
    bool setup = false;
    nvs_handle_t handle;
    esp_err_t err = nvs_open("jk_access", NVS_READONLY, &handle);
    if (err == ESP_OK) {
        size_t size = sizeof(s_password);
        err = nvs_get_blob(handle, "password", &s_password, &size);
        nvs_close(handle);
        if (err == ESP_ERR_NVS_NOT_FOUND) { jetkvm_password_defaults(&s_password); err = ESP_OK; }
        else if (err == ESP_OK && size != sizeof(s_password)) err = ESP_ERR_INVALID_SIZE;
        else if (err == ESP_OK) setup = true;
        nvs_handle_t names;
        if (nvs_open("jk_access", NVS_READONLY, &names) == ESP_OK) {
            load_extra_host_names(names);
            nvs_close(names);
        }
    } else if (err == ESP_ERR_NVS_NOT_FOUND) err = ESP_OK;
    if (err != ESP_OK || !jetkvm_password_record_valid(&s_password)) return ESP_FAIL;
    s_mutex = xSemaphoreCreateRecursiveMutex();
    if (!s_mutex) return ESP_ERR_NO_MEM;
    s_revoke_local = revoke_local;
    atomic_store(&s_epoch, 1);
    atomic_store(&s_expiry, xTaskGetTickCount());
    atomic_store(&s_enabled, s_password.enabled != 0);
    atomic_store(&s_setup, setup);
    return ESP_OK;
}
