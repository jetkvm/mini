#include "jetkvm_tailscale.h"
#include "jetkvm_network_settings.h"
#include "microlink.h"
#include "esp_log.h"
#include "esp_attr.h"
#include "esp_netif.h"
#include "esp_heap_caps.h"
#include "freertos/idf_additions.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "nvs.h"
#include "psa/crypto.h"
#include <string.h>
#include <stdio.h>

#ifdef CONFIG_ML_MAX_PEERS
_Static_assert(CONFIG_ML_MAX_PEERS == 64, "the peer table health message names 64 devices");
#endif

static const char *TAG = "tailscale";
static SemaphoreHandle_t s_mutex;
static TaskHandle_t s_task;
/* Accessed only by tasks with the cache enabled. Keep disabled Tailscale from
 * consuming the internal RAM reserved for existing TLS and DMA operations. */
static EXT_RAM_BSS_ATTR jetkvm_tailscale_config_t s_config;
static EXT_RAM_BSS_ATTR jetkvm_tailscale_status_t s_status;
static unsigned s_generation;
static uint8_t s_registered_digest[32];
static bool s_has_registered_digest;
static bool s_needs_login;
static bool s_registration_completed;
/* A log out waits for the worker to stop the client before the node identity
 * is erased; no client starts while it waits. NVS keeps the pending log out
 * across a restart, so the next start erases the identity first. */
static bool s_logout_pending;
/* The health line is the worker's own report (waiting, retrying, stopping),
 * which the worker clears when it no longer applies, not a registration
 * error from the control server. */
static bool s_health_from_worker;
static void worker(void *arg);

static bool start_worker(void)
{
    /* Real-tailnet enrollment, NVS commit and OTA recovery use 2284 bytes at
     * peak. Retain an internal flash-safe stack with over 1 KiB of margin. */
    return s_task || xTaskCreateWithCaps(worker, "jk_tailscale", 4096, NULL, 3, &s_task,
        MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT) == pdPASS;
}

static bool login_digest(const jetkvm_tailscale_config_t *config, uint8_t digest[32])
{
    char input[JETKVM_TAILSCALE_CONTROL_URL_CAPACITY + JETKVM_TAILSCALE_AUTH_KEY_CAPACITY + 1];
    int length = snprintf(input, sizeof(input), "%s\n%s", config->control_url, config->auth_key);
    size_t written = 0;
    bool ok = length > 0 && length < sizeof(input) && psa_crypto_init() == PSA_SUCCESS &&
        psa_hash_compute(PSA_ALG_SHA_256, (const uint8_t *)input, length,
                         digest, 32, &written) == PSA_SUCCESS && written == 32;
    memset(input, 0, sizeof(input));
    return ok;
}

static esp_err_t remember_registration(const uint8_t digest[32])
{
    nvs_handle_t handle;
    esp_err_t error = nvs_open("jk_tailscale", NVS_READWRITE, &handle);
    if (error != ESP_OK) return error;
    error = nvs_set_blob(handle, "login_digest", digest, 32);
    if (error == ESP_OK) error = nvs_commit(handle);
    nvs_close(handle);
    if (error == ESP_OK) {
        memcpy(s_registered_digest, digest, 32);
        s_has_registered_digest = true;
    }
    return error;
}

static esp_err_t erase_key(nvs_handle_t handle, const char *key)
{
    esp_err_t error = nvs_erase_key(handle, key);
    return error == ESP_ERR_NVS_NOT_FOUND ? ESP_OK : error;
}

/* With logged_out, also ends the pending log out in the same commit. */
static esp_err_t forget_registration(bool logged_out)
{
    nvs_handle_t handle;
    esp_err_t error = nvs_open("jk_tailscale", NVS_READWRITE, &handle);
    if (error != ESP_OK) return error;
    error = erase_key(handle, "login_digest");
    if (error == ESP_OK && logged_out) error = erase_key(handle, "logout_pending");
    if (error == ESP_OK) error = nvs_commit(handle);
    nvs_close(handle);
    if (error == ESP_OK) s_has_registered_digest = false;
    return error;
}

/* The node's keys and the digest of its enrollment: without them the next
 * start generates new keys and logs in as a new node. */
