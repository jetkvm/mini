/* set_config in tailscale.c against an NVS whose writes take effect at once,
 * as ESP-IDF's do, with one write made to fail: NVS and the settings that the
 * device reports must agree after every result. */
#include "jetkvm_tailscale.h"
#include "esp_netif.h"
#include "freertos/idf_additions.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "jetkvm_network_settings.h"
#include "microlink.h"
#include "nvs.h"
#include "psa/crypto.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static struct {
    bool has_config, has_digest;
    jetkvm_tailscale_config_t config;
    uint8_t digest[32];
    uint8_t logout_pending;
    const char *fail_key; /* The set or erase of this key fails. */
} nvs;

esp_err_t nvs_open(const char *name, nvs_open_mode_t mode, nvs_handle_t *handle)
{
    (void)mode;
    assert(!strcmp(name, "jk_tailscale"));
    *handle = 1;
    return ESP_OK;
}
void nvs_close(nvs_handle_t handle) { (void)handle; }
esp_err_t nvs_commit(nvs_handle_t handle) { (void)handle; return ESP_OK; }

esp_err_t nvs_set_blob(nvs_handle_t handle, const char *key, const void *value, size_t length)
{
    (void)handle;
    if (nvs.fail_key && !strcmp(key, nvs.fail_key)) return ESP_FAIL;
    if (!strcmp(key, "config_v1")) {
        assert(length == sizeof(nvs.config));
        memcpy(&nvs.config, value, length);
        nvs.has_config = true;
    } else {
        assert(!strcmp(key, "login_digest") && length == sizeof(nvs.digest));
        memcpy(nvs.digest, value, length);
        nvs.has_digest = true;
    }
    return ESP_OK;
}

esp_err_t nvs_get_blob(nvs_handle_t handle, const char *key, void *value, size_t *length)
{
    (void)handle;
    bool config = !strcmp(key, "config_v1");
    if (!(config ? nvs.has_config : nvs.has_digest)) return ESP_ERR_NVS_NOT_FOUND;
    size_t size = config ? sizeof(nvs.config) : sizeof(nvs.digest);
    assert(*length >= size);
    memcpy(value, config ? (const void *)&nvs.config : nvs.digest, size);
    *length = size;
    return ESP_OK;
}

esp_err_t nvs_set_u8(nvs_handle_t handle, const char *key, uint8_t value)
{
    (void)handle;
    if (nvs.fail_key && !strcmp(key, nvs.fail_key)) return ESP_FAIL;
    nvs.logout_pending = value;
    return ESP_OK;
}

esp_err_t nvs_get_u8(nvs_handle_t handle, const char *key, uint8_t *value)
{
    (void)handle;
    (void)key;
    if (!nvs.logout_pending) return ESP_ERR_NVS_NOT_FOUND;
    *value = nvs.logout_pending;
    return ESP_OK;
}

esp_err_t nvs_erase_key(nvs_handle_t handle, const char *key)
{
    (void)handle;
    if (nvs.fail_key && !strcmp(key, nvs.fail_key)) return ESP_FAIL;
    bool *present = !strcmp(key, "login_digest") ? &nvs.has_digest :
        !strcmp(key, "config_v1") ? &nvs.has_config : NULL;
    if (!present) {
        if (!nvs.logout_pending) return ESP_ERR_NVS_NOT_FOUND;
        nvs.logout_pending = 0;
        return ESP_OK;
    }
    if (!*present) return ESP_ERR_NVS_NOT_FOUND;
    *present = false;
    return ESP_OK;
}

/* One thread and no worker task: the mutex always succeeds, and the worker
 * that set_config starts is never scheduled. */
static int mutex, task;
SemaphoreHandle_t xSemaphoreCreateMutex(void) { return (SemaphoreHandle_t)&mutex; }
BaseType_t xSemaphoreTake(SemaphoreHandle_t s, TickType_t t) { (void)s; (void)t; return pdTRUE; }
BaseType_t xSemaphoreGive(SemaphoreHandle_t s) { (void)s; return pdTRUE; }
void vSemaphoreDelete(SemaphoreHandle_t s) { (void)s; }
BaseType_t xTaskNotifyGive(TaskHandle_t t) { (void)t; return pdPASS; }
BaseType_t xTaskCreateWithCaps(void (*fn)(void *), const char *name, uint32_t stack, void *arg,
                               int priority, TaskHandle_t *handle, uint32_t caps)
{
    (void)fn; (void)name; (void)stack; (void)arg; (void)priority; (void)caps;
    *handle = (TaskHandle_t)&task;
    return pdPASS;
}

/* Reached only from the worker. */
uint32_t ulTaskNotifyTake(BaseType_t c, TickType_t t) { (void)c; (void)t; abort(); }
void vTaskDeleteWithCaps(TaskHandle_t t) { (void)t; abort(); }
psa_status_t psa_crypto_init(void) { abort(); }
psa_status_t psa_hash_compute(psa_algorithm_t a, const uint8_t *i, size_t il, uint8_t *h, size_t hs,
                              size_t *hl) { (void)a; (void)i; (void)il; (void)h; (void)hs; (void)hl; abort(); }
