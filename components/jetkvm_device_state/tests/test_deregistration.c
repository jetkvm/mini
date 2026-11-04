#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "jetkvm_device_state.h"

static jetkvm_cloud_config_t persisted;
static esp_err_t save_result;
static unsigned saves, notifications;

esp_err_t jetkvm_device_id_get(char *out, size_t size) {
    assert(size >= 13); strcpy(out, "001122aabbcc"); return ESP_OK;
}
esp_err_t jetkvm_cloud_config_load(jetkvm_cloud_config_t *out) { *out = persisted; return ESP_OK; }
esp_err_t jetkvm_cloud_config_save(const jetkvm_cloud_config_t *config) {
    ++saves;
    if (save_result == ESP_OK) persisted = *config;
    return save_result;
}
static void changed(uint32_t changes, uint32_t generation, void *ctx) {
    (void)ctx;
    assert(changes == JETKVM_DEVICE_CLOUD_CHANGE_REGISTRATION);
    jetkvm_cloud_config_t snapshot; uint32_t current;
    // The observer can read state: persistence and mutex release preceded it.
    assert(jetkvm_device_state_cloud_snapshot(&snapshot, &current) == ESP_OK);
    assert(current == generation);
    assert(memcmp(&snapshot, &persisted, sizeof(snapshot)) == 0);
    ++notifications;
}
int main(void) {
    jetkvm_cloud_config_defaults(&persisted);
    assert(jetkvm_cloud_config_set_registration(&persisted, "token1", "aud:sub") == ESP_OK);
    assert(jetkvm_device_state_init() == ESP_OK);
    assert(jetkvm_device_state_cloud_subscribe(changed, NULL) == ESP_OK);
    jetkvm_cloud_config_t old, current; uint32_t generation, newer;
    assert(jetkvm_device_state_cloud_snapshot(&old, &generation) == ESP_OK);
    assert(jetkvm_device_state_cloud_set_registration("token2", "aud:new") == ESP_OK);
    unsigned before = saves;
    assert(jetkvm_device_state_cloud_clear_registration_if_current(generation, old.cloud_url) == ESP_ERR_INVALID_STATE);
    assert(saves == before && notifications == 1);
    assert(jetkvm_device_state_cloud_snapshot(&current, &newer) == ESP_OK);
    assert(strcmp(current.cloud_token, "token2") == 0);
    assert(jetkvm_device_state_cloud_clear_registration_if_current(newer, "https://other.example") == ESP_ERR_INVALID_STATE);
    save_result = ESP_FAIL;
    assert(jetkvm_device_state_cloud_clear_registration_if_current(newer, current.cloud_url) == ESP_FAIL);
    assert(notifications == 1);
    assert(jetkvm_device_state_cloud_snapshot(&current, &generation) == ESP_OK);
    assert(generation == newer && strcmp(current.cloud_token, "token2") == 0);
    save_result = ESP_OK;
    assert(jetkvm_device_state_cloud_clear_registration_if_current(newer, current.cloud_url) == ESP_OK);
    assert(notifications == 2);
    assert(jetkvm_device_state_cloud_snapshot(&current, &generation) == ESP_OK);
    assert(!jetkvm_cloud_config_is_registered(&current));
    assert(current.cloud_token[0] == 0 && current.google_identity[0] == 0);
    assert(strcmp(current.cloud_url, old.cloud_url) == 0);
    assert(strcmp(current.cloud_app_url, old.cloud_app_url) == 0);
    assert(generation != newer);
    assert(jetkvm_device_state_cloud_set_registration("token3", "aud:reply") == ESP_OK);
    assert(jetkvm_device_state_cloud_snapshot(&current, &generation) == ESP_OK);
    before = notifications;
    assert(jetkvm_device_state_cloud_prepare_deregistration(generation, current.cloud_url) == ESP_OK);
    assert(!jetkvm_cloud_config_is_registered(&persisted));
    assert(jetkvm_device_state_cloud_snapshot(&current, &newer) == ESP_OK);
    assert(newer == generation && jetkvm_cloud_config_is_registered(&current));
    assert(notifications == before);
    assert(jetkvm_device_state_cloud_set_registration("token4", "aud:race") == ESP_ERR_INVALID_STATE);
    assert(jetkvm_device_state_cloud_set_urls(current.cloud_url, current.cloud_app_url, NULL) == ESP_ERR_INVALID_STATE);
    assert(jetkvm_device_state_cloud_reload() == ESP_ERR_INVALID_STATE);
    assert(jetkvm_device_state_cloud_clear_registration() == ESP_ERR_INVALID_STATE);
    jetkvm_device_state_cloud_finish_deregistration();
    assert(notifications == before + 1);
    assert(jetkvm_device_state_cloud_snapshot(&current, &newer) == ESP_OK);
    assert(newer != generation && !jetkvm_cloud_config_is_registered(&current));
    jetkvm_device_state_cloud_finish_deregistration();
    assert(notifications == before + 1);
    puts("cloud deregistration CAS, persistence and reply ordering: OK");
}