static esp_err_t erase_identity(void)
{
    esp_err_t error = microlink_factory_reset();
    return error == ESP_OK ? forget_registration(true) : error;
}

static void set_worker_health(const char *text)
{
    snprintf(s_status.health, sizeof(s_status.health), "%s", text);
    s_health_from_worker = true;
}

static void copy_text(char *dst, size_t capacity, const char *src)
{
    snprintf(dst, capacity, "%s", src ? src : "");
}

static esp_err_t persist(const jetkvm_tailscale_config_t *config, bool logout_pending,
                         bool new_login)
{
    nvs_handle_t handle;
    esp_err_t error = nvs_open("jk_tailscale", NVS_READWRITE, &handle);
    if (error != ESP_OK) return error;
    /* A new auth key ends the enrollment digest of the old one. The digest goes
     * before the settings, so that new settings never keep the old digest. */
    bool erased = false;
    if (new_login) {
        error = erase_key(handle, "login_digest");
        erased = error == ESP_OK && s_has_registered_digest;
    }
    /* One blob gives the enabled/key/server update a single commit boundary;
     * a log out commits the pending erasure with it. */
    if (error == ESP_OK) error = nvs_set_blob(handle, "config_v1", config, sizeof(*config));
    /* NVS applies the erasure at once. When the settings write fails, the old
     * settings stay and need their digest back: no client may be running to
     * record it again, and the next start would send their single-use key. */
    if (erased && error != ESP_OK)
        erased = nvs_set_blob(handle, "login_digest", s_registered_digest, 32) != ESP_OK ||
                 nvs_commit(handle) != ESP_OK;
    if (error == ESP_OK && logout_pending) error = nvs_set_u8(handle, "logout_pending", 1);
    if (error == ESP_OK) error = nvs_commit(handle);
    nvs_close(handle);
    if (erased) s_has_registered_digest = false;
    return error;
}

static bool get_status(jetkvm_tailscale_status_t *status, void *ctx)
{
    (void)ctx;
    if (!status || !s_mutex || xSemaphoreTake(s_mutex, pdMS_TO_TICKS(100)) != pdTRUE) return false;
    *status = s_status;
    status->installed = true;
    status->enabled = s_config.enabled;
    status->auth_key_set = s_config.auth_key[0] != '\0';
    status->logged_in = s_has_registered_digest && !s_logout_pending;
    copy_text(status->control_url, sizeof(status->control_url), s_config.control_url);
    xSemaphoreGive(s_mutex);
    return true;
}

static bool get_config(jetkvm_tailscale_config_t *config, void *ctx)
{
    (void)ctx;
    if (!config || !s_mutex || xSemaphoreTake(s_mutex, pdMS_TO_TICKS(100)) != pdTRUE) return false;
    *config = s_config;
    xSemaphoreGive(s_mutex);
    return true;
}

static const char *set_config(const jetkvm_tailscale_config_t *config, void *ctx)
{
    (void)ctx;
    const char *error = jetkvm_tailscale_validate_config(config);
    if (error) return error;
    jetkvm_tailscale_config_t next = *config;
    char host[64];
    bool tls;
    uint16_t port;
    error = jetkvm_tailscale_normalize_url(config->control_url, next.control_url,
        sizeof(next.control_url), host, sizeof(host), &tls, &port);
    if (error) return error;
    if (xSemaphoreTake(s_mutex, pdMS_TO_TICKS(100)) != pdTRUE) return "Tailscale settings busy";
    /* The stored key goes only to the control server it was entered for. */
    bool new_server = strcmp(next.control_url, s_config.control_url) != 0;
    if (new_server && !strcmp(next.auth_key, s_config.auth_key))
        memset(next.auth_key, 0, sizeof(next.auth_key));
    /* A node that needs a login (an expired node key, a refused key) enrolls
     * again with the key it is given, also the stored one. */
    bool enroll_again = next.enabled && next.auth_key[0] &&
        !strcmp(s_status.backend_state, "NeedsLogin");
    if (memcmp(&next, &s_config, sizeof(next)) == 0 && !enroll_again) {
        xSemaphoreGive(s_mutex);
        return NULL;
    }
    if (next.enabled && !start_worker()) {
        xSemaphoreGive(s_mutex);
        return "Not enough memory to enable Tailscale";
    }
    /* A key that the user enters, also the same key again after its removal,
     * enrolls the node anew: the digest of a completed enrollment only keeps a
     * restart or a re-enable from sending a consumed key again. */
    esp_err_t saved = persist(&next, false,
        enroll_again || new_server || strcmp(next.auth_key, s_config.auth_key) != 0);
    if (saved == ESP_OK) {
        s_config = next;
        ++s_generation;
        s_status.running = false;
        s_status.auth_url[0] = s_status.health[0] = '\0';
        s_needs_login = false;
        copy_text(s_status.backend_state, sizeof(s_status.backend_state), next.enabled ? "Starting" : "Stopped");
    }
    if (s_task) xTaskNotifyGive(s_task);
    xSemaphoreGive(s_mutex);
    if (saved != ESP_OK) return "Could not save Tailscale settings";
    return NULL;
}

