#include <assert.h>
#include <string.h>

#include "jetkvm_mqtt.h"

int main(void)
{
    jetkvm_mqtt_config_t config;
    jetkvm_mqtt_config_defaults(&config);
    assert(!config.enabled);
    assert(config.port == 1883);
    assert(strcmp(config.base_topic, "jetkvm") == 0);
    assert(config.enable_ha_discovery);
    assert(config.enable_actions);
    assert(config.debounce_ms == 500);
    assert(jetkvm_mqtt_config_validate(&config, NULL, 0U));

    config.enabled = true;
    char error[JETKVM_MQTT_ERROR_CAPACITY];
    assert(!jetkvm_mqtt_config_validate(&config, error, sizeof(error)));
    assert(strcmp(error,
                  "broker address is required when MQTT is enabled") == 0);
    strcpy(config.broker, "192.168.50.1");
    assert(jetkvm_mqtt_config_validate(&config, error, sizeof(error)));

    config.port = 0;
    assert(!jetkvm_mqtt_config_validate(&config, error, sizeof(error)));
    assert(strcmp(error, "port must be between 1 and 65535") == 0);
    config.port = 1883;

    strcpy(config.base_topic, "bad/+");
    assert(!jetkvm_mqtt_config_validate(&config, error, sizeof(error)));
    strcpy(config.base_topic, "bad topic");
    assert(!jetkvm_mqtt_config_validate(&config, error, sizeof(error)));

    jetkvm_mqtt_config_t existing;
    jetkvm_mqtt_config_defaults(&existing);
    strcpy(existing.password, "secret");
    strcpy(config.base_topic, "");
    strcpy(config.password, JETKVM_MQTT_PASSWORD_MASK);
    config.debounce_ms = -9;
    assert(jetkvm_mqtt_config_normalize(
        &config, &existing, error, sizeof(error)));
    assert(strcmp(config.base_topic, "jetkvm") == 0);
    assert(strcmp(config.password, "secret") == 0);
    assert(config.debounce_ms == 0);

    char topic[JETKVM_MQTT_EFFECTIVE_TOPIC_CAPACITY];
    assert(jetkvm_mqtt_build_base_topic(
        &config, "abc123", topic, sizeof(topic)));
    assert(strcmp(topic, "jetkvm/abc123") == 0);
    strcpy(config.base_topic, "site/abc123/kvm");
    assert(jetkvm_mqtt_build_base_topic(
        &config, "abc123", topic, sizeof(topic)));
    assert(strcmp(topic, "site/abc123/kvm") == 0);
    return 0;
}
