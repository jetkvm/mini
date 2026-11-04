/* The fields of ESP-IDF's soc/spi_mem_c_reg.h (ESP32-P4 revision 3) that the
 * component uses, with register addresses as offsets into the test's array. */
#pragma once

#include "soc/soc.h"

#define SPI_MEM_C_CTRL_REG 0x08U
#define SPI_MEM_C_FCMD_QUAD (BIT(8))
#define SPI_MEM_C_FASTRD_MODE (BIT(13))
#define SPI_MEM_C_FREAD_DUAL (BIT(14))
#define SPI_MEM_C_FREAD_QUAD (BIT(20))
#define SPI_MEM_C_FREAD_DIO (BIT(23))
#define SPI_MEM_C_FREAD_QIO (BIT(24))

#define SPI_MEM_C_USER_REG 0x18U
#define SPI_MEM_C_USR_DUMMY (BIT(29))

#define SPI_MEM_C_USER1_REG 0x1cU
#define SPI_MEM_C_USR_DUMMY_CYCLELEN_V 0x0000003FU
#define SPI_MEM_C_USR_DUMMY_CYCLELEN_S 0
#define SPI_MEM_C_USR_ADDR_BITLEN_V 0x0000003FU
#define SPI_MEM_C_USR_ADDR_BITLEN_S 26

#define SPI_MEM_C_USER2_REG 0x20U
#define SPI_MEM_C_USR_COMMAND_VALUE_V 0x0000FFFFU
#define SPI_MEM_C_USR_COMMAND_VALUE_S 0
#define SPI_MEM_C_USR_COMMAND_BITLEN_V 0x0000000FU
#define SPI_MEM_C_USR_COMMAND_BITLEN_S 28

#define SPI_MEM_C_RD_STATUS_REG 0x2cU
#define SPI_MEM_C_WB_MODE_V 0x000000FFU
#define SPI_MEM_C_WB_MODE_S 16

#define SPI_MEM_C_CACHE_FCTRL_REG 0x3cU
#define SPI_MEM_C_CACHE_USR_ADDR_4BYTE (BIT(1))
#define SPI_MEM_C_FDIN_DUAL (BIT(3))
#define SPI_MEM_C_FDOUT_DUAL (BIT(4))
#define SPI_MEM_C_FADDR_DUAL (BIT(5))
#define SPI_MEM_C_FDIN_QUAD (BIT(6))
#define SPI_MEM_C_FDOUT_QUAD (BIT(7))
#define SPI_MEM_C_FADDR_QUAD (BIT(8))