static const char *logout(void *ctx)
{
    (void)ctx;
    if (!s_mutex || xSemaphoreTake(s_mutex, pdMS_TO_TICKS(100)) != pdTRUE) return "Tailscale settings busy";
    jetkvm_tailscale_config_t next = s_config;
    next.enabled = false;
    memset(next.auth_key, 0, sizeof(next.auth_key));
    esp_err_t error = persist(&next, true, false);
    if (error == ESP_OK) {
        s_config = next;
        ++s_generation;
        s_needs_login = false;
        s_status.running = false;
        s_status.auth_url[0] = s_status.health[0] = '\0';
        copy_text(s_status.backend_state, sizeof(s_status.backend_state), "Stopped");
        if (s_task) {
            /* The worker erases the identity once its client has stopped. */
            s_logout_pending = true;
            xTaskNotifyGive(s_task);
        } else {
            error = erase_identity();
            /* A failed erasure stays pending, as the flag in NVS does: no
             * client starts before the worker or the next start erases it. */
            s_logout_pending = error != ESP_OK;
        }
    }
    xSemaphoreGive(s_mutex);
    return error == ESP_OK ? NULL : "Could not erase the Tailscale identity";
}

static void on_auth(const char *url, const char *error, void *ctx)
{
    (void)ctx;
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    copy_text(s_status.auth_url, sizeof(s_status.auth_url), url);
    s_needs_login = (url && url[0]) || (error && error[0]);
    s_registration_completed = !s_needs_login;
    /* The server may echo a rejected key. Never place server errors in RPC or logs. */
    copy_text(s_status.health, sizeof(s_status.health), error && error[0] ?
        "Tailscale rejected registration. Check the auth key and control server." : "");
    s_health_from_worker = false;
    xSemaphoreGive(s_mutex);
}

static bool network_ready(void)
{
    esp_netif_t *netif = esp_netif_get_default_netif();
    esp_netif_ip_info_t ip = {0};
    return netif && esp_netif_is_netif_up(netif) &&
        esp_netif_get_ip_info(netif, &ip) == ESP_OK && ip.ip.addr;
}

