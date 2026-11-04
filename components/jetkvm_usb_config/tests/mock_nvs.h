#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef enum {
    MOCK_USB_SLOT_A = 0,
    MOCK_USB_SLOT_B = 1,
} mock_usb_slot_t;

void mock_nvs_reset(void);
void mock_nvs_fail_next_open(void);
void mock_nvs_fail_next_set(void);
void mock_nvs_fail_next_commit(void);
void mock_nvs_fail_readback_after_commit(void);
void mock_nvs_corrupt_after_next_commit(size_t offset, uint8_t xor_mask);
void mock_nvs_corrupt_committed(mock_usb_slot_t slot, size_t offset,
                                uint8_t xor_mask);
void mock_nvs_replace_committed(mock_usb_slot_t slot, const void *data,
                                size_t size);
void mock_nvs_remove_committed(mock_usb_slot_t slot);
const uint8_t *mock_nvs_committed_data(mock_usb_slot_t slot);
size_t mock_nvs_committed_size(mock_usb_slot_t slot);
bool mock_nvs_committed_present(mock_usb_slot_t slot);
unsigned mock_nvs_set_count(void);
unsigned mock_nvs_commit_count(void);
const char *mock_nvs_last_namespace(void);
const char *mock_nvs_last_set_key(void);
