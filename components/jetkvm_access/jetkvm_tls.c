#define MBEDTLS_ALLOW_PRIVATE_ACCESS
#include "jetkvm_tls.h"
#include "jetkvm_control.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_random.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "freertos/idf_additions.h"
#include "mbedtls/pk.h"
#include "mbedtls/x509_crt.h"
#include "mbedtls/oid.h"
#include "pk_internal.h"
#include "nvs.h"
#include "psa/crypto.h"

#define TLS_MAGIC 0x314c544aU
static const char *TAG = "local_tls";
static SemaphoreHandle_t s_mutex;
static jetkvm_tls_config_t *s_config;
static jetkvm_tls_config_t *s_previous;
static bool s_pending;
static bool s_stopping;
static esp_err_t (*s_apply)(const jetkvm_tls_config_t *);

static void destroy(jetkvm_tls_config_t *config)
{
    if (!config) return;
    volatile unsigned char *bytes = (void *)config;
    for (size_t i = 0; i < sizeof(*config); ++i) bytes[i] = 0;
    heap_caps_free(config);
}
static jetkvm_tls_config_t *allocate_config(void)
{
    return heap_caps_calloc(1, sizeof(jetkvm_tls_config_t), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
}
static bool save(const jetkvm_tls_config_t *config)
{
    nvs_handle_t handle;
    if (nvs_open("jk_access", NVS_READWRITE, &handle) != ESP_OK) return false;
    esp_err_t err = nvs_set_blob(handle, "tls", config, sizeof(*config));
    if (err == ESP_OK) err = nvs_commit(handle);
    nvs_close(handle);
    return err == ESP_OK;
}
static bool valid_pair(const jetkvm_tls_config_t *config)
{
    mbedtls_x509_crt chain;
    mbedtls_pk_context key;
    mbedtls_x509_crt_init(&chain);
    mbedtls_pk_init(&key);
    const char *stage = "certificate parse";
    int err = mbedtls_x509_crt_parse(&chain, (const unsigned char *)config->certificate, strlen(config->certificate) + 1);
    if (!err) {
        stage = "private key parse";
        err = mbedtls_pk_parse_key(&key, (const unsigned char *)config->private_key, strlen(config->private_key) + 1, NULL, 0);
    }
    /* IDF 6.0.2's RSA PKCS#8 parser omits this public-key cache population.
     * Use the pinned library's exporter, then retain the full pair check. */
    if (!err) { stage = "public key export"; err = mbedtls_pk_set_pubkey_from_prv(&key); }
    if (!err) { stage = "key pair check"; err = mbedtls_pk_check_pair(&chain.pk, &key); }
    if (err) ESP_LOGW(TAG, "TLS %s failed: -0x%x", stage, -err);
    mbedtls_pk_free(&key);
    mbedtls_x509_crt_free(&chain);
    return err == 0;
}
static bool self_signed(jetkvm_tls_config_t *config)
{
    /* Include both the stable device name and the current local IPv4 address.
     * A self-signed certificate still requires the browser's trust decision. */
    time_t now = time(NULL);
    if (now < 1704067200) return false;
    char before[16], after[16];
    struct tm tm;
    time_t start = now - 86400, end = now + (time_t)3650 * 86400;
    gmtime_r(&start, &tm); strftime(before, sizeof(before), "%Y%m%d%H%M%S", &tm);
    gmtime_r(&end, &tm); strftime(after, sizeof(after), "%Y%m%d%H%M%S", &tm);
    psa_key_attributes_t attributes = PSA_KEY_ATTRIBUTES_INIT;
    psa_set_key_type(&attributes, PSA_KEY_TYPE_ECC_KEY_PAIR(PSA_ECC_FAMILY_SECP_R1));
    psa_set_key_bits(&attributes, 256);
    psa_set_key_usage_flags(&attributes, PSA_KEY_USAGE_EXPORT | PSA_KEY_USAGE_SIGN_HASH);
    psa_set_key_algorithm(&attributes, PSA_ALG_ECDSA(PSA_ALG_SHA_256));
    mbedtls_svc_key_id_t id = MBEDTLS_SVC_KEY_ID_INIT;
    psa_status_t generated = psa_generate_key(&attributes, &id);
    psa_reset_key_attributes(&attributes);
    if (generated != PSA_SUCCESS) return false;
    mbedtls_pk_context key;
    mbedtls_pk_init(&key);
    int err = mbedtls_pk_copy_from_psa(id, &key);
    psa_destroy_key(id);
    mbedtls_x509write_cert cert;
    mbedtls_x509write_crt_init(&cert);
    uint8_t serial[16]; esp_fill_random(serial, sizeof(serial)); serial[0] &= 0x7f; serial[0] |= 1;
    mbedtls_x509write_crt_set_version(&cert, MBEDTLS_X509_CRT_VERSION_3);
    mbedtls_x509write_crt_set_md_alg(&cert, MBEDTLS_MD_SHA256);
    mbedtls_x509write_crt_set_subject_key(&cert, &key);
    mbedtls_x509write_crt_set_issuer_key(&cert, &key);
    if (!err) err = mbedtls_x509write_crt_set_serial_raw(&cert, serial, sizeof(serial));
    if (!err) err = mbedtls_x509write_crt_set_subject_name(&cert, "CN=jetkvm.local,O=JetKVM");
    if (!err) err = mbedtls_x509write_crt_set_issuer_name(&cert, "CN=jetkvm.local,O=JetKVM");
    if (!err) err = mbedtls_x509write_crt_set_validity(&cert, before, after);
    if (!err) err = mbedtls_x509write_crt_set_basic_constraints(&cert, 0, -1);
    if (!err) err = mbedtls_x509write_crt_set_key_usage(&cert, MBEDTLS_X509_KU_DIGITAL_SIGNATURE);
    uint8_t san[32] = {0x30, 14, 0x82, 12, 'j','e','t','k','v','m','.','l','o','c','a','l'};
    size_t san_size = 16;
    esp_netif_t *netif = esp_netif_get_handle_from_ifkey("ETH_DEF");
    if (!netif) netif = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
    esp_netif_ip_info_t ip;
    if (netif && esp_netif_get_ip_info(netif, &ip) == ESP_OK && ip.ip.addr) {
        san[san_size++] = 0x87; san[san_size++] = 4;
        memcpy(san + san_size, &ip.ip.addr, 4); san_size += 4;
        san[1] = san_size - 2;
    }
    if (!err) err = mbedtls_x509write_crt_set_extension(&cert, MBEDTLS_OID_SUBJECT_ALT_NAME,
        MBEDTLS_OID_SIZE(MBEDTLS_OID_SUBJECT_ALT_NAME), 0, san, san_size);
    if (!err) err = mbedtls_pk_write_key_pem(&key, (unsigned char *)config->private_key, sizeof(config->private_key));
    if (!err) err = mbedtls_x509write_crt_pem(&cert, (unsigned char *)config->certificate, sizeof(config->certificate));
    mbedtls_x509write_crt_free(&cert);
    mbedtls_pk_free(&key);
    return err == 0;
}
static bool get_state(char *mode, size_t mode_size, char *certificate, size_t size, void *ctx)
{
    (void)ctx;
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    bool fits = strlen(s_config->mode) < mode_size && strlen(s_config->certificate) < size;
    if (fits) { strcpy(mode, s_config->mode); strcpy(certificate, s_config->certificate); }
    xSemaphoreGive(s_mutex);
    return fits;
}
static void apply_worker(void *arg)
{
    (void)arg;
    ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
    vTaskDelay(pdMS_TO_TICKS(1000)); /* Allow the setting RPC reply to leave. */
    esp_err_t err = s_apply(s_config);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "TLS listener change failed: %s; restoring previous configuration", esp_err_to_name(err));
        bool restored = save(s_previous);
        esp_err_t applied = s_apply(s_previous);
        xSemaphoreTake(s_mutex, portMAX_DELAY);
        jetkvm_tls_config_t *failed = s_config;
        s_config = s_previous;
        s_previous = failed;
        xSemaphoreGive(s_mutex);
        if (!restored || applied != ESP_OK) ESP_LOGE(TAG, "TLS rollback failed (storage=%d listener=%s)", restored, esp_err_to_name(applied));
    }
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    destroy(s_previous); s_previous = NULL;
    s_pending = false;
    xSemaphoreGive(s_mutex);
    vTaskDeleteWithCaps(NULL);
}
static bool set_state(const char *mode, const char *certificate, const char *key,
                       char *error, size_t capacity, void *ctx)
{
    (void)ctx;
    const char *failure = NULL;
    if (strcmp(mode, "disabled") && strcmp(mode, "self-signed") && strcmp(mode, "custom")) failure = "Invalid TLS mode";
    if (strlen(certificate) > JETKVM_TLS_CERT_MAX || strlen(key) > JETKVM_TLS_KEY_MAX) failure = "Certificate or key too large";
    if (failure) { snprintf(error, capacity, "%s", failure); return false; }
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    if (s_pending || s_stopping) { xSemaphoreGive(s_mutex); snprintf(error, capacity, "TLS service busy"); return false; }
    jetkvm_tls_config_t *next = allocate_config();
    if (!next) failure = "TLS memory unavailable";
    else {
        next->magic = TLS_MAGIC;
        strcpy(next->mode, mode);
        if (!strcmp(mode, "self-signed")) {
            if (!self_signed(next)) failure = "Certificate generation failed; check device time";
        } else if (!strcmp(mode, "custom")) {
            strcpy(next->certificate, certificate); strcpy(next->private_key, key);
            if (!valid_pair(next)) failure = "Invalid certificate or private key pair";
        }
    }
    TaskHandle_t worker = NULL;
    if (!failure && xTaskCreateWithCaps(apply_worker, "local_tls", 8192, NULL, 4, &worker,
        MALLOC_CAP_RTCRAM | MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT) != pdPASS) failure = "TLS worker unavailable";
    if (!failure && !save(next)) failure = "Unable to save TLS configuration";
    if (failure) {
        if (worker) vTaskDeleteWithCaps(worker);
        destroy(next);
    } else {
        s_previous = s_config; s_config = next; s_pending = true;
        xTaskNotifyGive(worker);
    }
    xSemaphoreGive(s_mutex);
    if (failure) snprintf(error, capacity, "%s", failure);
    return !failure;
}
esp_err_t jetkvm_tls_init(esp_err_t (*apply)(const jetkvm_tls_config_t *))
{
    if (s_mutex) return ESP_OK;
    s_config = allocate_config();
    if (!s_config) return ESP_ERR_NO_MEM;
    s_config->magic = TLS_MAGIC; strcpy(s_config->mode, "disabled");
    nvs_handle_t handle;
    esp_err_t err = nvs_open("jk_access", NVS_READONLY, &handle);
    if (err == ESP_OK) {
        size_t size = sizeof(*s_config);
        err = nvs_get_blob(handle, "tls", s_config, &size);
        nvs_close(handle);
        if (err == ESP_ERR_NVS_NOT_FOUND) err = ESP_OK;
        else if (err == ESP_OK && size != sizeof(*s_config)) err = ESP_ERR_INVALID_SIZE;
    } else if (err == ESP_ERR_NVS_NOT_FOUND) err = ESP_OK;
    bool terminated = memchr(s_config->mode, 0, sizeof(s_config->mode)) &&
        memchr(s_config->certificate, 0, sizeof(s_config->certificate)) &&
        memchr(s_config->private_key, 0, sizeof(s_config->private_key));
    bool valid = err == ESP_OK && s_config->magic == TLS_MAGIC && terminated;
    if (valid && strcmp(s_config->mode, "disabled"))
        valid = (!strcmp(s_config->mode, "custom") || !strcmp(s_config->mode, "self-signed")) && valid_pair(s_config);
    if (!valid) {
        /* HTTP remains available to repair TLS, with its own password checks.
         * Retain the stored record until an explicit setting replaces it. */
        ESP_LOGW(TAG, "Stored TLS configuration unavailable or invalid; HTTPS disabled");
        memset(s_config, 0, sizeof(*s_config));
        s_config->magic = TLS_MAGIC; strcpy(s_config->mode, "disabled");
    }
    s_mutex = xSemaphoreCreateMutex();
    if (!s_mutex) { destroy(s_config); s_config = NULL; return ESP_ERR_NO_MEM; }
    s_apply = apply;
    const jetkvm_tls_context_t rpc = {.get = get_state, .set = set_state};
    jetkvm_control_set_default_tls_context(&rpc);
    return ESP_OK;
}
esp_err_t jetkvm_tls_start(void)
{
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    s_stopping = false;
    s_pending = true;
    xSemaphoreGive(s_mutex);
    esp_err_t err = s_apply(s_config);
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    s_pending = false;
    xSemaphoreGive(s_mutex);
    return err;
}
void jetkvm_tls_stop(void)
{
    if (!s_mutex) return;
    for (;;) {
        xSemaphoreTake(s_mutex, portMAX_DELAY);
        s_stopping = true;
        bool pending = s_pending;
        xSemaphoreGive(s_mutex);
        if (!pending) return;
        vTaskDelay(1);
    }
}
