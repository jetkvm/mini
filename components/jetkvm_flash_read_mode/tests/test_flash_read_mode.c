/*
 * Runs the production code of the component against a model of the cache
 * controller and a flash chip. The model says whether the chip answers the
 * read that the controller's registers hold: the DIO read of the ROM, or a
 * QIO read (EBh, 24 address and 8 mode bits, 4 dummy cycles), and no other
 * register values. It refuses a register write while the cache is on or the
 * other core runs.
 */
#include <setjmp.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "esp_flash.h"
#include "esp_system.h"
#include "soc/spi_mem_c_reg.h"

#include "flash_read_switch.h"
#include "jetkvm_flash_read_mode.h"

/* The registers as the ROM leaves them for a DIO read at 80 MHz. */
#define ROM_CTRL (BIT(31) | BIT(21) | BIT(19) | BIT(18) | BIT(3) | BIT(2) | \
                  SPI_MEM_C_FREAD_DIO | SPI_MEM_C_FASTRD_MODE)
#define ROM_USER (BIT(30) | SPI_MEM_C_USR_DUMMY | BIT(28))
#define ROM_USER1 ((27U << SPI_MEM_C_USR_ADDR_BITLEN_S) | BIT(6) | 1U)
#define ROM_USER2 ((7U << SPI_MEM_C_USR_COMMAND_BITLEN_S) | 0xBBU)
#define QUAD_CTRL ((ROM_CTRL & ~SPI_MEM_C_FREAD_DIO) | SPI_MEM_C_FREAD_QIO)

static uint32_t s_regs[0x40 / 4];

static struct {
    bool quad_enable;
    esp_reset_reason_t reset_reason;
    bool cores_stopped;
    bool cache_on;
    unsigned register_writes;
    /* The chip resets at the write that selects the QIO read. */
    bool reset_on_quad_write;
    jmp_buf reset;
} s;

static int s_failures;

#define CHECK(condition) \
    do { \
        if (!(condition)) { \
            printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #condition); \
            s_failures++; \
        } \
    } while (0)

typedef enum { FETCH_RIGHT, FETCH_WRONG } fetch_t;

static uint32_t field(uint32_t value, uint32_t mask, unsigned shift)
{
    return (value >> shift) & mask;
}

static fetch_t fetch_now(void)
{
    const uint32_t mode = s_regs[SPI_MEM_C_CTRL_REG / 4] &
        (SPI_MEM_C_FREAD_QIO | SPI_MEM_C_FREAD_QUAD | SPI_MEM_C_FREAD_DIO |
         SPI_MEM_C_FREAD_DUAL | SPI_MEM_C_FASTRD_MODE | SPI_MEM_C_FCMD_QUAD);
    const uint32_t user1 = s_regs[SPI_MEM_C_USER1_REG / 4];
    const uint32_t user2 = s_regs[SPI_MEM_C_USER2_REG / 4];
    const uint32_t address_bits =
        field(user1, SPI_MEM_C_USR_ADDR_BITLEN_V, SPI_MEM_C_USR_ADDR_BITLEN_S) + 1U;
    const uint32_t dummy_cycles =
        field(user1, SPI_MEM_C_USR_DUMMY_CYCLELEN_V, SPI_MEM_C_USR_DUMMY_CYCLELEN_S) + 1U;
    const uint32_t command =
        field(user2, SPI_MEM_C_USR_COMMAND_VALUE_V, SPI_MEM_C_USR_COMMAND_VALUE_S);
    const bool one_command_byte =
        field(user2, SPI_MEM_C_USR_COMMAND_BITLEN_V, SPI_MEM_C_USR_COMMAND_BITLEN_S) == 7U &&
        (s_regs[SPI_MEM_C_USER_REG / 4] & SPI_MEM_C_USR_DUMMY) != 0U;
    if (!one_command_byte) {
        return FETCH_WRONG;
    }
    if (mode == (SPI_MEM_C_FREAD_DIO | SPI_MEM_C_FASTRD_MODE) && command == 0xBBU &&
        address_bits == 28U && dummy_cycles == 2U) {
        return FETCH_RIGHT;
    }
    if (mode == (SPI_MEM_C_FREAD_QIO | SPI_MEM_C_FASTRD_MODE) && command == 0xEBU &&
        address_bits == 32U && dummy_cycles == 4U) {
        return FETCH_RIGHT;
    }
    return FETCH_WRONG;
}

