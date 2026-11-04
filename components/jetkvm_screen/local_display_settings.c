#include "local_display_settings.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "nvs.h"
#include "sdkconfig.h"
#include <string.h>

static const char *TAG = "local_display";
typedef struct {
    uint32_t version, rotation;
    jetkvm_backlight_settings_t backlight;
} display_record_t;
static display_record_t s_config;
static local_display_driver_t s_driver;
static local_display_state_t s_state;
static local_display_state_t s_published_state;
static portMUX_TYPE s_snapshot_lock = portMUX_INITIALIZER_UNLOCKED;
static SemaphoreHandle_t s_mutex;
static int64_t s_woke_us;
static bool s_wake;

uint32_t local_display_brightness(const jetkvm_backlight_settings_t *settings, uint64_t idle_seconds)
{
    if (settings->off_after && idle_seconds >= settings->off_after) return 0;
    if (settings->dim_after && idle_seconds >= settings->dim_after) return settings->max_brightness / 2;
    return settings->max_brightness;
}

static bool lock(void)
{ return s_mutex && xSemaphoreTake(s_mutex, pdMS_TO_TICKS(1000)) == pdTRUE; }
static void unlock(void)
{
    s_state.dark = s_state.ready && s_state.active && s_state.brightness == 0 &&
        s_config.backlight.max_brightness > 0;
    /* Publish completed state without making video metrics wait for NVS/SPI. */
    portENTER_CRITICAL(&s_snapshot_lock);
    s_published_state = s_state;
    portEXIT_CRITICAL(&s_snapshot_lock);
    xSemaphoreGive(s_mutex);
}
static bool valid(const display_record_t *record)
{
    return record->version == 1 && record->rotation <= 270 && record->rotation % 90 == 0 &&
        record->backlight.max_brightness <= 255 && record->backlight.dim_after <= INT32_MAX &&
        record->backlight.off_after <= INT32_MAX;
}
static esp_err_t save(const display_record_t *record)
{
    nvs_handle_t handle;
    esp_err_t error = nvs_open("jk_display", NVS_READWRITE, &handle);
    if (error != ESP_OK) return error;
    error = nvs_set_blob(handle, "settings", record, sizeof(*record));
    if (error == ESP_OK) error = nvs_commit(handle);
    nvs_close(handle);
    return error;
}
static esp_err_t brightness(uint32_t value)
{
    esp_err_t error = s_driver.brightness(value);
    if (error == ESP_OK) s_state.brightness = value;
    s_state.error = error;
    return error;
}
static bool get_backlight(jetkvm_backlight_settings_t *settings, void *ctx)
{
    (void)ctx;
    if (!settings || !lock()) return false;
    bool ready = s_state.ready;
    if (ready) *settings = s_config.backlight;
    unlock(); return ready;
}
static bool get_rotation(uint16_t *rotation, void *ctx)
{
    (void)ctx;
    if (!rotation || !lock()) return false;
    bool ready = s_state.ready;
    if (ready) *rotation = s_config.rotation;
    unlock(); return ready;
}
static const char *set_backlight(const jetkvm_backlight_settings_t *settings, void *ctx)
{
    (void)ctx;
    if (!settings || !lock()) return "Local display unavailable or busy";
    display_record_t next = s_config;
    next.backlight = *settings;
    esp_err_t error = ESP_ERR_INVALID_STATE;
    if (s_state.ready && valid(&next)) {
        uint32_t previous_brightness = s_state.brightness;
        error = brightness(s_state.active ? settings->max_brightness : 0);
        if (error == ESP_OK) error = save(&next);
        if (error != ESP_OK) {
            if (brightness(previous_brightness) != ESP_OK) {
                unlock(); return "Backlight update and restore failed";
            }
        } else {
            s_config = next;
            s_woke_us = esp_timer_get_time();
        }
    }
    s_state.error = error;
    unlock(); return error == ESP_OK ? NULL : "Could not apply or save backlight settings";
}
static const char *set_rotation(uint16_t rotation, void *ctx)
{
    (void)ctx;
    if (!lock()) return "Local display unavailable or busy";
    display_record_t next = s_config;
    next.rotation = rotation;
    esp_err_t error = ESP_ERR_INVALID_STATE;
    if (s_state.ready && valid(&next)) {
        uint32_t previous_brightness = s_state.brightness;
        error = s_driver.rotation(rotation);
        if (error == ESP_OK) error = brightness(s_state.active ? next.backlight.max_brightness : 0);
        if (error == ESP_OK) error = save(&next);
        if (error != ESP_OK) {
            esp_err_t restore_rotation = s_driver.rotation(s_config.rotation);
            esp_err_t restore_brightness = brightness(previous_brightness);
            if (restore_rotation != ESP_OK || restore_brightness != ESP_OK) {
                s_state.error = ESP_FAIL;
                unlock(); return "Display rotation and restore failed";
            }
        } else {
            s_config = next;
            s_state.rotation = rotation;
            s_woke_us = esp_timer_get_time();
        }
    }
    s_state.error = error;
    unlock(); return error == ESP_OK ? NULL : "Could not apply or save display rotation";
}

