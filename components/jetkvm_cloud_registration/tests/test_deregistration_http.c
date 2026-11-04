#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "jetkvm_cloud_registration.h"
#include "esp_http_client.h"
#include "esp_crt_bundle.h"

static int status, initialized, performed, cleaned;
static bool fail_init;
static esp_err_t header_error, perform_error;
static char client_storage;
static const char *expected_url = "https://api.jetkvm.com/devices/80f1b2d631d9";

esp_err_t esp_crt_bundle_attach(void *config) { (void)config; return ESP_OK; }
esp_http_client_handle_t esp_http_client_init(const esp_http_client_config_t *config)
{
    ++initialized;
    assert(strcmp(config->url, expected_url) == 0);
    assert(config->method == HTTP_METHOD_DELETE);
    assert(config->timeout_ms == 10000);
    assert(config->disable_auto_redirect);
    assert(config->crt_bundle_attach == esp_crt_bundle_attach);
    assert(!config->skip_cert_common_name_check && !config->common_name);
    return fail_init ? NULL : &client_storage;
}
esp_err_t esp_http_client_set_header(esp_http_client_handle_t client, const char *key, const char *value)
{
    assert(client == &client_storage);
    assert(strcmp(key, "Authorization") == 0);
    assert(strcmp(value, "Bearer test-token") == 0);
    return header_error;
}
esp_err_t esp_http_client_perform(esp_http_client_handle_t client)
{
    assert(client == &client_storage);
    ++performed;
    return perform_error;
}
int esp_http_client_get_status_code(esp_http_client_handle_t client)
{
    assert(client == &client_storage);
    return status;
}
esp_err_t esp_http_client_cleanup(esp_http_client_handle_t client)
{
    assert(client == &client_storage);
    ++cleaned;
    return ESP_OK;
}
static void reset(void)
{
    status = 204;
    initialized = performed = cleaned = 0;
    fail_init = false;
    header_error = perform_error = ESP_OK;
}
int main(void)
{
    jetkvm_cloud_config_t config;
    jetkvm_cloud_config_defaults(&config);
    assert(jetkvm_cloud_config_set_registration(&config, "test-token", "aud:sub") == ESP_OK);
    const int statuses[] = {200, 204, 299, 404, 301, 401, 500};
    for (size_t i = 0; i < sizeof(statuses) / sizeof(statuses[0]); ++i) {
        reset();
        status = statuses[i];
        bool success = (status >= 200 && status < 300) || status == 404;
        assert(jetkvm_cloud_registration_delete_remote(&config, "80f1b2d631d9") ==
               (success ? ESP_OK : ESP_ERR_INVALID_RESPONSE));
        assert(initialized == 1 && performed == 1 && cleaned == 1);
    }
    reset();
    strcpy(config.cloud_url, "http://api.jetkvm.com");
#if JETKVM_MOCK_CLOUD
    /* A mock cloud build deregisters from the mock's http:// URL. */
    expected_url = "http://api.jetkvm.com/devices/80f1b2d631d9";
    assert(jetkvm_cloud_registration_delete_remote(&config, "80f1b2d631d9") == ESP_OK);
    assert(initialized == 1);
    expected_url = "https://api.jetkvm.com/devices/80f1b2d631d9";
    reset();
#else
    assert(jetkvm_cloud_registration_delete_remote(&config, "80f1b2d631d9") == ESP_ERR_INVALID_ARG);
    assert(initialized == 0);
#endif
    strcpy(config.cloud_url, JETKVM_DEFAULT_CLOUD_URL);
    assert(jetkvm_cloud_registration_delete_remote(&config, "../bad") == ESP_ERR_INVALID_ARG);
    assert(initialized == 0);
    fail_init = true;
    assert(jetkvm_cloud_registration_delete_remote(&config, "80f1b2d631d9") == ESP_ERR_NO_MEM);
    assert(performed == 0 && cleaned == 0);
    reset();
    header_error = ESP_FAIL;
    assert(jetkvm_cloud_registration_delete_remote(&config, "80f1b2d631d9") == ESP_FAIL);
    assert(performed == 0 && cleaned == 1);
    reset();
    perform_error = ESP_FAIL;
    assert(jetkvm_cloud_registration_delete_remote(&config, "80f1b2d631d9") == ESP_FAIL);
    assert(performed == 1 && cleaned == 1);
    puts("cloud deregistration HTTPS tests passed");
}
