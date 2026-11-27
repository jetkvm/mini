#include "local_display_settings.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "jetkvm_time_sync.h"
#include "jetkvm_timezone.h"
#include "nvs.h"
#include "sdkconfig.h"
#include <string.h>
#include <time.h>

static const char *TAG = "local_display";
typedef struct {
    uint32_t version, rotation;
    jetkvm_backlight_settings_t backlight;
} display_record_t;
/* The quiet hours have their own NVS key, so that a firmware without them
 * still reads the "settings" record. */
typedef struct {
    uint8_t version, enabled, dim, reserved;
    uint16_t start_minute, end_minute;
    char timezone[JETKVM_CONTROL_DISPLAY_SCHEDULE_TIMEZONE_CAPACITY];
} schedule_record_t;
_Static_assert(sizeof(schedule_record_t) == 8U + JETKVM_CONTROL_DISPLAY_SCHEDULE_TIMEZONE_CAPACITY,
               "the stored schedule has no padding");
/* Never turned on: no time zone yet. */
static const jetkvm_display_schedule_t s_default_schedule = {
    .start_minute = 22U * 60U, .end_minute = 6U * 60U};
static display_record_t s_config;
static jetkvm_display_schedule_t s_schedule;
/* The rule of s_schedule.timezone, and the quiet state of one minute. */
static const char *s_schedule_rule;
static int64_t s_quiet_minute = -1;
static bool s_quiet;
static local_display_driver_t s_driver;
static local_display_state_t s_state;
static local_display_state_t s_published_state;
static portMUX_TYPE s_snapshot_lock = portMUX_INITIALIZER_UNLOCKED;
static SemaphoreHandle_t s_mutex;
static int64_t s_woke_us, s_pressed_us;
static bool s_wake, s_notice, s_pressed;

uint32_t local_display_brightness(const jetkvm_backlight_settings_t *settings, uint64_t idle_seconds)
{
    if (settings->off_after && idle_seconds >= settings->off_after) return 0;
    if (settings->dim_after && idle_seconds >= settings->dim_after) return settings->max_brightness / 2;
    return settings->max_brightness;
}

bool local_display_quiet(const jetkvm_display_schedule_t *schedule, const char *rule, int64_t utc_seconds)
{
    uint16_t minute;
    if (!schedule->enabled || !jetkvm_timezone_minute_of_day(rule, utc_seconds, &minute)) return false;
    return schedule->start_minute < schedule->end_minute
        ? minute >= schedule->start_minute && minute < schedule->end_minute
        : minute >= schedule->start_minute || minute < schedule->end_minute;
}

