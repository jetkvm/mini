#pragma once

#ifdef __cplusplus
extern "C" {
#endif

/*
 * The CPU runs code from flash through the L2 cache, and the cache fetches
 * each line with the read command that the cache controller (SPI0) holds.
 * The ROM sets that to Fast Read Dual I/O (DIO), and the second-stage
 * bootloader of every Mini keeps it. A line of 64 bytes takes 256 clock
 * cycles of data on two lines and 128 on four.
 *
 * jetkvm_flash_read_mode_start() switches the cache controller to Fast Read
 * Quad I/O (QIO) for this run when all of these hold:
 * - the controller has the DIO configuration of the ROM;
 * - the flash chip reports its quad enable bit set;
 * - the run before this one did not end in a panic or a watchdog reset while
 *   it read in QIO mode.
 * Otherwise the controller keeps the configuration it had. Every Mini board
 * connects all four data lines of the flash chip.
 * The function sends the flash chip no command but reads of its ID and its
 * status, and writes nothing to it. Every reset returns the controller to
 * DIO, so the mode lasts for one run and the application sets it again.
 *
 * Call it once, at the start of app_main, before other tasks use the cache
 * functions.
 */
void jetkvm_flash_read_mode_start(void);

typedef struct {
    /* How the cache controller reads flash now: "qio", "qout", "dio",
     * "dout", "fast" or "slow". */
    const char *cache_read_mode;
    /* What jetkvm_flash_read_mode_start() did: "on", or why it left the
     * controller as it was. */
    const char *quad_read;
} jetkvm_flash_read_mode_t;

void jetkvm_flash_read_mode_get(jetkvm_flash_read_mode_t *out);

#ifdef __cplusplus
}
#endif