uint32_t test_reg_read(uint32_t address)
{
    return s_regs[address / 4U];
}

void test_reg_write(uint32_t address, uint32_t value)
{
    CHECK(s.cores_stopped && !s.cache_on);
    s_regs[address / 4U] = value;
    s.register_writes++;
    if (s.reset_on_quad_write && address == SPI_MEM_C_CTRL_REG &&
        (value & SPI_MEM_C_FREAD_QIO) != 0U) {
        longjmp(s.reset, 1);
    }
}

void spi_flash_disable_interrupts_caches_and_other_cpu(void)
{
    CHECK(!s.cores_stopped && s.cache_on);
    s.cores_stopped = true;
    s.cache_on = false;
}

void spi_flash_enable_interrupts_caches_and_other_cpu(void)
{
    CHECK(s.cores_stopped && !s.cache_on);
    s.cores_stopped = false;
    s.cache_on = true;
}

esp_err_t esp_flash_read_id(esp_flash_t *chip, uint32_t *out_id)
{
    (void)chip;
    *out_id = 0xEF4018U;
    return ESP_OK;
}

esp_err_t esp_flash_get_io_mode(esp_flash_t *chip, bool *qe);
esp_err_t esp_flash_get_io_mode(esp_flash_t *chip, bool *qe)
{
    (void)chip;
    *qe = s.quad_enable;
    return ESP_OK;
}

esp_reset_reason_t esp_reset_reason(void)
{
    return s.reset_reason;
}

/* A reset: the ROM and the bootloader set the DIO read. */
static void reset(esp_reset_reason_t reason)
{
    memset(s_regs, 0, sizeof(s_regs));
    s_regs[SPI_MEM_C_CTRL_REG / 4] = ROM_CTRL;
    s_regs[SPI_MEM_C_USER_REG / 4] = ROM_USER;
    s_regs[SPI_MEM_C_USER1_REG / 4] = ROM_USER1;
    s_regs[SPI_MEM_C_USER2_REG / 4] = ROM_USER2;
    s.reset_reason = reason;
    s.cores_stopped = false;
    s.cache_on = true;
    s.register_writes = 0;
    s.reset_on_quad_write = false;
}

static void board(bool quad_enable)
{
    s.quad_enable = quad_enable;
}

static bool mode_is(const char *cache_read_mode, const char *quad_read)
{
    jetkvm_flash_read_mode_t mode;
    jetkvm_flash_read_mode_get(&mode);
    if (strcmp(mode.cache_read_mode, cache_read_mode) != 0 ||
        strcmp(mode.quad_read, quad_read) != 0) {
        printf("mode \"%s\", quad read \"%s\"\n", mode.cache_read_mode, mode.quad_read);
        return false;
    }
    return true;
}

static bool controller_as_the_rom_left_it(void)
{
    return s_regs[SPI_MEM_C_CTRL_REG / 4] == ROM_CTRL &&
           s_regs[SPI_MEM_C_USER_REG / 4] == ROM_USER &&
           s_regs[SPI_MEM_C_USER1_REG / 4] == ROM_USER1 &&
           s_regs[SPI_MEM_C_USER2_REG / 4] == ROM_USER2;
}

static bool left_running(void)
{
    return !s.cores_stopped && s.cache_on;
}

static void test_board_with_four_lines_reads_in_quad_mode(void)
{
    board(true);
    reset(ESP_RST_POWERON);
    jetkvm_flash_read_mode_start();
    CHECK(mode_is("qio", "on"));
    CHECK(s_regs[SPI_MEM_C_CTRL_REG / 4] == QUAD_CTRL);
    CHECK(fetch_now() == FETCH_RIGHT);
    CHECK(left_running());
}

static void test_clear_quad_enable_bit_leaves_the_controller(void)
{
    board(false);
    reset(ESP_RST_POWERON);
    jetkvm_flash_read_mode_start();
    CHECK(mode_is("dio", "quad enable bit is clear"));
    CHECK(s.register_writes == 0);
    CHECK(controller_as_the_rom_left_it());
    CHECK(left_running());
}