esp_err_t local_display_settings_init(uint16_t rotation, const local_display_driver_t *driver,
                                      bool restore_settings)
{
    if (s_mutex) return ESP_OK;
    if (!driver || !driver->rotation || !driver->brightness || !driver->active) return ESP_ERR_INVALID_ARG;
    s_mutex = xSemaphoreCreateMutex();
    if (!s_mutex) return ESP_ERR_NO_MEM;
    s_driver = *driver;
    /* Use upstream Medium (near the previous 15%) and keep the always-on default. */
    s_config = (display_record_t){.version = 1, .rotation = rotation, .backlight = {.max_brightness = 35}};
    nvs_handle_t handle;
    /* Reset boots render before erasure. Do not cache the old settings or
     * let a saved dim/off policy hide reset progress. Never writes NVS. */
    esp_err_t error = restore_settings ? nvs_open("jk_display", NVS_READONLY, &handle) :
                                       ESP_ERR_NVS_NOT_FOUND;
    if (error == ESP_OK) {
        display_record_t saved;
        size_t size = sizeof(saved);
        error = nvs_get_blob(handle, "settings", &saved, &size);
        nvs_close(handle);
        if (error == ESP_OK && size == sizeof(saved) && valid(&saved)) s_config = saved;
        else if (error != ESP_ERR_NVS_NOT_FOUND) ESP_LOGW(TAG, "Invalid saved display settings; using defaults");
    } else if (error != ESP_ERR_NVS_NOT_FOUND) ESP_LOGW(TAG, "Display settings unavailable; using defaults");
    const jetkvm_local_display_context_t context = {
        .get_backlight = get_backlight, .set_backlight = set_backlight,
        .get_rotation = get_rotation, .set_rotation = set_rotation,
#if CONFIG_JETKVM_SCREEN_BUTTONS
        .buttons = true,
#endif
    };
    jetkvm_control_set_default_local_display_context(&context);
    return ESP_OK;
}
esp_err_t local_display_prepare(void)
{
    if (!lock()) return ESP_ERR_INVALID_STATE;
    esp_err_t error = s_driver.rotation(s_config.rotation);
    s_state.rotation = s_config.rotation;
    s_state.error = error;
    unlock(); return error;
}
esp_err_t local_display_ready(void)
{
    if (!lock()) return ESP_ERR_INVALID_STATE;
    s_state.ready = s_state.error == ESP_OK;
    esp_err_t error = s_state.error;
    unlock(); return error;
}
esp_err_t local_display_active(bool enabled)
{
    if (!lock()) return ESP_ERR_INVALID_STATE;
    bool previous_active = s_state.active;
    uint32_t previous_brightness = s_state.brightness;
    esp_err_t error = s_driver.active(enabled);
    if (error == ESP_OK) error = brightness(enabled ? s_config.backlight.max_brightness : 0);
    if (error == ESP_OK) {
        s_state.active = enabled;
        s_woke_us = esp_timer_get_time();
    } else {
        /* A failed PWM update must not leave the panel and owner disagreeing. */
        esp_err_t restore_active = s_driver.active(previous_active);
        esp_err_t restore_brightness = brightness(previous_brightness);
        if (restore_active != ESP_OK || restore_brightness != ESP_OK) error = ESP_FAIL;
    }
    s_state.error = error;
    unlock(); return error;
}
void local_display_wake(void)
{ __atomic_store_n(&s_wake, true, __ATOMIC_RELEASE); }
void local_display_tick(void)
{
    if (!s_mutex || xSemaphoreTake(s_mutex, 0) != pdTRUE) return;
    int64_t now = esp_timer_get_time();
    if (__atomic_exchange_n(&s_wake, false, __ATOMIC_ACQ_REL)) s_woke_us = now;
    if (s_state.ready && s_state.active) {
        uint64_t idle = now > s_woke_us ? (uint64_t)(now - s_woke_us) / 1000000 : 0;
        uint32_t value = local_display_brightness(&s_config.backlight, idle);
        if (value != s_state.brightness) (void)brightness(value);
    }
    unlock();
}
bool local_display_state(local_display_state_t *state)
{
    if (!state || !s_mutex) return false;
    portENTER_CRITICAL(&s_snapshot_lock);
    *state = s_published_state;
    portEXIT_CRITICAL(&s_snapshot_lock);
    return true;
}
