#pragma once

#include <stddef.h>
#include <stdint.h>

void mock_nvs_reset(void);
void mock_nvs_fail_next_commit(void);
void mock_nvs_corrupt_byte(size_t offset, uint8_t xor_mask);
size_t mock_nvs_committed_size(void);

