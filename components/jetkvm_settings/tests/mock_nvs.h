#pragma once

#include <stddef.h>
#include <stdint.h>

void mock_nvs_reset(void);
void mock_nvs_fail_next_commit(void);
void mock_nvs_corrupt_byte(size_t offset, uint8_t xor_mask);
void mock_nvs_replace_committed(const void *data, size_t size);
size_t mock_nvs_committed_size(void);
const uint8_t *mock_nvs_committed_data(void);
unsigned mock_nvs_commit_count(void);