static void worker(void *arg)
{
    (void)arg;
    microlink_t *client = NULL;
    jetkvm_tailscale_config_t active = {0};
    char hostname[64] = "jetkvm", host[64] = "";
    unsigned applied = 0;
    uint8_t active_digest[32] = {0};
    bool digest_valid = false;
    for (;;) {
        ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(1000));
        jetkvm_tailscale_config_t desired;
        xSemaphoreTake(s_mutex, portMAX_DELAY);
        desired = s_config;
        unsigned generation = s_generation;
        xSemaphoreGive(s_mutex);
        if (client && (applied != generation || !desired.enabled)) {
            if (microlink_stop(client) != ESP_OK) {
                xSemaphoreTake(s_mutex, portMAX_DELAY);
                s_status.running = false;
                set_worker_health("Tailscale workers are still stopping.");
                xSemaphoreGive(s_mutex);
                continue;
            }
            microlink_destroy(client);
            client = NULL;
            /* The stop can take seconds: start from the settings as they are
             * now, not from those read before it. */
            xSemaphoreTake(s_mutex, portMAX_DELAY);
            desired = s_config;
            generation = s_generation;
            xSemaphoreGive(s_mutex);
        }
        xSemaphoreTake(s_mutex, portMAX_DELAY);
        bool erase = !client && s_logout_pending;
        xSemaphoreGive(s_mutex);
        if (erase) {
            esp_err_t erased = erase_identity();
            xSemaphoreTake(s_mutex, portMAX_DELAY);
            if (erased == ESP_OK) s_logout_pending = false;
            else set_worker_health("Could not erase the Tailscale identity. Retrying.");
            erase = s_logout_pending;
            xSemaphoreGive(s_mutex);
        }
        if (!client && !erase && desired.enabled && network_ready()) {
            xSemaphoreTake(s_mutex, portMAX_DELAY);
            s_registration_completed = false;
            xSemaphoreGive(s_mutex);
            active = desired; /* These strings must outlive the native session. */
            digest_valid = login_digest(&active, active_digest);
            bool registered = digest_valid && s_has_registered_digest &&
                memcmp(active_digest, s_registered_digest, 32) == 0;
            bool tls;
            uint16_t port;
            char normalized[JETKVM_TAILSCALE_CONTROL_URL_CAPACITY];
            jetkvm_tailscale_normalize_url(active.control_url, normalized, sizeof(normalized),
                                           host, sizeof(host), &tls, &port);
            jetkvm_network_settings_t network = {0};
            if (jetkvm_network_settings_get(&network, NULL)) copy_text(hostname, sizeof(hostname), network.hostname);
            microlink_config_t cfg = {
                /* Auth keys enroll a persistent node. Do not reuse a consumed
                 * single-use key when the same identity reconnects. */
                .auth_key = registered ? "" : active.auth_key, .device_name = hostname,
                /* Only an enrollment without a digest, which a new key or a
                 * log out erases, gets a new node key. Another control server
                 * takes the key with the node key that the node has, so a
                 * change back finds the node as it was. */
                .renew_node_key = !s_has_registered_digest && active.auth_key[0] != '\0',
                .ctrl_host = host, .ctrl_tls = tls, .ctrl_port = port,
                .enable_derp = true, .enable_stun = true, .enable_disco = true,
                /* A direct path is trusted for 15 s after a pong
                 * (ML_DISCO_TRUST_DURATION_MS): a ping every 5 s renews it
                 * with room for two lost pongs. */
                .disco_heartbeat_ms = 5000, .stun_interval_ms = 60000,
                .auth_callback = on_auth,
            };
            client = microlink_init(&cfg);
            applied = generation;
            if (client && microlink_start(client) != ESP_OK) {
                if (microlink_stop(client) == ESP_OK) {
                    microlink_destroy(client);
                    client = NULL;
                } else {
                    /* No generation is 0: the next pass stops the client again. */
                    applied = 0;
                }
            }
        }
        xSemaphoreTake(s_mutex, portMAX_DELAY);
        /* Registration consumes a single-use key before the peer map arrives.
         * Remember it even when subsequent map or relay setup must retry, but
         * only for the settings that the client was started with: a
         * set_config since then may have erased the digest to enroll again. */
        if (client && applied == s_generation && s_registration_completed && digest_valid &&
            (!s_has_registered_digest || memcmp(active_digest, s_registered_digest, 32))) {
            if (remember_registration(active_digest) != ESP_OK)
                ESP_LOGE(TAG, "Could not persist the completed registration");
        }
        /* An RPC may have queued a new configuration during slow network work. */
        if (generation == s_generation) {
            s_status.running = client && microlink_is_connected(client) &&
                microlink_get_vpn_ip(client) && !microlink_key_expired(client);
            const char *state = !desired.enabled ? "Stopped" : s_needs_login ? "NeedsLogin" :
                client && microlink_key_expired(client) ? "NeedsLogin" : s_status.running ? "Running" :
                client && microlink_get_state(client) == ML_STATE_ERROR ? "Error" : "Starting";
            copy_text(s_status.backend_state, sizeof(s_status.backend_state), state);
            if (!desired.enabled) s_status.auth_url[0] = s_status.health[0] = '\0';
            unsigned left_out = client ? microlink_peers_left_out(client) : 0;
            if (desired.enabled && !network_ready())
                set_worker_health("Waiting for a network address.");
            else if (desired.enabled && !client)
                set_worker_health("Could not start Tailscale. Retrying.");
            else if (left_out) {
                char health[sizeof(s_status.health)];
                snprintf(health, sizeof(health), "Your tailnet has more than 64 devices. Tailscale on "
                    "this KVM connects to 64 of them, the online and most recently seen first, so %u "
                    "%s cannot reach it. Remove unused devices in the Tailscale admin console.",
                    left_out, left_out == 1 ? "device" : "devices");
                set_worker_health(health);
            } else if (s_status.running || s_health_from_worker)
                s_status.health[0] = '\0';
            s_status.ipv4[0] = s_status.dns_name[0] = '\0';
            if (client && microlink_get_vpn_ip(client)) {
                microlink_ip_to_str(microlink_get_vpn_ip(client), s_status.ipv4);
                copy_text(s_status.dns_name, sizeof(s_status.dns_name), microlink_get_self_name(client));
            }
            copy_text(s_status.hostname, sizeof(s_status.hostname), hostname);
        }
        bool exit = !s_config.enabled && !client && !s_logout_pending;
        if (exit) {
            /* A control-URL change can supersede the generation while stop
             * joins the workers. Publish the final stopped cache even when
             * that generation's status update was skipped above. */
            s_status.running = false;
            s_status.ipv4[0] = s_status.dns_name[0] = '\0';
            s_status.auth_url[0] = s_status.health[0] = '\0';
            copy_text(s_status.backend_state, sizeof(s_status.backend_state), "Stopped");
            s_task = NULL;
        }
        xSemaphoreGive(s_mutex);
        if (exit) {
            vTaskDeleteWithCaps(NULL);
            return;
        }
    }
}

