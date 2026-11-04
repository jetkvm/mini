#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "jetkvm_cloud_config.h"
#include "mock_nvs.h"

static void configure_registered(jetkvm_cloud_config_t *config,
                                 const char *api_url)
{
    jetkvm_cloud_config_defaults(config);
    assert(jetkvm_cloud_config_set_urls(
               config, api_url, "https://app.example.test") == ESP_OK);
    assert(jetkvm_cloud_config_set_registration(
               config, "opaque-permanent-token",
               "client.apps.googleusercontent.com:subject") == ESP_OK);
}

int main(void)
{
    mock_nvs_reset();
    jetkvm_cloud_config_t config;
    assert(jetkvm_cloud_config_load(&config) == ESP_OK);
    assert(strcmp(config.cloud_url, JETKVM_DEFAULT_CLOUD_URL) == 0);
    assert(!jetkvm_cloud_config_is_registered(&config));

    configure_registered(&config, "https://first.example.test");
    assert(jetkvm_cloud_config_save(&config) == ESP_OK);
    assert(mock_nvs_committed_size() > sizeof(config));

    jetkvm_cloud_config_t loaded;
    memset(&loaded, 0xa5, sizeof(loaded));
    assert(jetkvm_cloud_config_load(&loaded) == ESP_OK);
    assert(memcmp(&loaded, &config, sizeof(config)) == 0);
    assert(jetkvm_cloud_config_is_registered(&loaded));

    jetkvm_cloud_config_t second;
    configure_registered(&second, "https://second.example.test");
    mock_nvs_fail_next_commit();
    assert(jetkvm_cloud_config_save(&second) == ESP_FAIL);
    assert(jetkvm_cloud_config_load(&loaded) == ESP_OK);
    assert(strcmp(loaded.cloud_url, "https://first.example.test") == 0);

    size_t record_size = mock_nvs_committed_size();
    mock_nvs_corrupt_byte(record_size - 1U, 0x01U);
    assert(jetkvm_cloud_config_load(&loaded) == ESP_ERR_INVALID_CRC);

    mock_nvs_reset();
    assert(jetkvm_cloud_config_save(&config) == ESP_OK);
    mock_nvs_corrupt_byte(sizeof(uint32_t), 0x01U);
    assert(jetkvm_cloud_config_load(&loaded) == ESP_ERR_INVALID_VERSION);

    mock_nvs_reset();
    assert(jetkvm_cloud_config_save(&config) == ESP_OK);
    assert(jetkvm_cloud_config_clear_registration_persisted() == ESP_OK);
    assert(jetkvm_cloud_config_load(&loaded) == ESP_OK);
    assert(!jetkvm_cloud_config_is_registered(&loaded));
    assert(strcmp(loaded.cloud_url, "https://first.example.test") == 0);

    puts("JetKVM cloud NVS transactional record: OK");
    return 0;
}
