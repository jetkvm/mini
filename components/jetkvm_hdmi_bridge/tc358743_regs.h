/* TC358743 HDMI -> MIPI-CSI bridge: board wiring and register map.
 *
 * Component-private: only jetkvm_hdmi_bridge.c and the capture code in
 * jetkvm_video.c, which sequences a few register writes itself, include this
 * header.  It holds the bridge's I2C and GPIO wiring, its register
 * addresses, and their bit masks.  Do not renumber or "clean up" values
 * here; they describe fixed hardware. */

#ifndef TC358743_REGS_H
#define TC358743_REGS_H

#define TC358743_I2C_PORT           I2C_NUM_0
#define TC358743_I2C_SDA            2
#define TC358743_I2C_SCL            5
#define TC358743_RST                32
#define TC358743_INT                33
#define TC358743_I2C_ADDR           0x0f
#define TC358743_I2C_SPEED_HZ       (100 * 1000)

/* VDD_O3 powers both ESP32-P4 VDD_MIPI_DPHY and the board's 27 MHz
 * TC358743 reference oscillator. The oscillator accepts 1.8--3.3 V; 2.5 V
 * also supplies the P4 MIPI D-PHY at its required voltage. */
#define JETKVM_MIPI_LDO_CHAN_ID     3
#define JETKVM_MIPI_LDO_VOLTAGE_MV 2500


#define TC358743_LANES              2
#define TC358743_FIFO_LEVEL         374
#define TC358743_DDC5V_DELAY_100MS  0x02

#define TC358743_OUTPUT_RGB888      1
#define TC358743_USE_EXTCNT         0

#define CHIPID                      0x0000
#define MASK_CHIPID                 0xff00
#define MASK_REVID                  0x00ff

#define SYSCTL                      0x0002
#define MASK_IRRST                  0x0800
#define MASK_CECRST                 0x0400
#define MASK_CTXRST                 0x0200
#define MASK_HDMIRST                0x0100
#define MASK_SLEEP                  0x0001

#define CONFCTL                     0x0004
#define MASK_YCBCRFMT               0x00c0
#define MASK_YCBCRFMT_COLORBAR      0x0080
#define MASK_YCBCRFMT_422_8_BIT     0x00c0
#define MASK_ABUFEN                 0x0002
#define MASK_VBUFEN                 0x0001

#define FIFOCTL                     0x0006

#define INTSTATUS                   0x0014
#define MASK_HDMI_INT               0x0200
#define MASK_CSI_INT                0x0100
#define MASK_SYS_INT                0x0020
#define INTMASK                     0x0016
#define INTFLAG                     0x0018
#define INTSYSSTATUS                0x001a

#define PLLCTL0                     0x0020
#define SET_PLL_PRD(prd)            ((((prd) - 1) << 12) & 0xf000)
#define SET_PLL_FBD(fbd)            (((fbd) - 1) & 0x01ff)

#define PLLCTL1                     0x0022
#define SET_PLL_FRS(frs)            (((frs) << 10) & 0x0c00)
#define MASK_CKEN                   0x0010
#define MASK_RESETB                 0x0002
#define MASK_PLL_EN                 0x0001

#define CECHCLK                     0x0028
#define CECLCLK                     0x002a

#define CLW_CNTRL                   0x0140
#define MASK_CLW_LANEDISABLE        0x0001
#define D0W_CNTRL                   0x0144
#define MASK_D0W_LANEDISABLE        0x0001
#define D1W_CNTRL                   0x0148
#define MASK_D1W_LANEDISABLE        0x0001
#define D2W_CNTRL                   0x014c
#define MASK_D2W_LANEDISABLE        0x0001
#define D3W_CNTRL                   0x0150
#define MASK_D3W_LANEDISABLE        0x0001

#define STARTCNTRL                  0x0204
#define MASK_START                  0x00000001
#define LINEINITCNT                 0x0210
#define LPTXTIMECNT                 0x0214
#define TCLK_HEADERCNT              0x0218
#define TCLK_TRAILCNT               0x021c
#define THS_HEADERCNT               0x0220
#define TWAKEUP                     0x0224
#define TCLK_POSTCNT                0x0228
#define THS_TRAILCNT                0x022c
#define HSTXVREGCNT                 0x0230
#define HSTXVREGEN                  0x0234
#define MASK_CLM_HSTXVREGEN         0x0001
#define MASK_D0M_HSTXVREGEN         0x0002
#define MASK_D1M_HSTXVREGEN         0x0004
#define MASK_D2M_HSTXVREGEN         0x0008
#define MASK_D3M_HSTXVREGEN         0x0010

#define TXOPTIONCNTRL               0x0238
#define MASK_CONTCLKMODE            0x00000001

