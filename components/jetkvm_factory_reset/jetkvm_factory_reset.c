#include "jetkvm_factory_reset.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_partition.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/idf_additions.h"
#include "freertos/task.h"
#include "nvs.h"
#include <string.h>
#include <stdatomic.h>

#define RESET_NAMESPACE "jk_reset"
#define RESET_KEY "intent"
#define RESET_LOCAL_SETTINGS 3U

typedef struct {
    uint32_t version, phase, card_present, card_serial;
} reset_intent_t;
static reset_intent_t s_intent;
static portMUX_TYPE s_lock = portMUX_INITIALIZER_UNLOCKED;
static bool s_busy;
static TaskHandle_t s_restart_task;
enum { RESET_IDLE, RESET_PENDING, RESET_FAILED };
static atomic_int s_state;

bool jetkvm_factory_reset_pending(void) { return atomic_load(&s_state) != RESET_IDLE; }
const char *jetkvm_factory_reset_error(void)
{
    return atomic_load(&s_state) == RESET_FAILED ?
        "Factory reset incomplete. Reboot to retry." : NULL;
}

static esp_err_t save_intent(const reset_intent_t *intent)
{
    nvs_handle_t handle;
    esp_err_t error = nvs_open(RESET_NAMESPACE, NVS_READWRITE, &handle);
    if (error != ESP_OK) return error;
    error = nvs_set_blob(handle, RESET_KEY, intent, sizeof(*intent));
    if (error == ESP_OK) error = nvs_commit(handle);
    nvs_close(handle);
    return error;
}

/* Restart iteration after deleting a namespace: never advance an iterator
 * into entries which its preceding erase may have invalidated. Keep the
 * durable intent until every cleanup step has succeeded. */
static esp_err_t erase_settings(void)
{
    for (;;) {
        nvs_iterator_t iterator = NULL;
        esp_err_t error = nvs_entry_find("nvs", NULL, NVS_TYPE_ANY, &iterator);
        char namespace_name[16] = {0};
        while (error == ESP_OK) {
            nvs_entry_info_t info;
            nvs_entry_info(iterator, &info);
            if (strcmp(info.namespace_name, RESET_NAMESPACE) != 0) {
                memcpy(namespace_name, info.namespace_name, sizeof(namespace_name));
                break;
            }
            error = nvs_entry_next(&iterator);
        }
        nvs_release_iterator(iterator);
        if (error != ESP_OK && error != ESP_ERR_NVS_NOT_FOUND) return error;
        if (!namespace_name[0]) return ESP_OK;
        nvs_handle_t handle;
        error = nvs_open(namespace_name, NVS_READWRITE, &handle);
        if (error != ESP_OK) return error;
        error = nvs_erase_all(handle);
        if (error == ESP_OK) error = nvs_commit(handle);
        nvs_close(handle);
        if (error != ESP_OK) return error;
    }
}

static esp_err_t clear_intent(void)
{
    nvs_handle_t handle;
    esp_err_t error = nvs_open(RESET_NAMESPACE, NVS_READWRITE, &handle);
    if (error != ESP_OK) return error;
    error = nvs_erase_key(handle, RESET_KEY);
    if (error == ESP_OK) error = nvs_commit(handle);
    nvs_close(handle);
    if (error == ESP_OK) memset(&s_intent, 0, sizeof(s_intent));
    return error;
}

static esp_err_t discard_invalid_intent(void)
{
    ESP_LOGW("factory_reset", "Invalid reset intent; user data was not erased");
    return clear_intent();
}

static esp_err_t load_intent(void)
{
    /* An unreadable or rejected marker must never authorize later cleanup. */
    memset(&s_intent, 0, sizeof(s_intent));
    reset_intent_t candidate = {0};
    nvs_handle_t handle;
    esp_err_t error = nvs_open(RESET_NAMESPACE, NVS_READONLY, &handle);
    if (error == ESP_ERR_NVS_NOT_FOUND) return ESP_OK;
    if (error != ESP_OK) return error;
    size_t size = sizeof(candidate);
    error = nvs_get_blob(handle, RESET_KEY, &candidate, &size);
    nvs_close(handle);
    if (error == ESP_ERR_NVS_NOT_FOUND) { memset(&s_intent, 0, sizeof(candidate)); return ESP_OK; }
    if (error == ESP_ERR_NVS_INVALID_LENGTH || error == ESP_ERR_NVS_TYPE_MISMATCH)
        return discard_invalid_intent();
    if (error != ESP_OK) return error;
    if (size != sizeof(candidate) || candidate.version != 1 || candidate.card_present > 1 ||
        candidate.phase < 1 || candidate.phase > RESET_LOCAL_SETTINGS)
        return discard_invalid_intent();
    /* Version-1 TF/radio phases are still valid reset requests. Resume by
     * wiping local settings; never inspect or modify their recorded TF card. */
    s_intent = candidate;
    return ESP_OK;
}

#define STORAGE_ERASE_BLOCK (64U * 1024U)
#define STORAGE_READ_CHUNK 4096U

