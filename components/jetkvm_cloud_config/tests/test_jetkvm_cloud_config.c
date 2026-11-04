#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "jetkvm_cloud_config.h"

static void test_defaults_and_registration(void)
{
    jetkvm_cloud_config_t config;
    jetkvm_cloud_config_defaults(&config);
    assert(jetkvm_cloud_config_validate(&config));
    assert(!jetkvm_cloud_config_is_registered(&config));
    assert(strcmp(config.cloud_url, JETKVM_DEFAULT_CLOUD_URL) == 0);
    assert(strcmp(config.cloud_app_url, JETKVM_DEFAULT_CLOUD_APP_URL) == 0);

    assert(jetkvm_cloud_config_set_registration(
               &config, "0123456789abcdef0123456789abcdef01234567",
               "client.apps.googleusercontent.com:123456789") == ESP_OK);
    assert(jetkvm_cloud_config_is_registered(&config));
    jetkvm_cloud_config_clear_registration(&config);
    assert(jetkvm_cloud_config_validate(&config));
    assert(!jetkvm_cloud_config_is_registered(&config));
}

static void test_urls_and_header_injection(void)
{
    jetkvm_cloud_config_t config;
    jetkvm_cloud_config_defaults(&config);
    assert(jetkvm_cloud_config_set_urls(
               &config, "https://cloud.example.test/base",
               "http://app.example.test:8080") == ESP_OK);
    assert(strcmp(config.cloud_url, "https://cloud.example.test/base") == 0);

    jetkvm_cloud_config_t snapshot = config;
    assert(jetkvm_cloud_config_set_urls(
               &config, "wss://cloud.example.test",
               "https://app.example.test") == ESP_ERR_INVALID_ARG);
    assert(memcmp(&config, &snapshot, sizeof(config)) == 0);
    assert(jetkvm_cloud_config_set_urls(
               &config, "https://cloud.example.test\r\nX-Evil: yes",
               "https://app.example.test") == ESP_ERR_INVALID_ARG);
    assert(jetkvm_cloud_config_set_urls(
               &config, "https://user@cloud.example.test",
               "https://app.example.test") == ESP_ERR_INVALID_ARG);
    assert(jetkvm_cloud_config_set_urls(
               &config, "https://cloud.example.test/\"quoted",
               "https://app.example.test") == ESP_ERR_INVALID_ARG);
    assert(jetkvm_cloud_config_set_registration(
               &config, "token\r\nX-Evil: yes", "aud:sub") ==
           ESP_ERR_INVALID_ARG);
}

static void test_pair_and_bounds(void)
{
    jetkvm_cloud_config_t config;
    jetkvm_cloud_config_defaults(&config);
    memcpy(config.cloud_token, "token", sizeof("token"));
    assert(!jetkvm_cloud_config_validate(&config));

    char oversized[JETKVM_CLOUD_TOKEN_CAPACITY + 1U];
    memset(oversized, 'a', sizeof(oversized));
    oversized[sizeof(oversized) - 1U] = '\0';
    assert(jetkvm_cloud_config_set_registration(
               &config, oversized, "aud:sub") == ESP_ERR_INVALID_ARG);

    jetkvm_cloud_config_defaults(&config);
    config.schema_version++;
    assert(!jetkvm_cloud_config_validate(&config));
}

int main(void)
{
    test_defaults_and_registration();
    test_urls_and_header_injection();
    test_pair_and_bounds();
    puts("JetKVM cloud configuration core: OK");
    return 0;
}
