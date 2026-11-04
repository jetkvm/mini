#include "jetkvm_flash_read_mode.h"

#include <inttypes.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_attr.h"
#include "esp_err.h"
#include "esp_flash.h"
#include "esp_log.h"
#include "esp_system.h"
#include "soc/spi_mem_c_reg.h"

#include "flash_read_switch.h"

/* ESP-IDF has the function without a declaration in a header. It reads the
 * status register of the chip and writes nothing. */
esp_err_t esp_flash_get_io_mode(esp_flash_t *chip, bool *qe);

static const char *TAG = "flash_read";

/* Set while a run reads flash in QIO mode, from before the change of the
 * controller, so that it also covers a reset that the change itself causes.
 * RTC memory keeps it over every reset but power-on. */
#define QUAD_RUN 0x51494F31U
static RTC_NOINIT_ATTR volatile uint32_t s_run;

/* Writes the word and waits until the write is done, so that it is in RTC
 * memory before the cache controller changes, and so that the compiler does
 * not move it past the change. */
static void set_run(uint32_t value)
{
    s_run = value;
    __asm__ volatile("" ::: "memory");
    __sync_synchronize();
}

static const char *s_quad_read = "not started";

static const char *mode_name(uint32_t ctrl)
{
    if ((ctrl & SPI_MEM_C_FREAD_QIO) != 0U) {
        return "qio";
    }
    if ((ctrl & SPI_MEM_C_FREAD_QUAD) != 0U) {
        return "qout";
    }
    if ((ctrl & SPI_MEM_C_FREAD_DIO) != 0U) {
        return "dio";
    }
    if ((ctrl & SPI_MEM_C_FREAD_DUAL) != 0U) {
        return "dout";
    }
    return (ctrl & SPI_MEM_C_FASTRD_MODE) != 0U ? "fast" : "slow";
}

static bool quad(uint32_t ctrl)
{
    return (ctrl & (SPI_MEM_C_FREAD_QIO | SPI_MEM_C_FREAD_QUAD)) != 0U;
}

/* A reset that follows a fault of the firmware. A Mini whose quad reads go
 * wrong after the check at the start ends its run with one of these. */
static bool fault_reset(esp_reset_reason_t reason)
{
    return reason == ESP_RST_PANIC || reason == ESP_RST_INT_WDT ||
           reason == ESP_RST_TASK_WDT || reason == ESP_RST_WDT ||
           reason == ESP_RST_CPU_LOCKUP;
}

static const char *switch_to_quad(void)
{
    bool quad_enable = false;
    if (esp_flash_get_io_mode(NULL, &quad_enable) != ESP_OK) {
        return "flash status not readable";
    }
    if (!quad_enable) {
        return "quad enable bit is clear";
    }
    set_run(QUAD_RUN);
    return flash_read_switch_to_quad() == FLASH_READ_SWITCH_QUAD ?
        "on" : "controller not in the DIO configuration of the ROM";
}

static void log_registers(const char *when, const flash_read_registers_t *regs)
{
    ESP_LOGI(TAG, "cache controller %s: ctrl=%08" PRIx32 " user=%08" PRIx32 " user1=%08" PRIx32
             " user2=%08" PRIx32 " rd_status=%08" PRIx32 " cache_fctrl=%08" PRIx32,
             when, regs->ctrl, regs->user, regs->user1, regs->user2, regs->rd_status,
             regs->cache_fctrl);
}

void jetkvm_flash_read_mode_start(void)
{
    const bool last_run_quad = s_run == QUAD_RUN;
    set_run(0U);
    uint32_t id = 0;
    (void)esp_flash_read_id(NULL, &id);
    flash_read_registers_t found;
    flash_read_registers(&found);
    flash_read_registers_t now = found;
    if (quad(found.ctrl)) {
        s_quad_read = "set before the application";
    } else if (last_run_quad && fault_reset(esp_reset_reason())) {
        s_quad_read = "the last run in QIO mode ended in a panic or a watchdog reset";
    } else {
        s_quad_read = switch_to_quad();
        flash_read_registers(&now);
    }
    if (!quad(now.ctrl)) {
        set_run(0U);
    }
    ESP_LOGI(TAG, "flash %06" PRIx32 ": the cache reads flash in %s mode (quad read: %s)",
             id, mode_name(now.ctrl), s_quad_read);
    /* The values that decide whether the change is made, for a Mini that
     * reports another configuration than the ROM's DIO read. */
    log_registers("as found", &found);
    if (now.ctrl != found.ctrl) {
        log_registers("as set", &now);
    }
}

void jetkvm_flash_read_mode_get(jetkvm_flash_read_mode_t *out)
{
    flash_read_registers_t regs;
    flash_read_registers(&regs);
    out->cache_read_mode = mode_name(regs.ctrl);
    out->quad_read = s_quad_read;
}