static void test_other_dual_configuration_is_left_alone(void)
{
    /* Each is the ROM's DIO read with one field of the read at another
     * value: a 32-bit address; lines selected in the cache control register,
     * for the address and for the data; one dummy cycle more, as for a
     * faster flash clock; mode bits that are not 0; another command. */
    static const struct {
        uint32_t address;
        uint32_t value;
    } others[] = {
        { SPI_MEM_C_CACHE_FCTRL_REG, SPI_MEM_C_CACHE_USR_ADDR_4BYTE },
        { SPI_MEM_C_CACHE_FCTRL_REG, SPI_MEM_C_FADDR_DUAL },
        { SPI_MEM_C_CACHE_FCTRL_REG, SPI_MEM_C_FDIN_QUAD },
        { SPI_MEM_C_USER1_REG, ROM_USER1 + 1U },
        { SPI_MEM_C_RD_STATUS_REG, 0xA0U << SPI_MEM_C_WB_MODE_S },
        { SPI_MEM_C_USER2_REG, (ROM_USER2 & ~0xFFU) | 0x3BU },
    };
    board(true);
    for (size_t i = 0; i < sizeof(others) / sizeof(others[0]); ++i) {
        reset(ESP_RST_POWERON);
        s_regs[others[i].address / 4U] = others[i].value;
        jetkvm_flash_read_mode_start();
        CHECK(mode_is("dio", "controller not in the DIO configuration of the ROM"));
        CHECK(s.register_writes == 0);
        CHECK(left_running());
    }
}

static void test_quad_mode_of_the_bootloader_is_kept(void)
{
    board(true);
    reset(ESP_RST_POWERON);
    s_regs[SPI_MEM_C_CTRL_REG / 4] = QUAD_CTRL;
    jetkvm_flash_read_mode_start();
    CHECK(mode_is("qio", "set before the application"));
    CHECK(s.register_writes == 0);
}

static void test_fault_after_a_quad_run_holds_the_next_run_in_dual_mode(void)
{
    static const esp_reset_reason_t faults[] = {
        ESP_RST_PANIC, ESP_RST_INT_WDT, ESP_RST_TASK_WDT, ESP_RST_WDT, ESP_RST_CPU_LOCKUP,
    };
    board(true);
    for (size_t i = 0; i < sizeof(faults) / sizeof(faults[0]); ++i) {
        reset(ESP_RST_POWERON);
        jetkvm_flash_read_mode_start();
        CHECK(mode_is("qio", "on"));

        reset(faults[i]);
        jetkvm_flash_read_mode_start();
        CHECK(mode_is("dio", "the last run in QIO mode ended in a panic or a watchdog reset"));
        CHECK(s.register_writes == 0);

        /* The run in DIO mode ended in the same fault: the cause is not the
         * read mode, and the next run tries it again. */
        reset(faults[i]);
        jetkvm_flash_read_mode_start();
        CHECK(mode_is("qio", "on"));
    }
}

static void test_restart_and_power_loss_after_a_quad_run_keep_quad_mode(void)
{
    static const esp_reset_reason_t resets[] = {
        ESP_RST_SW, ESP_RST_POWERON, ESP_RST_EXT, ESP_RST_BROWNOUT,
    };
    board(true);
    reset(ESP_RST_POWERON);
    jetkvm_flash_read_mode_start();
    for (size_t i = 0; i < sizeof(resets) / sizeof(resets[0]); ++i) {
        reset(resets[i]);
        jetkvm_flash_read_mode_start();
        CHECK(mode_is("qio", "on"));
    }
}

static void test_reset_during_the_change_holds_the_next_run_in_dual_mode(void)
{
    board(true);
    reset(ESP_RST_POWERON);
    s.reset_on_quad_write = true;
    if (setjmp(s.reset) == 0) {
        jetkvm_flash_read_mode_start();
        CHECK(!"the model resets at the write that selects the QIO read");
    }
    reset(ESP_RST_INT_WDT);
    jetkvm_flash_read_mode_start();
    CHECK(mode_is("dio", "the last run in QIO mode ended in a panic or a watchdog reset"));
    CHECK(s.register_writes == 0);
}

int main(void)
{
    test_board_with_four_lines_reads_in_quad_mode();
    test_clear_quad_enable_bit_leaves_the_controller();
    test_other_dual_configuration_is_left_alone();
    test_quad_mode_of_the_bootloader_is_kept();
    test_fault_after_a_quad_run_holds_the_next_run_in_dual_mode();
    test_restart_and_power_loss_after_a_quad_run_keep_quad_mode();
    test_reset_during_the_change_holds_the_next_run_in_dual_mode();
    if (s_failures != 0) {
        printf("%d checks failed\n", s_failures);
        return 1;
    }
    printf("flash read mode tests passed\n");
    return 0;
}
