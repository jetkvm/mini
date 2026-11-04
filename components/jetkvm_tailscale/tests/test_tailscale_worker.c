/* The settings and one start of the worker of tailscale.c, with the platform
 * stubbed: which auth key a start sends and whether it makes a new node key. */
#include "../tailscale.c"
#include <assert.h>
#include <stdlib.h>

static struct {
    bool has_config, has_digest;
    jetkvm_tailscale_config_t config;
    uint8_t digest[32];
    uint8_t logout_pending;
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
    bool config = !strcmp(key, "config_v1");
    memcpy(config ? (void *)&nvs.config : nvs.digest, value, length);
    *(config ? &nvs.has_config : &nvs.has_digest) = true;
    return ESP_OK;
}
esp_err_t nvs_get_blob(nvs_handle_t handle, const char *key, void *value, size_t *length)
{
    (void)handle;
    bool config = !strcmp(key, "config_v1");
    if (!(config ? nvs.has_config : nvs.has_digest)) return ESP_ERR_NVS_NOT_FOUND;
    *length = config ? sizeof(nvs.config) : sizeof(nvs.digest);
    memcpy(value, config ? (const void *)&nvs.config : nvs.digest, *length);
    return ESP_OK;
}
esp_err_t nvs_set_u8(nvs_handle_t handle, const char *key, uint8_t value)
{
    (void)handle;
    assert(!strcmp(key, "logout_pending"));
    nvs.logout_pending = value;
    return ESP_OK;
}
esp_err_t nvs_get_u8(nvs_handle_t handle, const char *key, uint8_t *value)
{
    (void)handle;
    assert(!strcmp(key, "logout_pending"));
    if (!nvs.logout_pending) return ESP_ERR_NVS_NOT_FOUND;
    *value = nvs.logout_pending;
    return ESP_OK;
}
esp_err_t nvs_erase_key(nvs_handle_t handle, const char *key)
{
    (void)handle;
    if (!strcmp(key, "login_digest") && nvs.has_digest) {
        nvs.has_digest = false;
        return ESP_OK;
    }
    if (!strcmp(key, "logout_pending") && nvs.logout_pending) {
        nvs.logout_pending = 0;
        return ESP_OK;
    }
    return ESP_ERR_NVS_NOT_FOUND;
}

/* One thread. The worker runs when the test calls it: its first pass starts
 * a client, and its second finds Tailscale disabled, stops the client and
 * ends. */
static int mutex, task, passes;
/* The client's live peers without a slot and the health that the first
 * pass published. */
static unsigned left_out;
static char first_health[sizeof(s_status.health)];
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
uint32_t ulTaskNotifyTake(BaseType_t c, TickType_t t)
{
    (void)c; (void)t;
    if (++passes == 2) {
        snprintf(first_health, sizeof(first_health), "%s", s_status.health);
        s_config.enabled = false;
    }
    return 0;
}
void vTaskDeleteWithCaps(TaskHandle_t t) { (void)t; }

/* A digest that differs for each input. */
psa_status_t psa_crypto_init(void) { return PSA_SUCCESS; }
psa_status_t psa_hash_compute(psa_algorithm_t a, const uint8_t *in, size_t length, uint8_t *hash,
                              size_t size, size_t *written)
{
    (void)a;
    memset(hash, 0, size);
    for (size_t i = 0; i < length; ++i) hash[i % size] = (uint8_t)(hash[i % size] * 31 + in[i]);
    *written = size;
    return PSA_SUCCESS;
}

static int netif;
esp_netif_t *esp_netif_get_default_netif(void) { return (esp_netif_t *)&netif; }
bool esp_netif_is_netif_up(esp_netif_t *n) { (void)n; return true; }
esp_err_t esp_netif_get_ip_info(esp_netif_t *n, esp_netif_ip_info_t *i) { (void)n; i->ip.addr = 1; return ESP_OK; }
bool jetkvm_network_settings_get(jetkvm_network_settings_t *s, void *c) { (void)s; (void)c; return false; }