#define CSI_CONFW                   0x0500
#define CSI_STATUS                  0x0410
#define CSI_CONTROL                 0x040c
#define CSI_INT                     0x0414
#define CSI_INT_ENA                 0x0418
#define CSI_ERR                     0x044c
#define CSI_ERR_INTENA              0x0450
#define CSI_ERR_HALT                0x0454
#define MASK_S_WSYNC                0x0400
#define MASK_S_TXACT                0x0200
#define MASK_S_RXACT                0x0100
#define MASK_S_HLT                  0x0001
#define MASK_MODE_SET               0xa0000000
#define MASK_MODE_CLEAR             0xc0000000
#define MASK_ADDRESS_CSI_CONTROL    0x03000000
#define MASK_ADDRESS_CSI_INT_ENA    0x06000000
#define MASK_ADDRESS_CSI_ERR_INTENA 0x14000000
#define MASK_ADDRESS_CSI_ERR_HALT   0x15000000
#define MASK_CSI_MODE               0x8000
#define MASK_HTXTOEN                0x0400
#define MASK_TXHSMD                 0x0080
#define MASK_HSCKMD                 0x0020
#define MASK_NOL_1                  0x0000
#define MASK_NOL_2                  0x0002
#define MASK_EOTDIS                 0x0001
#define MASK_INTER                  0x00000004
#define MASK_INER                   0x00000200
#define MASK_WCER                   0x00000100
#define MASK_QUNK                   0x00000010
#define MASK_TXBRK                  0x00000002

#define CSI_INT_CLR                 0x050c
#define MASK_ICRER                  0x00000004
#define CSI_START                   0x0518
#define MASK_STRT                   0x00000001

#define HDMI_INT1                   0x8501
#define MASK_I_AUD                  0x10
#define MASK_I_CBIT                 0x08
#define MASK_I_CLK                  0x02
#define MASK_I_SYS                  0x01

#define SYS_INT                     0x8502
#define MASK_I_DVI                  0x20
#define MASK_I_HDMI                 0x10
#define MASK_I_DDC                  0x01

#define CLK_INT                     0x8503
#define MASK_I_OUT_H_CHG            0x40
#define MASK_I_IN_DE_CHG            0x20

#define CBIT_INT                    0x8505
#define MASK_I_AF_LOCK              0x80
#define MASK_I_AF_UNLOCK            0x40
#define MASK_I_CBIT_FS              0x02

#define AUDIO_INT                   0x8506
#define MASK_I_BUFINIT_END          0x01

#define MISC_INT                    0x850b
#define MASK_I_SYNC_CHG             0x02

#define KEY_INT                     0x850f

#define SYS_INTM                    0x8512
#define MASK_M_DVI_DET              0x20
#define MASK_M_HDMI_DET             0x10
#define MASK_M_DDC                  0x01

#define CLK_INTM                    0x8513
#define MASK_M_IN_DE_CHG            0x20

#define CBIT_INTM                   0x8515
#define MASK_M_AF_LOCK              0x80
#define MASK_M_AF_UNLOCK            0x40
#define MASK_M_CBIT_FS              0x02

#define AUDIO_INTM                  0x8516
#define MASK_M_BUFINIT_END          0x01

#define MISC_INTM                   0x851b
#define MASK_M_SYNC_CHG             0x02

#define SYS_STATUS                  0x8520
#define MASK_S_SYNC                 0x80
#define MASK_S_AVMUTE               0x40
#define MASK_S_HDMI                 0x10
#define MASK_S_PHY_SCDT             0x08
#define MASK_S_PHY_PLL              0x04
#define MASK_S_TMDS                 0x02
#define MASK_S_DDC5V                0x01

#define VI_STATUS1                  0x8522
#define MASK_S_DEEPCOLOR            0x0c
#define MASK_S_V_INTERLACE          0x01

#define VI_STATUS3                  0x8528
#define MASK_S_V_COLOR              0x1e
#define MASK_LIMITED                0x01

#define PHY_CTL0                    0x8531
#define MASK_PHY_SYSCLK_IND         0x02
#define PHY_CTL1                    0x8532
#define SET_PHY_AUTO_RST1_US(us)    ((((us) / 200) << 4) & 0xf0)
#define SET_FREQ_RANGE_MODE_CYCLES(cycles) (((cycles) - 1) & 0x03)
#define PHY_CTL2                    0x8533
#define MASK_PHY_AUTO_RST4          0x04
#define MASK_PHY_AUTO_RST3          0x02
#define MASK_PHY_AUTO_RST2          0x01
#define PHY_EN                      0x8534
#define MASK_ENABLE_PHY             0x01
#define PHY_RST                     0x8535
#define MASK_RESET_CTRL             0x01
#define PHY_BIAS                    0x8536
#define PHY_CSQ                     0x853f
#define SET_CSQ_CNT_LEVEL(n)        ((n) & 0x0f)

#define SYS_FREQ0                   0x8540
#define SYS_FREQ1                   0x8541
#define DDC_CTL                     0x8543
#define MASK_DDC5V_MODE             0x03
#define HPD_CTL                     0x8544
#define MASK_HPD_OUT0               0x01
#define ANA_CTL                     0x8545
#define MASK_APPL_PCSX_NORMAL       0x30
#define MASK_ANALOG_ON              0x01
#define AVM_CTL                     0x8546
#define HDMI_DET                    0x8552
#define MASK_HDMI_DET_V             0x30

