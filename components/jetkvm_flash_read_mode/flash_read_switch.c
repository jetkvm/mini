/*
 * The change of the cache controller's flash read from DIO to QIO. linker.lf
 * puts this whole file in internal RAM: while the registers change, the CPU
 * must not fetch a line of code from flash. The file calls only functions in
 * internal RAM or in the ROM, and has no data outside the stack.
 */
#include "flash_read_switch.h"

#include "esp_private/cache_utils.h"
#include "soc/soc.h"
#include "soc/spi_mem_c_reg.h"

/* The bits of CTRL that select the read command's lines. */
#define READ_MODE_BITS (SPI_MEM_C_FREAD_QIO | SPI_MEM_C_FREAD_QUAD | SPI_MEM_C_FREAD_DIO | \
                        SPI_MEM_C_FREAD_DUAL | SPI_MEM_C_FASTRD_MODE | SPI_MEM_C_FCMD_QUAD)
#define DUAL_MODE_BITS (SPI_MEM_C_FREAD_DIO | SPI_MEM_C_FASTRD_MODE)
#define QUAD_MODE_BITS (SPI_MEM_C_FREAD_QIO | SPI_MEM_C_FASTRD_MODE)

/* Fast Read Dual I/O as the ROM sets it for a flash clock of 80 MHz: command
 * BBh on one line, then 24 address bits and 4 mode bits on two lines, then 2
 * dummy cycles. */
#define DUAL_COMMAND 0xBBU
#define DUAL_ADDRESS_BITS 28U
#define DUAL_DUMMY_CYCLES 2U
/* Fast Read Quad I/O: command EBh on one line, then 24 address bits and 8
 * mode bits on four lines, then 4 dummy cycles. The mode bits count as
 * address bits for the controller, as in the DIO read. WB_MODE holds them
 * and stays 0: a W25Q128 keeps reading without a command (its continuous
 * read mode) only after mode bits whose bits 5 and 4 are 1 and 0. */
#define QUAD_COMMAND 0xEBU
#define QUAD_ADDRESS_BITS 32U
#define QUAD_DUMMY_CYCLES 4U
#define COMMAND_BITS 8U

#define FIELD(value, name) (((value) >> name##_S) & name##_V)
#define WITH_FIELD(value, name, field) \
    (((value) & ~((uint32_t)name##_V << name##_S)) | ((uint32_t)(field) << name##_S))

void flash_read_registers(flash_read_registers_t *out)
{
    out->ctrl = REG_READ(SPI_MEM_C_CTRL_REG);
    out->user = REG_READ(SPI_MEM_C_USER_REG);
    out->user1 = REG_READ(SPI_MEM_C_USER1_REG);
    out->user2 = REG_READ(SPI_MEM_C_USER2_REG);
    out->rd_status = REG_READ(SPI_MEM_C_RD_STATUS_REG);
    out->cache_fctrl = REG_READ(SPI_MEM_C_CACHE_FCTRL_REG);
}

/* The bits of CACHE_FCTRL that also select lines or the address length. The
 * change is made for a controller that selects them through CTRL alone. */
#define CACHE_READ_BITS (SPI_MEM_C_CACHE_USR_ADDR_4BYTE | SPI_MEM_C_FDIN_DUAL | \
                         SPI_MEM_C_FDOUT_DUAL | SPI_MEM_C_FADDR_DUAL | SPI_MEM_C_FDIN_QUAD | \
                         SPI_MEM_C_FDOUT_QUAD | SPI_MEM_C_FADDR_QUAD)

/* True for the one configuration from which the change is made. Any other
 * value of a field that takes part in the read leaves the controller as it
 * is. The length fields hold the number of bits or cycles minus 1. */
static bool rom_dual_read(const flash_read_registers_t *regs)
{
    return (regs->ctrl & READ_MODE_BITS) == DUAL_MODE_BITS &&
           (regs->user & SPI_MEM_C_USR_DUMMY) != 0U &&
           FIELD(regs->user1, SPI_MEM_C_USR_ADDR_BITLEN) == DUAL_ADDRESS_BITS - 1U &&
           FIELD(regs->user1, SPI_MEM_C_USR_DUMMY_CYCLELEN) == DUAL_DUMMY_CYCLES - 1U &&
           FIELD(regs->user2, SPI_MEM_C_USR_COMMAND_VALUE) == DUAL_COMMAND &&
           FIELD(regs->user2, SPI_MEM_C_USR_COMMAND_BITLEN) == COMMAND_BITS - 1U &&
           FIELD(regs->rd_status, SPI_MEM_C_WB_MODE) == 0U &&
           (regs->cache_fctrl & CACHE_READ_BITS) == 0U;
}

static void write_quad_read(const flash_read_registers_t *dual)
{
    uint32_t user1 = WITH_FIELD(dual->user1, SPI_MEM_C_USR_ADDR_BITLEN, QUAD_ADDRESS_BITS - 1U);
    user1 = WITH_FIELD(user1, SPI_MEM_C_USR_DUMMY_CYCLELEN, QUAD_DUMMY_CYCLES - 1U);
    REG_WRITE(SPI_MEM_C_USER1_REG, user1);
    REG_WRITE(SPI_MEM_C_USER2_REG,
              WITH_FIELD(dual->user2, SPI_MEM_C_USR_COMMAND_VALUE, QUAD_COMMAND));
    REG_WRITE(SPI_MEM_C_CTRL_REG, (dual->ctrl & ~READ_MODE_BITS) | QUAD_MODE_BITS);
}

flash_read_switch_result_t flash_read_switch_to_quad(void)
{
    flash_read_switch_result_t result = FLASH_READ_SWITCH_NOT_DUAL;
    spi_flash_disable_interrupts_caches_and_other_cpu();
    flash_read_registers_t dual;
    flash_read_registers(&dual);
    if (rom_dual_read(&dual)) {
        write_quad_read(&dual);
        result = FLASH_READ_SWITCH_QUAD;
    }
    spi_flash_enable_interrupts_caches_and_other_cpu();
    return result;
}