/* What the started client was given. */
static int client;
static char sent_key[JETKVM_TAILSCALE_AUTH_KEY_CAPACITY];
static bool renewed;
/* Only a log out erases the node identity: the machine key, without which
 * the next registration is a new device on the tailnet. */
static unsigned identity_erasures;
esp_err_t microlink_factory_reset(void) { ++identity_erasures; return ESP_OK; }
microlink_t *microlink_init(const microlink_config_t *c)
{
    snprintf(sent_key, sizeof(sent_key), "%s", c->auth_key);
    renewed = c->renew_node_key;
    return (microlink_t *)&client;
}
/* What another task does while the worker starts the client. */
static void (*during_start)(void);
esp_err_t microlink_start(microlink_t *m)
{
    (void)m;
    if (during_start) during_start();
    return ESP_OK;
}
esp_err_t microlink_stop(microlink_t *m) { (void)m; return ESP_OK; }
void microlink_destroy(microlink_t *m) { (void)m; }
microlink_state_t microlink_get_state(const microlink_t *m) { (void)m; return ML_STATE_IDLE; }
bool microlink_is_connected(const microlink_t *m) { (void)m; return false; }
uint32_t microlink_get_vpn_ip(const microlink_t *m) { (void)m; return 0; }
const char *microlink_get_self_name(const microlink_t *m) { (void)m; return ""; }
bool microlink_key_expired(const microlink_t *m) { (void)m; return false; }
unsigned microlink_peers_left_out(const microlink_t *m) { (void)m; return left_out; }
void microlink_ip_to_str(uint32_t ip, char *buf) { (void)ip; buf[0] = '\0'; }

static jetkvm_tailscale_context_t tailscale;
static const jetkvm_tailscale_config_t enrolled = {
    .enabled = true, .control_url = JETKVM_TAILSCALE_DEFAULT_CONTROL_URL,
    .auth_key = "tskey-auth-stored-test-key",
};

/* The client registers, its node key expires, and the user enables Tailscale
 * again before the worker remembers the registration. */
static void registered_then_enroll_again(void)
{
    on_auth("", "", NULL);
    snprintf(s_status.backend_state, sizeof(s_status.backend_state), "NeedsLogin");
    assert(!tailscale.set_config(&enrolled, NULL));
}

/* The control server takes the registration. */
static void registered(void) { on_auth("", "", NULL); }

/* A restart, also into an update: the module reads its settings from NVS. */
static void restart(void)
{
    s_mutex = NULL;
    s_task = NULL;
    memset(&s_config, 0, sizeof(s_config));
    memset(&s_status, 0, sizeof(s_status));
    s_has_registered_digest = s_needs_login = s_registration_completed = s_logout_pending = false;
    assert(jetkvm_tailscale_init() == ESP_OK);
    assert(s_has_registered_digest == nvs.has_digest);
}

/* Applies the settings, as setTailscaleConfig does, and runs one start of the
 * worker with them. */
static void start(const jetkvm_tailscale_config_t *config)
{
    assert(!tailscale.set_config(config, NULL));
    passes = 0;
    sent_key[0] = '\0';
    worker(NULL);
    assert(passes == 2);
    s_config.enabled = config->enabled;
}

