#pragma once
#include <stdint.h>
#define BIT(n) (1U << (n))
uint32_t test_reg_read(uint32_t address);
void test_reg_write(uint32_t address, uint32_t value);
#define REG_READ(address) test_reg_read(address)
#define REG_WRITE(address, value) test_reg_write((address), (value))