uint32_t local_display_scheduled_brightness(const jetkvm_backlight_settings_t *settings,
                                            const jetkvm_display_schedule_t *schedule, bool quiet,
                                            uint64_t idle_seconds, uint64_t press_idle_seconds)
{
    uint32_t value = local_display_brightness(settings, idle_seconds);
    if (!quiet) return value;
    uint32_t lit = settings->dim_after ? settings->dim_after :
                   settings->off_after ? settings->off_after : LOCAL_DISPLAY_QUIET_PRESS_SECONDS;
    if (press_idle_seconds < lit) return value;
    uint32_t limit = schedule->dim ? settings->max_brightness / 2 : 0;
    return value < limit ? value : limit;
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
static esp_err_t save_blob(const char *key, const void *record, size_t size)
{
    nvs_handle_t handle;
    esp_err_t error = nvs_open("jk_display", NVS_READWRITE, &handle);
    if (error != ESP_OK) return error;
    error = nvs_set_blob(handle, key, record, size);
    if (error == ESP_OK) error = nvs_commit(handle);
    nvs_close(handle);
    return error;
}
static esp_err_t save(const display_record_t *record)
{ return save_blob("settings", record, sizeof(*record)); }
/* True for a schedule that can be stored and used, with the rule of its
 * zone. A schedule that is off may have no zone, and then has no rule. */
static bool schedule_valid(const jetkvm_display_schedule_t *schedule, const char **rule)
{
    *rule = NULL;
    if (schedule->start_minute > 1439U || schedule->end_minute > 1439U ||
        schedule->start_minute == schedule->end_minute ||
        !memchr(schedule->timezone, '\0', sizeof(schedule->timezone))) return false;
    if (schedule->timezone[0] == '\0') return !schedule->enabled;
    *rule = jetkvm_timezone_rule(schedule->timezone);
    return *rule != NULL;
}
/* A settings change counts as a press, so that the display shows it. */
static void pressed(int64_t now)
{
    s_woke_us = s_pressed_us = now;
    s_pressed = true;
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
            pressed(esp_timer_get_time());
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
            pressed(esp_timer_get_time());
        }
    }
    s_state.error = error;
    unlock(); return error == ESP_OK ? NULL : "Could not apply or save display rotation";
}
static bool get_schedule(jetkvm_display_schedule_t *schedule, void *ctx)
{
    (void)ctx;
    if (!schedule || !lock()) return false;
    *schedule = s_schedule;
    unlock(); return true;
}
static const char *set_schedule(const jetkvm_display_schedule_t *schedule, void *ctx)
{
    (void)ctx;
    if (!schedule) return "Display schedule unavailable";
    const char *rule;
    if (!schedule_valid(schedule, &rule))
        return "Unknown time zone, or start and end are not two times of the day";
    schedule_record_t record = {.version = 1, .enabled = schedule->enabled, .dim = schedule->dim,
        .start_minute = schedule->start_minute, .end_minute = schedule->end_minute};
    /* The record is all zero after the name, so equal schedules store equal
     * bytes. schedule_valid() found the name's end inside the field. */
    memcpy(record.timezone, schedule->timezone, strlen(schedule->timezone));
    if (!lock()) return "Local display unavailable or busy";
    esp_err_t error = save_blob("schedule", &record, sizeof(record));
    if (error == ESP_OK) {
        s_schedule = (jetkvm_display_schedule_t){.enabled = schedule->enabled, .dim = schedule->dim,
            .start_minute = schedule->start_minute, .end_minute = schedule->end_minute};
        memcpy(s_schedule.timezone, record.timezone, sizeof(s_schedule.timezone));
        s_schedule_rule = rule;
        s_quiet_minute = -1;
    }
    unlock(); return error == ESP_OK ? NULL : "Could not save the display schedule";
}
static bool schedule_timezone(size_t index, const char **name, void *ctx)
{
    (void)ctx;
    if (!name) return false;
    *name = jetkvm_timezone_name(index);
    return true;
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
    s_schedule = s_default_schedule;
    s_quiet_minute = -1;
    s_pressed = false;
    error = restore_settings ? nvs_open("jk_display", NVS_READONLY, &handle) : ESP_ERR_NVS_NOT_FOUND;
    if (error == ESP_OK) {
        schedule_record_t saved = {0};
        size_t size = sizeof(saved);
        error = nvs_get_blob(handle, "schedule", &saved, &size);
        nvs_close(handle);
        jetkvm_display_schedule_t schedule = {.enabled = saved.enabled == 1U, .dim = saved.dim == 1U,
            .start_minute = saved.start_minute, .end_minute = saved.end_minute};
        memcpy(schedule.timezone, saved.timezone, sizeof(schedule.timezone));
        const char *rule;
        if (error == ESP_OK && size == sizeof(saved) && saved.version == 1U && saved.enabled <= 1U &&
            saved.dim <= 1U && schedule_valid(&schedule, &rule)) s_schedule = schedule;
        else if (error != ESP_ERR_NVS_NOT_FOUND) ESP_LOGW(TAG, "Invalid saved display schedule; it is off");
    }
    (void)schedule_valid(&s_schedule, &s_schedule_rule);
    const jetkvm_local_display_context_t context = {
        .get_backlight = get_backlight, .set_backlight = set_backlight,
        .get_rotation = get_rotation, .set_rotation = set_rotation,
        .get_schedule = get_schedule, .set_schedule = set_schedule,
        .schedule_timezone = schedule_timezone,
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
void local_display_notice(void)
{ __atomic_store_n(&s_notice, true, __ATOMIC_RELEASE); }
/* The schedule does nothing until the clock has the network time. The local
 * time of a minute is found once. */
static bool quiet_now(void)
{
    if (!s_schedule.enabled || !jetkvm_time_sync_ready()) return false;
    int64_t utc = (int64_t)time(NULL);
    if (utc / 60 != s_quiet_minute) {
        s_quiet_minute = utc / 60;
        s_quiet = local_display_quiet(&s_schedule, s_schedule_rule, utc);
    }
    return s_quiet;
}
void local_display_tick(void)
{
    if (!s_mutex || xSemaphoreTake(s_mutex, 0) != pdTRUE) return;
    int64_t now = esp_timer_get_time();
    if (__atomic_exchange_n(&s_wake, false, __ATOMIC_ACQ_REL)) pressed(now);
    bool quiet = quiet_now();
    if (__atomic_exchange_n(&s_notice, false, __ATOMIC_ACQ_REL) && !quiet) s_woke_us = now;
    if (s_state.ready && s_state.active) {
        uint64_t idle = now > s_woke_us ? (uint64_t)(now - s_woke_us) / 1000000 : 0;
        uint64_t press_idle = !s_pressed ? UINT64_MAX :
            now > s_pressed_us ? (uint64_t)(now - s_pressed_us) / 1000000 : 0;
        uint32_t value = local_display_scheduled_brightness(&s_config.backlight, &s_schedule, quiet,
                                                            idle, press_idle);
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