esp_netif_t *esp_netif_get_default_netif(void) { abort(); }
bool esp_netif_is_netif_up(esp_netif_t *n) { (void)n; abort(); }
esp_err_t esp_netif_get_ip_info(esp_netif_t *n, esp_netif_ip_info_t *i) { (void)n; (void)i; abort(); }
bool jetkvm_network_settings_get(jetkvm_network_settings_t *s, void *c) { (void)s; (void)c; abort(); }
esp_err_t microlink_factory_reset(void) { abort(); }
microlink_t *microlink_init(const microlink_config_t *c) { (void)c; abort(); }
esp_err_t microlink_start(microlink_t *m) { (void)m; abort(); }
esp_err_t microlink_stop(microlink_t *m) { (void)m; abort(); }
void microlink_destroy(microlink_t *m) { (void)m; abort(); }
microlink_state_t microlink_get_state(const microlink_t *m) { (void)m; abort(); }
bool microlink_is_connected(const microlink_t *m) { (void)m; abort(); }
uint32_t microlink_get_vpn_ip(const microlink_t *m) { (void)m; abort(); }
const char *microlink_get_self_name(const microlink_t *m) { (void)m; abort(); }
bool microlink_key_expired(const microlink_t *m) { (void)m; abort(); }
unsigned microlink_peers_left_out(const microlink_t *m) { (void)m; abort(); }
void microlink_ip_to_str(uint32_t ip, char *buf) { (void)ip; (void)buf; abort(); }

static jetkvm_tailscale_context_t tailscale;

static void expect_reported(const jetkvm_tailscale_config_t *expected)
{
    jetkvm_tailscale_config_t reported;
    jetkvm_tailscale_status_t status;
    assert(tailscale.get_config(&reported, NULL) && tailscale.get_status(&status, NULL));
    assert(!memcmp(&reported, expected, sizeof(reported)));
    /* The digest decides whether a restart sends the key again. */
    assert(status.logged_in == nvs.has_digest);
}

int main(void)
{
    const jetkvm_tailscale_config_t old = {
        .enabled = true, .control_url = JETKVM_TAILSCALE_DEFAULT_CONTROL_URL,
        .auth_key = "tskey-auth-old-test-key",
    };
    jetkvm_tailscale_config_t next = old;
    snprintf(next.auth_key, sizeof(next.auth_key), "tskey-auth-new-test-key");
    nvs.config = old;
    nvs.has_config = nvs.has_digest = true;
    memset(nvs.digest, 0xa5, sizeof(nvs.digest));
    assert(jetkvm_tailscale_init() == ESP_OK);
    jetkvm_tailscale_fill_control_context(&tailscale);
    expect_reported(&old);

    /* A new key whose digest erasure fails leaves the old settings in NVS
     * and in the device, so a restart applies what the caller was told. */
    nvs.fail_key = "login_digest";
    assert(tailscale.set_config(&next, NULL));
    assert(!memcmp(&nvs.config, &old, sizeof(old)) && nvs.has_digest);
    expect_reported(&old);

    /* The settings write fails after the digest is gone: the old settings
     * stay with the digest of their enrollment, so that the next start does
     * not send their single-use key again. */
    const uint8_t enrolled[32] = { [0 ... 31] = 0xa5 };
    nvs.fail_key = "config_v1";
    assert(tailscale.set_config(&next, NULL));
    assert(!memcmp(&nvs.config, &old, sizeof(old)) && nvs.has_digest &&
           !memcmp(nvs.digest, enrolled, sizeof(enrolled)));
    expect_reported(&old);

    nvs.fail_key = NULL;
    nvs.has_digest = true;
    assert(!tailscale.set_config(&next, NULL));
    assert(!memcmp(&nvs.config, &next, sizeof(next)) && !nvs.has_digest);
    expect_reported(&next);

    /* The stored key belongs to the server it was entered for: a new control
     * server, as setTailscaleControlURL sets it, gets neither the key nor the
     * digest of its enrollment. A key given with the new server stays. */
    nvs.has_digest = true;
    jetkvm_tailscale_config_t moved = next;
    snprintf(moved.control_url, sizeof(moved.control_url), "https://headscale.example.com");
    assert(!tailscale.set_config(&moved, NULL));
    memset(moved.auth_key, 0, sizeof(moved.auth_key));
    assert(!memcmp(&nvs.config, &moved, sizeof(moved)) && !nvs.has_digest);
    expect_reported(&moved);
    jetkvm_tailscale_config_t with_key = next;
    snprintf(with_key.control_url, sizeof(with_key.control_url), "https://other.example.com");
    assert(!tailscale.set_config(&with_key, NULL));
    assert(!memcmp(&nvs.config, &with_key, sizeof(with_key)));
    expect_reported(&with_key);
    puts("tailscale settings host test passed");
    return 0;
}