/* A 64 KiB erase takes about 250 ms, and the partition is mostly blank, so
 * erasing all of it added about 14 s to every reset. Erase only the blocks
 * that do not already read as erased; a block that cannot be read is erased.
 * Either way every byte of the partition reads 0xFF afterwards. */
static esp_err_t erase_storage(const esp_partition_t *storage)
{
    uint32_t *chunk = heap_caps_malloc(STORAGE_READ_CHUNK, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    if (!chunk) return esp_partition_erase_range(storage, 0, storage->size);
    esp_err_t error = ESP_OK;
    for (size_t block = 0; error == ESP_OK && block < storage->size; block += STORAGE_ERASE_BLOCK) {
        size_t length = storage->size - block;
        if (length > STORAGE_ERASE_BLOCK) length = STORAGE_ERASE_BLOCK;
        bool blank = true;
        for (size_t offset = 0; blank && offset < length; offset += STORAGE_READ_CHUNK) {
            size_t size = length - offset;
            if (size > STORAGE_READ_CHUNK) size = STORAGE_READ_CHUNK;
            if (esp_partition_read(storage, block + offset, chunk, size) != ESP_OK) {
                blank = false;
                break;
            }
            for (size_t word = 0; blank && word < size / sizeof(*chunk); ++word)
                blank = chunk[word] == UINT32_MAX;
        }
        if (!blank) error = esp_partition_erase_range(storage, block, length);
    }
    heap_caps_free(chunk);
    return error;
}

static esp_err_t erase_configuration(void)
{
    /* No settings readers or network workers have started in this boot. */
    esp_err_t error = erase_settings();
    if (error != ESP_OK) return error;
    const esp_partition_t *storage = esp_partition_find_first(
        ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_ANY, "storage");
    if (!storage) return ESP_ERR_NOT_FOUND;
    return erase_storage(storage);
}

esp_err_t jetkvm_factory_reset_boot_begin(void (*show_progress)(void))
{
    esp_err_t error = load_intent();
    atomic_store(&s_state, error != ESP_OK ? RESET_FAILED :
                 s_intent.phase ? RESET_PENDING : RESET_IDLE);
    if (!jetkvm_factory_reset_pending()) return error;
    if (show_progress) show_progress();
    if (error == ESP_OK) error = erase_configuration();
    if (error != ESP_OK) atomic_store(&s_state, RESET_FAILED);
    return error;
}

esp_err_t jetkvm_factory_reset_finish(esp_err_t error)
{
    if (!jetkvm_factory_reset_pending()) return error;
    if (error == ESP_OK && atomic_load(&s_state) == RESET_FAILED)
        return ESP_ERR_INVALID_STATE; /* Only another boot may retry cleanup. */
    if (error == ESP_OK) error = clear_intent();
    atomic_store(&s_state, error == ESP_OK ? RESET_IDLE : RESET_FAILED);
    return error;
}

static void restart_worker(void *arg)
{
    (void)arg;
    uint32_t command = ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
    if (command == 1) {
        vTaskDelay(pdMS_TO_TICKS(1000) + 1);
        esp_restart();
    }
    portENTER_CRITICAL(&s_lock);
    s_busy = false;
    portEXIT_CRITICAL(&s_lock);
    vTaskDeleteWithCaps(NULL);
}

void jetkvm_factory_reset_dispatch_complete(void)
{
    portENTER_CRITICAL(&s_lock);
    TaskHandle_t task = s_restart_task;
    s_restart_task = NULL;
    portEXIT_CRITICAL(&s_lock);
    if (task) xTaskNotify(task, 1, eSetValueWithOverwrite);
}

const char *jetkvm_factory_reset_request(void *ctx)
{
    (void)ctx;
    portENTER_CRITICAL(&s_lock);
    bool available = !s_busy;
    if (available) s_busy = true;
    portEXIT_CRITICAL(&s_lock);
    if (!available) return "Factory reset already pending";
    const char *error = NULL;
    TaskHandle_t task = NULL;
    reset_intent_t next = {.version = 1, .phase = RESET_LOCAL_SETTINGS};
    if (s_intent.phase) {
        error = "Factory reset is incomplete; reboot to retry";
    } else if (xTaskCreateWithCaps(restart_worker, "jk_reset", 4096, NULL, 4,
                   &task, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT) != pdPASS) {
        error = "Not enough memory to schedule factory reset";
    } else {
        if (save_intent(&next) != ESP_OK) error = "Could not save factory reset request";
        else {
            s_intent = next;
            atomic_store(&s_state, RESET_PENDING);
            portENTER_CRITICAL(&s_lock);
            s_restart_task = task;
            portEXIT_CRITICAL(&s_lock);
        }
    }
    if (task) {
        /* Successful requests wait for the peer's response/event sends. A
         * cancelled/notification-only reply releases the wait on cleanup. */
        if (error) xTaskNotify(task, 2, eSetValueWithOverwrite);
    } else {
        portENTER_CRITICAL(&s_lock);
        s_busy = false;
        portEXIT_CRITICAL(&s_lock);
    }
    return error;
}
