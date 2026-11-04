#pragma once

#include <stdbool.h>
#include <stdint.h>

/* The registers of the cache controller (SPI0) that hold its flash read. */
typedef struct {
    uint32_t ctrl;
    uint32_t user;
    uint32_t user1;
    uint32_t user2;
    uint32_t rd_status;
    uint32_t cache_fctrl;
} flash_read_registers_t;

void flash_read_registers(flash_read_registers_t *out);

typedef enum {
    /* The cache controller reads in QIO mode. */
    FLASH_READ_SWITCH_QUAD,
    /* The controller did not have the DIO configuration of the ROM, and is
     * left as it was. */
    FLASH_READ_SWITCH_NOT_DUAL,
} flash_read_switch_result_t;

/* Stops the other core and all interrupts that are not in internal RAM, and
 * suspends the cache, while it changes the controller's read. Runs from
 * internal RAM. */
flash_read_switch_result_t flash_read_switch_to_quad(void);