esp_err_t jetkvm_tailscale_init(void)
{
    if (s_mutex) return ESP_OK;
    s_mutex = xSemaphoreCreateMutex();
    if (!s_mutex) return ESP_ERR_NO_MEM;
    copy_text(s_config.control_url, sizeof(s_config.control_url), JETKVM_TAILSCALE_DEFAULT_CONTROL_URL);
    copy_text(s_status.backend_state, sizeof(s_status.backend_state), "Stopped");
    nvs_handle_t handle;
    esp_err_t loaded = nvs_open("jk_tailscale", NVS_READONLY, &handle);
    if (loaded == ESP_OK) {
        jetkvm_tailscale_config_t saved = {0};
        size_t length = sizeof(saved);
        loaded = nvs_get_blob(handle, "config_v1", &saved, &length);
        size_t digest_length = sizeof(s_registered_digest);
        s_has_registered_digest = nvs_get_blob(handle, "login_digest", s_registered_digest,
            &digest_length) == ESP_OK && digest_length == sizeof(s_registered_digest);
        uint8_t logout_pending = 0;
        s_logout_pending = nvs_get_u8(handle, "logout_pending", &logout_pending) == ESP_OK &&
            logout_pending;
        nvs_close(handle);
        if (loaded == ESP_OK && length == sizeof(saved) && !jetkvm_tailscale_validate_config(&saved)) s_config = saved;
        else if (loaded != ESP_ERR_NVS_NOT_FOUND) ESP_LOGW(TAG, "Invalid saved settings; Tailscale stays disabled");
    } else if (loaded != ESP_ERR_NVS_NOT_FOUND) {
        vSemaphoreDelete(s_mutex);
        s_mutex = NULL;
        return loaded;
    }
    s_generation = 1;
    /* A log out that a restart interrupted: no client runs yet, so the
     * identity can go now. On failure the worker retries before any start. */
    if (s_logout_pending && erase_identity() == ESP_OK) s_logout_pending = false;
    /* Key generation commits NVS from this task. Flash writes require an
     * internal stack while caches are disabled on ESP32-P4. */
    if (s_config.enabled && !start_worker()) {
        copy_text(s_status.backend_state, sizeof(s_status.backend_state), "Error");
        copy_text(s_status.health, sizeof(s_status.health), "Not enough memory to start Tailscale.");
        /* Keep the settings provider available so the user can disable it. */
    }
    return ESP_OK;
}

void jetkvm_tailscale_fill_control_context(jetkvm_tailscale_context_t *context)
{
    if (context) *context = (jetkvm_tailscale_context_t){
        .get_status = get_status, .get_config = get_config, .set_config = set_config,
        .logout = logout,
    };
}