#define HDCP_MODE                   0x8560
#define MASK_MANUAL_AUTHENTICATION  0x02

#define VI_MODE                     0x8570
#define MASK_RGB_DVI                0x08
#define VOUT_SET2                   0x8573
#define MASK_SEL422                 0x80
#define MASK_VOUT_422FIL_100        0x40
#define MASK_VOUTCOLORMODE          0x03
#define MASK_VOUTCOLORMODE_AUTO     0x01
#define VOUT_SET3                   0x8574
#define MASK_VOUT_EXTCNT            0x08
#define VI_REP                      0x8576
#define MASK_VOUT_COLOR_SEL         0xe0
#define MASK_VOUT_COLOR_RGB_FULL    0x00
#define MASK_VOUT_COLOR_601_YCBCR_LIMITED 0x60
#define VI_MUTE                     0x857f
#define MASK_AUTO_MUTE              0xc0
#define MASK_VI_MUTE                0x10
#define DE_WIDTH_H_LO               0x8582
#define DE_WIDTH_H_HI               0x8583
#define DE_WIDTH_V_LO               0x8588
#define DE_WIDTH_V_HI               0x8589
#define H_SIZE_LO                   0x858a
#define H_SIZE_HI                   0x858b
#define V_SIZE_LO                   0x858c
#define V_SIZE_HI                   0x858d
#define HV_RST                      0x85af
#define MASK_H_PI_RST               0x20
#define MASK_V_PI_RST               0x10

#define DE_WIDTH_H_LO               0x8582
#define DE_WIDTH_H_HI               0x8583
#define DE_WIDTH_V_LO               0x8588
#define DE_WIDTH_V_HI               0x8589
#define H_SIZE_LO                   0x858a
#define H_SIZE_HI                   0x858b
#define V_SIZE_LO                   0x858c
#define V_SIZE_HI                   0x858d
#define EDID_MODE                   0x85c7
#define MASK_EDID_MODE              0x03
#define MASK_EDID_MODE_E_DDC        0x02
#define EDID_LEN1                   0x85ca
#define EDID_LEN2                   0x85cb
#define FV_CNT_LO                   0x85a1
#define FV_CNT_HI                   0x85a2
#define FH_MIN0                     0x85aa
#define FH_MIN1                     0x85ab
#define FH_MAX0                     0x85ac
#define FH_MAX1                     0x85ad
#define EDID_RAM                    0x8c00

#define FS_SET                      0x8621
#define LOCKDET_REF0                0x8630
#define LOCKDET_REF1                0x8631
#define LOCKDET_REF2                0x8632
#define FORCE_MUTE                  0x8600
#define AUTO_CMD0                   0x8602
#define MASK_AUTO_MUTE7             0x80
#define MASK_AUTO_MUTE6             0x40
#define MASK_AUTO_MUTE5             0x20
#define MASK_AUTO_MUTE4             0x10
#define MASK_AUTO_MUTE1             0x02
#define MASK_AUTO_MUTE0             0x01
#define AUTO_CMD1                   0x8603
#define MASK_AUTO_MUTE9             0x02
#define AUTO_CMD2                   0x8604
#define MASK_AUTO_PLAY3             0x08
#define MASK_AUTO_PLAY2             0x04
#define BUFINIT_START               0x8606
#define SET_BUFINIT_START_MS(milliseconds) ((milliseconds) / 100)
#define FS_MUTE                     0x8607
#define FS_IMODE                    0x8620
#define MASK_NLPCM_SMODE            0x20
#define MASK_FS_SMODE               0x02
#define NCO_F0_MOD                  0x8670
#define MASK_NCO_F0_MOD             0x03
#define MASK_NCO_F0_MOD_27MHZ       0x01
#define ACR_MODE                    0x8640
#define MASK_CTS_MODE               0x01
#define ACR_MDF0                    0x8641
#define MASK_ACR_L2MDF_1976_PPM     0x60
#define MASK_ACR_L1MDF_976_PPM      0x05
#define ACR_MDF1                    0x8642
#define MASK_ACR_L3MDF_3906_PPM     0x07
#define SDO_MODE1                   0x8652
#define MASK_SDO_FMT_I2S            0x02
#define DIV_MODE                    0x8665
#define SET_DIV_DLY_MS(milliseconds) ((((milliseconds) / 100) << 4) & 0xf0)
#define PK_INT_MODE                 0x8709
#define MASK_ISRC2_INT_MODE         0x80
#define MASK_ISRC_INT_MODE          0x40
#define MASK_ACP_INT_MODE           0x20
#define MASK_VS_INT_MODE            0x10
#define MASK_SPD_INT_MODE           0x08
#define MASK_MS_INT_MODE            0x04
#define MASK_AUD_INT_MODE           0x02
#define MASK_AVI_INT_MODE           0x01
#define NO_PKT_LIMIT                0x870b
#define NO_PKT_CLR                  0x870c
#define ERR_PK_LIMIT                0x870d
#define NO_PKT_LIMIT2               0x870e
#define NO_GDB_LIMIT                0x9007

#endif /* TC358743_REGS_H */
