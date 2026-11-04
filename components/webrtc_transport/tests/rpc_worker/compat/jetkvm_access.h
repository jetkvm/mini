#pragma once
#include <stdbool.h>
#include <stdint.h>
bool jetkvm_access_lock_epoch(uint32_t epoch);
void jetkvm_access_unlock(void);
