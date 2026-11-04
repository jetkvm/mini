#pragma once

#include <stdbool.h>
#include <stddef.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

#define JETKVM_WOL_MAX_DEVICES 16U
#define JETKVM_WOL_NAME_CAPACITY 64U
#define JETKVM_WOL_MAC_CAPACITY 18U
#define JETKVM_WOL_BROADCAST_CAPACITY 16U

typedef struct {
    char name[JETKVM_WOL_NAME_CAPACITY];
    char mac_address[JETKVM_WOL_MAC_CAPACITY];
    char broadcast_ip[JETKVM_WOL_BROADCAST_CAPACITY];
} jetkvm_wol_device_t;

typedef struct {
    size_t count;
    jetkvm_wol_device_t devices[JETKVM_WOL_MAX_DEVICES];
} jetkvm_wol_device_list_t;

/** Load the CRC-protected NVS list, recovering to an empty list if corrupt. */
esp_err_t jetkvm_wol_init(void);
bool jetkvm_wol_is_initialized(void);
esp_err_t jetkvm_wol_get_devices(jetkvm_wol_device_list_t *list);
/** Validate and persist before publishing the replacement list. */
esp_err_t jetkvm_wol_set_devices(const jetkvm_wol_device_list_t *list);

#ifdef __cplusplus
}
#endif