int main(void)
{
    nvs.config = enrolled;
    nvs.has_config = nvs.has_digest = true;
    assert(login_digest(&enrolled, nvs.digest));
    assert(jetkvm_tailscale_init() == ESP_OK);
    jetkvm_tailscale_fill_control_context(&tailscale);

    /* Another control server gets neither the stored key nor the digest of
     * its enrollment. A key entered again for the first server enrolls anew. */
    jetkvm_tailscale_config_t other = enrolled;
    snprintf(other.control_url, sizeof(other.control_url), "https://headscale.example");
    start(&other);
    assert(!strcmp(sent_key, "") && !renewed);
    assert(!nvs.has_digest && !s_config.auth_key[0]);
    start(&enrolled);
    assert(!strcmp(sent_key, enrolled.auth_key) && renewed);

    /* A node that needs a login takes the stored key again: it enrolls anew. */
    snprintf(s_status.backend_state, sizeof(s_status.backend_state), "NeedsLogin");
    start(&enrolled);
    assert(!nvs.has_digest);
    assert(!strcmp(sent_key, enrolled.auth_key) && renewed);

    /* The same settings again without a login to wait for change nothing. */
    nvs.has_digest = s_has_registered_digest = true;
    snprintf(s_status.backend_state, sizeof(s_status.backend_state), "Running");
    unsigned generation = s_generation;
    assert(!tailscale.set_config(&enrolled, NULL));
    assert(s_generation == generation && nvs.has_digest);

    /* The registration of settings that a new enrollment replaced while the
     * client started is not remembered: the erased digest stays erased, and
     * the next start sends the stored key with a new node key. */
    during_start = registered_then_enroll_again;
    start(&enrolled);
    during_start = NULL;
    assert(!nvs.has_digest && !s_has_registered_digest);
    snprintf(s_status.backend_state, sizeof(s_status.backend_state), "Starting");
    start(&enrolled);
    assert(!strcmp(sent_key, enrolled.auth_key) && renewed);
    assert(!strstr(first_health, "more than 64 devices"));

    /* A tailnet with more live peers than the client has slots for: the
     * status says how many devices cannot reach the KVM, only while some
     * cannot. */
    left_out = 17;
    snprintf(s_status.backend_state, sizeof(s_status.backend_state), "Starting");
    start(&enrolled);
    assert(strstr(first_health, "more than 64 devices"));
    assert(strstr(first_health, "so 17 devices cannot reach it."));
    assert(strstr(first_health, "Remove unused devices in the Tailscale admin console."));
    left_out = 1;
    start(&enrolled);
    assert(strstr(first_health, "so 1 device cannot reach it."));
    left_out = 0;
    start(&enrolled);
    assert(!strstr(first_health, "more than 64 devices"));

    /* None of these erases the identity: a join, Tailscale off and on with
     * the key kept, a restart or an update, and off with the key removed and
     * on with it again. */
    during_start = registered;
    snprintf(s_status.backend_state, sizeof(s_status.backend_state), "Starting");
    start(&enrolled);
    assert(nvs.has_digest);
    jetkvm_tailscale_config_t off = enrolled;
    off.enabled = false;
    assert(!tailscale.set_config(&off, NULL));
    start(&enrolled);
    assert(!strcmp(sent_key, "") && !renewed);
    restart();
    start(&enrolled);
    assert(!strcmp(sent_key, "") && !renewed);
    jetkvm_tailscale_config_t removed = off;
    removed.auth_key[0] = '\0';
    assert(!tailscale.set_config(&removed, NULL));
    assert(!nvs.has_digest);
    start(&enrolled);
    /* The key again enrolls with a new node key and the same machine key:
     * the control server keeps the device and its address. */
    assert(!strcmp(sent_key, enrolled.auth_key) && renewed);
    assert(nvs.has_digest);
    restart();
    start(&enrolled);
    assert(!strcmp(sent_key, "") && !renewed);
    during_start = NULL;
    assert(identity_erasures == 0);

    /* A log out erases it, and the next join is a new device. */
    assert(!tailscale.logout(NULL));
    assert(identity_erasures == 1 && !nvs.has_digest && !nvs.logout_pending);
    start(&enrolled);
    assert(!strcmp(sent_key, enrolled.auth_key) && renewed);

    /* A log out while the worker runs leaves the erasure to the worker. A
     * restart before the worker gets to it erases the identity at the next
     * start, before any client runs with the old machine key. */
    during_start = registered;
    start(&enrolled);
    during_start = NULL;
    assert(nvs.has_digest);
    s_task = (TaskHandle_t)&task;
    assert(!tailscale.logout(NULL));
    assert(identity_erasures == 1 && nvs.logout_pending == 1 && s_logout_pending);
    restart();
    assert(identity_erasures == 2 && !nvs.logout_pending && !nvs.has_digest);
    assert(!s_logout_pending && !s_config.enabled);
    start(&enrolled);
    assert(!strcmp(sent_key, enrolled.auth_key) && renewed);
    puts("tailscale worker host test passed");
    return 0;
}
