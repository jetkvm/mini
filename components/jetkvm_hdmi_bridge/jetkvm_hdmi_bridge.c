/* TC358743 HDMI -> MIPI-CSI bridge driver.
 *
 * See include/jetkvm_hdmi_bridge.h for the ownership and concurrency
 * contract. Board wiring and register definitions live in tc358743_regs.h. */

#include "jetkvm_hdmi_bridge.h"
#include "tc358743_regs.h"

#include <inttypes.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "driver/gpio.h"
#include "esp_bit_defs.h"
#include "esp_check.h"
#include "esp_ldo_regulator.h"
#include "esp_log.h"
#include "esp_rom_sys.h"
#include "soc/dw_gdma_struct.h"
#include "soc/isp_struct.h"
#include "soc/mipi_csi_bridge_struct.h"
#include "soc/mipi_csi_host_struct.h"

#include "edid_presets_data.h"

_Static_assert(sizeof(s_edid_default) == 256,
               "the default EDID must contain two blocks");
_Static_assert(sizeof(s_edid_dell_d2721h) == 256,
               "the DELL D2721H EDID must contain two blocks");

/* Both presets list every mode the Mini captures, with 1920x1080p30
 * preferred; the host picks its resolution. DELL D2721H carries that
 * monitor's identity for hosts that expect a known display. */
static const p4_edid_descriptor_t s_edid_presets[] = {
    {
        .preset = JETKVM_VIDEO_EDID_PRESET_DEFAULT,
        .bytes = s_edid_default,
        .size = sizeof(s_edid_default),
        .label = "JetKVM Default",
    },
    {
        .preset = JETKVM_VIDEO_EDID_PRESET_DELL_D2721H,
        .bytes = s_edid_dell_d2721h,
        .size = sizeof(s_edid_dell_d2721h),
        .label = "DELL D2721H, 1920x1080",
    },
};

typedef struct {
    SemaphoreHandle_t transaction_lock;
    StaticSemaphore_t transaction_lock_storage;
    SemaphoreHandle_t completion;
    StaticSemaphore_t completion_storage;
    volatile bool ready;
    volatile bool in_flight;
    volatile bool command_pending;
    volatile uint32_t requested_preset;
    volatile uint32_t applied_preset;
    volatile esp_err_t result;
} p4_edid_control_t;

static p4_edid_control_t s_edid_control;
static esp_ldo_channel_handle_t s_refclk_ldo;

const p4_edid_descriptor_t *p4_edid_descriptor(
    jetkvm_video_edid_preset_t preset)
{
    for (size_t i = 0U; i < sizeof(s_edid_presets) /
                                  sizeof(s_edid_presets[0]); ++i) {
        if (s_edid_presets[i].preset == preset) {
            return &s_edid_presets[i];
        }
    }
    return NULL;
}

const p4_edid_descriptor_t *p4_edid_presets(size_t *count_out)
{
    if (count_out) {
        *count_out = sizeof(s_edid_presets) / sizeof(s_edid_presets[0]);
    }
    return s_edid_presets;
}

bool p4_edid_hex_matches(const char *hex,
                         const p4_edid_descriptor_t *descriptor)
{
    static const char digits[] = "0123456789abcdef";
    if (!hex || !descriptor || strlen(hex) != descriptor->size * 2U) {
        return false;
    }
    for (size_t i = 0U; i < descriptor->size; ++i) {
        const unsigned char high = (unsigned char)hex[i * 2U];
        const unsigned char low = (unsigned char)hex[i * 2U + 1U];
        const char expected_high = digits[descriptor->bytes[i] >> 4U];
        const char expected_low = digits[descriptor->bytes[i] & 0x0fU];
        if ((char)(high | 0x20U) != expected_high ||
            (char)(low | 0x20U) != expected_low) {
            return false;
        }
    }
    return true;
}

bool p4_edid_to_hex(const p4_edid_descriptor_t *descriptor,
                    char *output, size_t output_capacity)
{
    static const char digits[] = "0123456789ABCDEF";
    if (!descriptor || !output ||
        output_capacity < descriptor->size * 2U + 1U) {
        return false;
    }
    for (size_t i = 0U; i < descriptor->size; ++i) {
        output[i * 2U] = digits[descriptor->bytes[i] >> 4U];
        output[i * 2U + 1U] = digits[descriptor->bytes[i] & 0x0fU];
    }
    output[descriptor->size * 2U] = '\0';
    return true;
}

esp_err_t p4_edid_control_init(void)
{
    memset(&s_edid_control, 0, sizeof(s_edid_control));
    s_edid_control.transaction_lock = xSemaphoreCreateMutexStatic(
        &s_edid_control.transaction_lock_storage);
    s_edid_control.completion = xSemaphoreCreateBinaryStatic(
        &s_edid_control.completion_storage);
    if (!s_edid_control.transaction_lock || !s_edid_control.completion) {
        return ESP_ERR_NO_MEM;
    }
    s_edid_control.result = ESP_ERR_INVALID_STATE;
    return ESP_OK;
}

bool p4_edid_control_lock(uint32_t timeout_ms)
{
    if (!s_edid_control.transaction_lock) {
        return false;
    }
    return xSemaphoreTake(s_edid_control.transaction_lock,
                          pdMS_TO_TICKS(timeout_ms)) == pdTRUE;
}

void p4_edid_control_unlock(void)
{
    xSemaphoreGive(s_edid_control.transaction_lock);
}

bool p4_edid_control_pending(void)
{
    return __atomic_load_n(&s_edid_control.command_pending, __ATOMIC_ACQUIRE);
}

bool p4_edid_control_ready(void)
{
    return s_edid_control.transaction_lock &&
           __atomic_load_n(&s_edid_control.ready, __ATOMIC_ACQUIRE);
}

jetkvm_video_edid_preset_t p4_edid_control_applied_preset(void)
{
    return (jetkvm_video_edid_preset_t)__atomic_load_n(
        &s_edid_control.applied_preset, __ATOMIC_ACQUIRE);
}

bool p4_edid_control_mark_ready(jetkvm_video_edid_preset_t preset)
{
    if (!s_edid_control.transaction_lock || !s_edid_control.completion) {
        return false;
    }
    __atomic_store_n(&s_edid_control.applied_preset, (uint32_t)preset,
                     __ATOMIC_RELAXED);
    __atomic_store_n(&s_edid_control.ready, true, __ATOMIC_RELEASE);
    return true;
}

static p4_edid_apply_result_fn s_edid_apply_result_fn;
static void *s_edid_apply_result_ctx;

void p4_edid_set_apply_result_handler(p4_edid_apply_result_fn handler, void *ctx)
{
    s_edid_apply_result_fn = handler;
    s_edid_apply_result_ctx = ctx;
}

esp_err_t p4_edid_submit_async_locked(jetkvm_video_edid_preset_t preset)
{
    if (!__atomic_load_n(&s_edid_control.ready, __ATOMIC_ACQUIRE)) {
        return ESP_ERR_INVALID_STATE;
    }
    if (__atomic_load_n(&s_edid_control.in_flight, __ATOMIC_ACQUIRE)) {
        /* An apply is running: publish the newer preset and re-arm the
         * mailbox so the owner applies it next.  The UI may change the
         * preset again within the apply window (upstream never refuses). */
        __atomic_store_n(&s_edid_control.requested_preset, (uint32_t)preset,
                         __ATOMIC_RELAXED);
        __atomic_store_n(&s_edid_control.command_pending, true,
                         __ATOMIC_RELEASE);
        return ESP_OK;
    }
    while (xSemaphoreTake(s_edid_control.completion, 0) == pdTRUE) {
    }
    __atomic_store_n(&s_edid_control.requested_preset, (uint32_t)preset,
                     __ATOMIC_RELAXED);
    __atomic_store_n(&s_edid_control.result, ESP_ERR_INVALID_STATE,
                     __ATOMIC_RELAXED);
    __atomic_store_n(&s_edid_control.in_flight, true, __ATOMIC_RELAXED);
    __atomic_store_n(&s_edid_control.command_pending, true,
                     __ATOMIC_RELEASE);
    return ESP_OK;
}
void tc_gpio_init(void)
{
    gpio_config_t rst_cfg = {
        .pin_bit_mask = 1ULL << TC358743_RST,
        .mode = GPIO_MODE_INPUT_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    gpio_config_t int_cfg = {
        .pin_bit_mask = 1ULL << TC358743_INT,
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    ESP_ERROR_CHECK(gpio_config(&rst_cfg));
    ESP_ERROR_CHECK(gpio_config(&int_cfg));

    /*
     * The board powers the external 27 MHz oscillator from ESP32-P4 VDD_O3.
     * Keep RESETN asserted while LDO3 and the oscillator settle; the bridge
     * requires REFCLK before it can answer its control interface.
     */
    ESP_ERROR_CHECK(gpio_set_level(TC358743_RST, 0));
    if (s_refclk_ldo == NULL) {
        const esp_ldo_channel_config_t ldo_config = {
            .chan_id = JETKVM_MIPI_LDO_CHAN_ID,
            .voltage_mv = JETKVM_MIPI_LDO_VOLTAGE_MV,
        };
        ESP_ERROR_CHECK(esp_ldo_acquire_channel(&ldo_config, &s_refclk_ldo));
        vTaskDelay(pdMS_TO_TICKS(10));
    }
}

void tc_hw_reset(jetkvm_hdmi_bridge_t *ctx)
{
    ESP_LOGI(ctx->log_tag, "TC reference clock supply ready: LDO_VO3=%d mV",
             JETKVM_MIPI_LDO_VOLTAGE_MV);
    ESP_LOGI(ctx->log_tag, "reset bridge rst=%d int=%d", TC358743_RST, TC358743_INT);
    ESP_ERROR_CHECK(gpio_set_level(TC358743_RST, 0));
    vTaskDelay(pdMS_TO_TICKS(20));
    ESP_LOGI(ctx->log_tag, "bridge reset asserted: rst_level=%d int_level=%d",
             gpio_get_level(TC358743_RST), gpio_get_level(TC358743_INT));
    ESP_ERROR_CHECK(gpio_set_level(TC358743_RST, 1));
    vTaskDelay(pdMS_TO_TICKS(50));
    ESP_LOGI(ctx->log_tag, "bridge reset released: rst_level=%d int_level=%d",
             gpio_get_level(TC358743_RST), gpio_get_level(TC358743_INT));
}

void tc_i2c_init(jetkvm_hdmi_bridge_t *ctx)
{
    i2c_master_bus_config_t i2c_bus_config = {
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .i2c_port = TC358743_I2C_PORT,
        .sda_io_num = TC358743_I2C_SDA,
        .scl_io_num = TC358743_I2C_SCL,
        .glitch_ignore_cnt = 7,
        .flags.enable_internal_pullup = true,
    };
    ESP_ERROR_CHECK(i2c_new_master_bus(&i2c_bus_config, &ctx->i2c_bus));
}

static esp_err_t tc_read_bytes(jetkvm_hdmi_bridge_t *ctx, uint16_t reg, uint8_t *data, size_t len)
{
    i2c_device_config_t dev_cfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = TC358743_I2C_ADDR,
        .scl_speed_hz = TC358743_I2C_SPEED_HZ,
    };
    i2c_master_dev_handle_t dev = NULL;
    ESP_RETURN_ON_ERROR(i2c_master_bus_add_device(ctx->i2c_bus, &dev_cfg, &dev), ctx->log_tag, "add i2c dev failed");

    uint8_t addr[2] = { (uint8_t)(reg >> 8), (uint8_t)reg };
    esp_err_t ret = i2c_master_transmit_receive(dev, addr, sizeof(addr), data, len, 1000);
    i2c_master_bus_rm_device(dev);
    return ret;
}

static esp_err_t tc_write_bytes(jetkvm_hdmi_bridge_t *ctx, uint16_t reg, const uint8_t *data, size_t len)
{
    uint8_t buffer[130];
    ESP_RETURN_ON_FALSE(len <= sizeof(buffer) - 2, ESP_ERR_INVALID_ARG, ctx->log_tag, "write too large");

    i2c_device_config_t dev_cfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = TC358743_I2C_ADDR,
        .scl_speed_hz = TC358743_I2C_SPEED_HZ,
    };
    i2c_master_dev_handle_t dev = NULL;
    ESP_RETURN_ON_ERROR(i2c_master_bus_add_device(ctx->i2c_bus, &dev_cfg, &dev), ctx->log_tag, "add i2c dev failed");

    buffer[0] = (uint8_t)(reg >> 8);
    buffer[1] = (uint8_t)reg;
    if (len) {
        memcpy(&buffer[2], data, len);
    }
    esp_err_t ret = i2c_master_transmit(dev, buffer, len + 2, 1000);
    i2c_master_bus_rm_device(dev);
    return ret;
}

static int tc_reg_size(uint16_t reg)
{
    if (reg <= 0x00ff) {
        return 2;
    }
    if (reg <= 0x06ff) {
        return 4;
    }
    if (reg <= 0x84ff) {
        return 2;
    }
    return 1;
}

static esp_err_t tc_read_reg(jetkvm_hdmi_bridge_t *ctx, uint16_t reg, int size, uint32_t *value)
{
    uint8_t raw[4] = {0};
    ESP_RETURN_ON_FALSE(size >= 1 && size <= 4, ESP_ERR_INVALID_ARG, ctx->log_tag, "bad read size");
    ESP_RETURN_ON_ERROR(tc_read_bytes(ctx, reg, raw, size), ctx->log_tag, "read reg 0x%04x failed", reg);

    uint32_t v = 0;
    for (int i = 0; i < size; i++) {
        v |= ((uint32_t)raw[i]) << (8 * i);
    }
    *value = v;
    return ESP_OK;
}

esp_err_t tc_write_reg(jetkvm_hdmi_bridge_t *ctx, uint16_t reg, int size, uint32_t value)
{
    uint8_t raw[4];
    ESP_RETURN_ON_FALSE(size >= 1 && size <= 4, ESP_ERR_INVALID_ARG, ctx->log_tag, "bad write size");
    for (int i = 0; i < size; i++) {
        raw[i] = (value >> (8 * i)) & 0xff;
    }
    return tc_write_bytes(ctx, reg, raw, size);
}

static esp_err_t tc_read_auto(jetkvm_hdmi_bridge_t *ctx, uint16_t reg, uint32_t *value)
{
    return tc_read_reg(ctx, reg, tc_reg_size(reg), value);
}

static esp_err_t tc_write_auto(jetkvm_hdmi_bridge_t *ctx, uint16_t reg, uint32_t value)
{
    return tc_write_reg(ctx, reg, tc_reg_size(reg), value);
}

static esp_err_t tc_update_bits(jetkvm_hdmi_bridge_t *ctx, uint16_t reg, uint32_t mask, uint32_t value)
{
    uint32_t cur = 0;
    ESP_RETURN_ON_ERROR(tc_read_auto(ctx, reg, &cur), ctx->log_tag, "read reg 0x%04x failed", reg);
    cur = (cur & mask) | value;
    return tc_write_auto(ctx, reg, cur);
}

esp_err_t tc_probe_chip(jetkvm_hdmi_bridge_t *ctx)
{
    ESP_LOGI(ctx->log_tag,
             "bridge bus: sda=%d level=%d scl=%d level=%d addr=0x%02x",
             TC358743_I2C_SDA, gpio_get_level(TC358743_I2C_SDA),
             TC358743_I2C_SCL, gpio_get_level(TC358743_I2C_SCL),
             TC358743_I2C_ADDR);

    esp_err_t address_probe =
        i2c_master_probe(ctx->i2c_bus, TC358743_I2C_ADDR, 100);
    if (address_probe != ESP_OK) {
        unsigned found = 0;
        ESP_LOGW(ctx->log_tag, "bridge address 0x%02x did not acknowledge: %s",
                 TC358743_I2C_ADDR, esp_err_to_name(address_probe));
        for (uint16_t address = 0x08; address <= 0x77; ++address) {
            if (address == TC358743_I2C_ADDR) {
                continue;
            }
            if (i2c_master_probe(ctx->i2c_bus, address, 20) == ESP_OK) {
                ESP_LOGW(ctx->log_tag, "unexpected I2C responder at 0x%02x",
                         address);
                ++found;
            }
        }
        if (found == 0) {
            ESP_LOGW(ctx->log_tag, "no I2C device acknowledged on the configured bus");
        }
        return address_probe;
    }

    uint32_t chipid = 0;
    ESP_RETURN_ON_ERROR(tc_read_reg(ctx, CHIPID, 2, &chipid), ctx->log_tag, "chip id read failed");
    ESP_LOGI(ctx->log_tag, "chip id=0x%02" PRIx32 " rev=0x%02" PRIx32 " int=%d",
             (chipid & MASK_CHIPID) >> 8, chipid & MASK_REVID, gpio_get_level(TC358743_INT));
    return ESP_OK;
}

static esp_err_t tc_reset_block(jetkvm_hdmi_bridge_t *ctx, uint16_t mask)
{
    uint32_t sysctl = 0;
    ESP_RETURN_ON_ERROR(tc_read_reg(ctx, SYSCTL, 2, &sysctl), ctx->log_tag, "read sysctl failed");
    ESP_RETURN_ON_ERROR(tc_write_reg(ctx, SYSCTL, 2, sysctl | mask), ctx->log_tag, "set reset failed");
    return tc_write_reg(ctx, SYSCTL, 2, sysctl & ~mask);
}

static esp_err_t tc_sleep_mode(jetkvm_hdmi_bridge_t *ctx, bool enable)
{
    return tc_update_bits(ctx, SYSCTL, ~MASK_SLEEP, enable ? MASK_SLEEP : 0);
}

esp_err_t tc_reset_phy(jetkvm_hdmi_bridge_t *ctx)
{
    ESP_RETURN_ON_ERROR(tc_update_bits(ctx, PHY_RST, ~MASK_RESET_CTRL, 0), ctx->log_tag, "phy rst low failed");
    return tc_update_bits(ctx, PHY_RST, ~MASK_RESET_CTRL, MASK_RESET_CTRL);
}

static esp_err_t tc_init_interrupts(jetkvm_hdmi_bridge_t *ctx)
{
    for (uint16_t reg = SYS_INT; reg <= KEY_INT; reg++) {
        ESP_RETURN_ON_ERROR(tc_write_reg(ctx, reg, 1, 0xff), ctx->log_tag, "clear irq reg 0x%04x failed", reg);
    }
    return tc_write_reg(ctx, INTSTATUS, 2, 0xffff);
}

static esp_err_t tc_enable_interrupts(jetkvm_hdmi_bridge_t *ctx, bool cable_connected)
{
    if (cable_connected) {
        ESP_RETURN_ON_ERROR(tc_write_reg(ctx, SYS_INTM, 1, (~(MASK_M_DDC | MASK_M_DVI_DET | MASK_M_HDMI_DET)) & 0xff),
                            ctx->log_tag, "sys_intm failed");
        ESP_RETURN_ON_ERROR(tc_write_reg(ctx, CLK_INTM, 1, (~MASK_M_IN_DE_CHG) & 0xff), ctx->log_tag, "clk_intm failed");
        ESP_RETURN_ON_ERROR(tc_write_reg(ctx, CBIT_INTM, 1, (~(MASK_M_CBIT_FS | MASK_M_AF_LOCK | MASK_M_AF_UNLOCK)) & 0xff),
                            ctx->log_tag, "cbit_intm failed");
        ESP_RETURN_ON_ERROR(tc_write_reg(ctx, AUDIO_INTM, 1, (~MASK_M_BUFINIT_END) & 0xff), ctx->log_tag, "audio_intm failed");
        return tc_write_reg(ctx, MISC_INTM, 1, (~MASK_M_SYNC_CHG) & 0xff);
    }

    ESP_RETURN_ON_ERROR(tc_write_reg(ctx, SYS_INTM, 1, (~MASK_M_DDC) & 0xff), ctx->log_tag, "sys_intm ddc failed");
    ESP_RETURN_ON_ERROR(tc_write_reg(ctx, CLK_INTM, 1, 0xff), ctx->log_tag, "clk_intm mask failed");
    ESP_RETURN_ON_ERROR(tc_write_reg(ctx, CBIT_INTM, 1, 0xff), ctx->log_tag, "cbit_intm mask failed");
    ESP_RETURN_ON_ERROR(tc_write_reg(ctx, AUDIO_INTM, 1, 0xff), ctx->log_tag, "audio_intm mask failed");
    return tc_write_reg(ctx, MISC_INTM, 1, 0xff);
}

static bool tc_no_signal(jetkvm_hdmi_bridge_t *ctx)
{
    uint32_t status = 0;
    if (tc_read_reg(ctx, SYS_STATUS, 1, &status) != ESP_OK) {
        return true;
    }
    return (status & MASK_S_TMDS) == 0;
}

static bool tc_no_sync(jetkvm_hdmi_bridge_t *ctx)
{
    uint32_t status = 0;
    if (tc_read_reg(ctx, SYS_STATUS, 1, &status) != ESP_OK) {
        return true;
    }
    return (status & MASK_S_SYNC) == 0;
}

static esp_err_t tc_set_ref_clk(jetkvm_hdmi_bridge_t *ctx, uint32_t refclk_hz)
{
    uint32_t sys_freq = refclk_hz / 10000;
    uint32_t fh_min = refclk_hz / 100000;
    uint32_t fh_max = (fh_min * 66) / 10;
    uint32_t lockdet_ref = refclk_hz / 100;
    uint32_t cec_freq = (656 * sys_freq) / 4200;

    ESP_RETURN_ON_ERROR(tc_write_reg(ctx, SYS_FREQ0, 1, sys_freq & 0xff), ctx->log_tag, "sys_freq0 failed");
    ESP_RETURN_ON_ERROR(tc_write_reg(ctx, SYS_FREQ1, 1, (sys_freq >> 8) & 0xff), ctx->log_tag, "sys_freq1 failed");
    ESP_RETURN_ON_ERROR(tc_update_bits(ctx, PHY_CTL0, ~MASK_PHY_SYSCLK_IND, 0), ctx->log_tag, "phy_ctl0 failed");
    ESP_RETURN_ON_ERROR(tc_write_reg(ctx, FH_MIN0, 1, fh_min & 0xff), ctx->log_tag, "fh_min0 failed");
    ESP_RETURN_ON_ERROR(tc_write_reg(ctx, FH_MIN1, 1, (fh_min >> 8) & 0xff), ctx->log_tag, "fh_min1 failed");
    ESP_RETURN_ON_ERROR(tc_write_reg(ctx, FH_MAX0, 1, fh_max & 0xff), ctx->log_tag, "fh_max0 failed");
    ESP_RETURN_ON_ERROR(tc_write_reg(ctx, FH_MAX1, 1, (fh_max >> 8) & 0xff), ctx->log_tag, "fh_max1 failed");
    ESP_RETURN_ON_ERROR(tc_write_reg(ctx, LOCKDET_REF0, 1, lockdet_ref & 0xff), ctx->log_tag, "lockdet0 failed");
    ESP_RETURN_ON_ERROR(tc_write_reg(ctx, LOCKDET_REF1, 1, (lockdet_ref >> 8) & 0xff), ctx->log_tag, "lockdet1 failed");
    ESP_RETURN_ON_ERROR(tc_write_reg(ctx, LOCKDET_REF2, 1, (lockdet_ref >> 16) & 0x0f), ctx->log_tag, "lockdet2 failed");
    ESP_RETURN_ON_ERROR(tc_update_bits(ctx, NCO_F0_MOD, ~MASK_NCO_F0_MOD, MASK_NCO_F0_MOD_27MHZ), ctx->log_tag, "nco_f0 failed");
    ESP_RETURN_ON_ERROR(tc_write_reg(ctx, CECHCLK, 2, cec_freq), ctx->log_tag, "cechclk failed");
    return tc_write_reg(ctx, CECLCLK, 2, cec_freq);
}

esp_err_t tc_set_pll(jetkvm_hdmi_bridge_t *ctx, uint32_t refclk_hz, uint32_t bps_per_lane)
{
    const uint16_t pll_prd = refclk_hz / 6000000;
    const uint16_t pll_fbd = (uint16_t)(((uint64_t)bps_per_lane * pll_prd) / refclk_hz);
    const uint16_t pllctl0 = SET_PLL_PRD(pll_prd) | SET_PLL_FBD(pll_fbd);
    uint16_t pll_frs = 0x3;

    if (bps_per_lane > 500000000U) {
        pll_frs = 0x0;
    } else if (bps_per_lane > 250000000U) {
        pll_frs = 0x1;
    } else if (bps_per_lane > 125000000U) {
        pll_frs = 0x2;
    }

    ESP_RETURN_ON_ERROR(tc_sleep_mode(ctx, true), ctx->log_tag, "sleep enable failed");
    ESP_RETURN_ON_ERROR(tc_write_reg(ctx, PLLCTL0, 2, pllctl0), ctx->log_tag, "pllctl0 failed");
    ESP_RETURN_ON_ERROR(tc_update_bits(ctx, PLLCTL1,
                                       ~(uint32_t)(0x0c00 | MASK_RESETB | MASK_PLL_EN),
                                       SET_PLL_FRS(pll_frs) | MASK_RESETB | MASK_PLL_EN),
                        ctx->log_tag, "pllctl1 failed");
    esp_rom_delay_us(10);
    ESP_RETURN_ON_ERROR(tc_update_bits(ctx, PLLCTL1, ~MASK_CKEN, MASK_CKEN), ctx->log_tag, "pll cken failed");
    return tc_sleep_mode(ctx, false);
}

static esp_err_t tc_set_hdmi_phy(jetkvm_hdmi_bridge_t *ctx)
{
    ESP_RETURN_ON_ERROR(tc_update_bits(ctx, PHY_EN, ~MASK_ENABLE_PHY, 0), ctx->log_tag, "phy disable failed");
    ESP_RETURN_ON_ERROR(tc_write_reg(ctx, PHY_CTL1, 1, SET_PHY_AUTO_RST1_US(1600) | SET_FREQ_RANGE_MODE_CYCLES(1)),
                        ctx->log_tag, "phy_ctl1 failed");
    ESP_RETURN_ON_ERROR(tc_update_bits(ctx, PHY_CTL2, ~(MASK_PHY_AUTO_RST4 | MASK_PHY_AUTO_RST3 | MASK_PHY_AUTO_RST2),
                                       0),
                        ctx->log_tag, "phy_ctl2 failed");
    ESP_RETURN_ON_ERROR(tc_write_reg(ctx, PHY_BIAS, 1, 0x40), ctx->log_tag, "phy_bias failed");
    ESP_RETURN_ON_ERROR(tc_write_reg(ctx, PHY_CSQ, 1, SET_CSQ_CNT_LEVEL(0x0a)), ctx->log_tag, "phy_csq failed");
    ESP_RETURN_ON_ERROR(tc_write_reg(ctx, AVM_CTL, 1, 45), ctx->log_tag, "avm_ctl failed");
    ESP_RETURN_ON_ERROR(tc_update_bits(ctx, HDMI_DET, ~MASK_HDMI_DET_V, 0), ctx->log_tag, "hdmi_det failed");
    ESP_RETURN_ON_ERROR(tc_update_bits(ctx, HV_RST,
                                       ~(uint32_t)(MASK_H_PI_RST | MASK_V_PI_RST),
                                       0),
                        ctx->log_tag, "hv_rst failed");
    return tc_update_bits(ctx, PHY_EN, ~MASK_ENABLE_PHY, MASK_ENABLE_PHY);
}

static esp_err_t tc_set_hdmi_audio_defaults(jetkvm_hdmi_bridge_t *ctx)
{
    ESP_RETURN_ON_ERROR(tc_write_reg(ctx, FORCE_MUTE, 1, 0x00), ctx->log_tag, "force_mute failed");
    ESP_RETURN_ON_ERROR(tc_write_reg(ctx, AUTO_CMD0, 1,
                                     MASK_AUTO_MUTE7 | MASK_AUTO_MUTE6 |
                                     MASK_AUTO_MUTE5 | MASK_AUTO_MUTE4 |
                                     MASK_AUTO_MUTE1 | MASK_AUTO_MUTE0),
                        ctx->log_tag, "auto_cmd0 failed");
    ESP_RETURN_ON_ERROR(tc_write_reg(ctx, AUTO_CMD1, 1, MASK_AUTO_MUTE9), ctx->log_tag, "auto_cmd1 failed");
    ESP_RETURN_ON_ERROR(tc_write_reg(ctx, AUTO_CMD2, 1, MASK_AUTO_PLAY3 | MASK_AUTO_PLAY2), ctx->log_tag, "auto_cmd2 failed");
    ESP_RETURN_ON_ERROR(tc_write_reg(ctx, BUFINIT_START, 1, SET_BUFINIT_START_MS(500)), ctx->log_tag, "bufinit_start failed");
    ESP_RETURN_ON_ERROR(tc_write_reg(ctx, FS_MUTE, 1, 0x00), ctx->log_tag, "fs_mute failed");
    ESP_RETURN_ON_ERROR(tc_write_reg(ctx, FS_IMODE, 1, MASK_NLPCM_SMODE | MASK_FS_SMODE), ctx->log_tag, "fs_imode failed");
    ESP_RETURN_ON_ERROR(tc_write_reg(ctx, ACR_MODE, 1, MASK_CTS_MODE), ctx->log_tag, "acr_mode failed");
    ESP_RETURN_ON_ERROR(tc_write_reg(ctx, ACR_MDF0, 1, MASK_ACR_L2MDF_1976_PPM | MASK_ACR_L1MDF_976_PPM),
                        ctx->log_tag, "acr_mdf0 failed");
    ESP_RETURN_ON_ERROR(tc_write_reg(ctx, ACR_MDF1, 1, MASK_ACR_L3MDF_3906_PPM), ctx->log_tag, "acr_mdf1 failed");
    ESP_RETURN_ON_ERROR(tc_write_reg(ctx, SDO_MODE1, 1, MASK_SDO_FMT_I2S), ctx->log_tag, "sdo_mode1 failed");
    ESP_RETURN_ON_ERROR(tc_write_reg(ctx, DIV_MODE, 1, SET_DIV_DLY_MS(100)), ctx->log_tag, "div_mode failed");
    return tc_update_bits(ctx, CONFCTL,
                          ~(uint32_t)(0x0c00 | 0x0018 | 0x0004),
                          0x0c00 | 0x0010 | 0x0004);
}

static esp_err_t tc_set_hdmi_info_frame_mode(jetkvm_hdmi_bridge_t *ctx)
{
    ESP_RETURN_ON_ERROR(tc_write_reg(ctx, PK_INT_MODE, 1,
                                     MASK_ISRC2_INT_MODE | MASK_ISRC_INT_MODE |
                                     MASK_ACP_INT_MODE | MASK_VS_INT_MODE |
                                     MASK_SPD_INT_MODE | MASK_MS_INT_MODE |
                                     MASK_AUD_INT_MODE | MASK_AVI_INT_MODE),
                        ctx->log_tag, "pk_int_mode failed");
    ESP_RETURN_ON_ERROR(tc_write_reg(ctx, NO_PKT_LIMIT, 1, 0x2c), ctx->log_tag, "no_pkt_limit failed");
    ESP_RETURN_ON_ERROR(tc_write_reg(ctx, NO_PKT_CLR, 1, 0x53), ctx->log_tag, "no_pkt_clr failed");
    ESP_RETURN_ON_ERROR(tc_write_reg(ctx, ERR_PK_LIMIT, 1, 0x01), ctx->log_tag, "err_pk_limit failed");
    ESP_RETURN_ON_ERROR(tc_write_reg(ctx, NO_PKT_LIMIT2, 1, 0x30), ctx->log_tag, "no_pkt_limit2 failed");
    return tc_write_reg(ctx, NO_GDB_LIMIT, 1, 0x10);
}

esp_err_t tc_set_yuv422_output(jetkvm_hdmi_bridge_t *ctx)
{
    ESP_RETURN_ON_ERROR(tc_update_bits(ctx, VOUT_SET2,
                                       ~(uint32_t)(MASK_SEL422 | MASK_VOUT_422FIL_100 | MASK_VOUTCOLORMODE),
                                       MASK_SEL422 | MASK_VOUT_422FIL_100 | MASK_VOUTCOLORMODE_AUTO),
                        ctx->log_tag, "vout_set2 failed");
    ESP_RETURN_ON_ERROR(tc_update_bits(ctx, VI_REP, ~MASK_VOUT_COLOR_SEL, MASK_VOUT_COLOR_601_YCBCR_LIMITED),
                        ctx->log_tag, "vi_rep failed");
    return tc_update_bits(ctx, CONFCTL, ~MASK_YCBCRFMT, MASK_YCBCRFMT_422_8_BIT);
}

esp_err_t tc_set_colorbar_output(jetkvm_hdmi_bridge_t *ctx)
{
    ESP_RETURN_ON_ERROR(tc_update_bits(ctx, VOUT_SET2,
                                       ~(uint32_t)(MASK_SEL422 | MASK_VOUT_422FIL_100 | MASK_VOUTCOLORMODE),
                                       MASK_SEL422 | MASK_VOUT_422FIL_100 | MASK_VOUTCOLORMODE_AUTO),
                        ctx->log_tag, "vout_set2 colorbar failed");
    ESP_RETURN_ON_ERROR(tc_update_bits(ctx, VI_REP, ~MASK_VOUT_COLOR_SEL, MASK_VOUT_COLOR_601_YCBCR_LIMITED),
                        ctx->log_tag, "vi_rep colorbar failed");
    return tc_update_bits(ctx, CONFCTL, ~MASK_YCBCRFMT, MASK_YCBCRFMT_COLORBAR);
}

esp_err_t tc_set_rgb888_output(jetkvm_hdmi_bridge_t *ctx)
{
    ESP_RETURN_ON_ERROR(tc_update_bits(ctx, VOUT_SET2,
                                       ~(uint32_t)(MASK_SEL422 | MASK_VOUT_422FIL_100),
                                       0),
                        ctx->log_tag, "vout_set2 rgb failed");
    ESP_RETURN_ON_ERROR(tc_update_bits(ctx, VI_REP, ~MASK_VOUT_COLOR_SEL, MASK_VOUT_COLOR_RGB_FULL),
                        ctx->log_tag, "vi_rep rgb failed");
    return tc_update_bits(ctx, CONFCTL, ~MASK_YCBCRFMT, 0);
}

esp_err_t tc_enable_stream(jetkvm_hdmi_bridge_t *ctx, bool enable)
{
    const tc358743_capture_candidate_t *candidate = ctx->active_candidate;
    bool continuous_clock = !candidate || candidate->continuous_clock;

    if (enable) {
        uint32_t vi_mute = (candidate && candidate->disable_auto_mute) ? 0 : MASK_AUTO_MUTE;
        /* Mirror the Linux driver sequence: force LP11 on the clock lane,
         * then switch back to continuous clock so the receiver sees a clean
         * LP11->HS transition before video starts flowing.
         */
        ESP_RETURN_ON_ERROR(tc_write_reg(ctx, TXOPTIONCNTRL, 4, 0), ctx->log_tag, "txoption lp11 failed");
        ESP_RETURN_ON_ERROR(tc_write_reg(ctx, TXOPTIONCNTRL, 4, continuous_clock ? MASK_CONTCLKMODE : 0),
                            ctx->log_tag, "txoption mode failed");
        ESP_RETURN_ON_ERROR(tc_write_reg(ctx, VI_MUTE, 1, vi_mute), ctx->log_tag, "vi mute clear failed");
        ESP_LOGI(ctx->log_tag, "stream enable candidate=%s vi_mute=0x%02" PRIx32 " contclk=%d",
                 candidate ? candidate->name : "none", vi_mute, continuous_clock);
        return tc_update_bits(ctx, CONFCTL, ~(uint32_t)(MASK_VBUFEN | MASK_ABUFEN), MASK_VBUFEN | MASK_ABUFEN);
    }

    ESP_RETURN_ON_ERROR(tc_write_reg(ctx, VI_MUTE, 1, MASK_AUTO_MUTE | MASK_VI_MUTE),
                        ctx->log_tag, "vi mute set failed");
    return tc_update_bits(ctx, CONFCTL, ~(uint32_t)(MASK_VBUFEN | MASK_ABUFEN), 0);
}

esp_err_t tc_set_csi(jetkvm_hdmi_bridge_t *ctx, const tc358743_csi_preset_t *preset)
{
    const tc358743_capture_candidate_t *candidate = ctx->active_candidate;
    uint8_t lane_num = candidate ? candidate->data_lane_num : TC358743_LANES;
    uint32_t lane_enable = MASK_CLM_HSTXVREGEN;
    uint32_t csi_control = MASK_MODE_SET | MASK_ADDRESS_CSI_CONTROL |
                           MASK_CSI_MODE | MASK_TXHSMD |
                           (lane_num >= 2 ? MASK_NOL_2 : MASK_NOL_1);

    if (candidate && candidate->hsckmd) {
        csi_control |= MASK_HSCKMD;
    }
    if (candidate && candidate->eot_disable) {
        csi_control |= MASK_EOTDIS;
    }

    ESP_RETURN_ON_ERROR(tc_reset_block(ctx, MASK_CTXRST), ctx->log_tag, "csi reset failed");
    ESP_RETURN_ON_ERROR(tc_write_reg(ctx, CLW_CNTRL, 4, 0), ctx->log_tag, "clw failed");
    ESP_RETURN_ON_ERROR(tc_write_reg(ctx, D0W_CNTRL, 4, 0), ctx->log_tag, "d0 failed");
    ESP_RETURN_ON_ERROR(tc_write_reg(ctx, D1W_CNTRL, 4, lane_num >= 2 ? 0 : MASK_D1W_LANEDISABLE), ctx->log_tag, "d1 failed");
    ESP_RETURN_ON_ERROR(tc_write_reg(ctx, D2W_CNTRL, 4, MASK_D2W_LANEDISABLE), ctx->log_tag, "d2 failed");
    ESP_RETURN_ON_ERROR(tc_write_reg(ctx, D3W_CNTRL, 4, MASK_D3W_LANEDISABLE), ctx->log_tag, "d3 failed");

    ESP_RETURN_ON_ERROR(tc_write_reg(ctx, LINEINITCNT, 4, preset->lineinitcnt), ctx->log_tag, "lineinitcnt failed");
    ESP_RETURN_ON_ERROR(tc_write_reg(ctx, LPTXTIMECNT, 4, preset->lptxtimecnt), ctx->log_tag, "lptxtimecnt failed");
    ESP_RETURN_ON_ERROR(tc_write_reg(ctx, TCLK_HEADERCNT, 4, preset->tclk_headercnt), ctx->log_tag, "tclk_headercnt failed");
    ESP_RETURN_ON_ERROR(tc_write_reg(ctx, TCLK_TRAILCNT, 4, preset->tclk_trailcnt), ctx->log_tag, "tclk_trailcnt failed");
    ESP_RETURN_ON_ERROR(tc_write_reg(ctx, THS_HEADERCNT, 4, preset->ths_headercnt), ctx->log_tag, "ths_headercnt failed");
    ESP_RETURN_ON_ERROR(tc_write_reg(ctx, TWAKEUP, 4, preset->twakeup), ctx->log_tag, "twakeup failed");
    ESP_RETURN_ON_ERROR(tc_write_reg(ctx, TCLK_POSTCNT, 4, preset->tclk_postcnt), ctx->log_tag, "tclk_postcnt failed");
    ESP_RETURN_ON_ERROR(tc_write_reg(ctx, THS_TRAILCNT, 4, preset->ths_trailcnt), ctx->log_tag, "ths_trailcnt failed");
    ESP_RETURN_ON_ERROR(tc_write_reg(ctx, HSTXVREGCNT, 4, preset->hstxvregcnt), ctx->log_tag, "hstxvregcnt failed");
    lane_enable |= MASK_D0M_HSTXVREGEN;
    if (lane_num >= 2) {
        lane_enable |= MASK_D1M_HSTXVREGEN;
    }
    ESP_RETURN_ON_ERROR(tc_write_reg(ctx, HSTXVREGEN, 4, lane_enable), ctx->log_tag, "hstxvregen failed");
    ESP_RETURN_ON_ERROR(tc_write_reg(ctx, TXOPTIONCNTRL, 4,
                                     (!candidate || candidate->continuous_clock) ? MASK_CONTCLKMODE : 0),
                        ctx->log_tag, "txoptioncntrl failed");
    ESP_RETURN_ON_ERROR(tc_write_reg(ctx, STARTCNTRL, 4, MASK_START), ctx->log_tag, "startcntrl failed");
    ESP_RETURN_ON_ERROR(tc_write_reg(ctx, CSI_START, 4, MASK_STRT), ctx->log_tag, "csi_start failed");

    ESP_RETURN_ON_ERROR(tc_write_reg(ctx, CSI_CONFW, 4, csi_control), ctx->log_tag, "csi control failed");
    ESP_RETURN_ON_ERROR(tc_write_reg(ctx, CSI_CONFW, 4,
                                     MASK_MODE_SET | MASK_ADDRESS_CSI_ERR_INTENA |
                                     MASK_TXBRK | MASK_QUNK | MASK_WCER | MASK_INER),
                        ctx->log_tag, "csi err int ena failed");
    ESP_RETURN_ON_ERROR(tc_write_reg(ctx, CSI_CONFW, 4,
                                     MASK_MODE_CLEAR | MASK_ADDRESS_CSI_ERR_HALT |
                                     MASK_TXBRK | MASK_QUNK),
                        ctx->log_tag, "csi err halt failed");
    ESP_RETURN_ON_ERROR(tc_write_reg(ctx, CSI_CONFW, 4,
                                     MASK_MODE_SET | MASK_ADDRESS_CSI_INT_ENA | MASK_INTER),
                        ctx->log_tag, "csi int ena failed");
    return tc_write_reg(ctx, CSI_INT_CLR, 4, MASK_ICRER);
}

esp_err_t tc_program_edid(jetkvm_hdmi_bridge_t *ctx,
                          const p4_edid_descriptor_t *descriptor)
{
    uint32_t hpd_ctl = 0;
    uint8_t verify[16];
    esp_err_t error = ESP_OK;

    ESP_RETURN_ON_FALSE(descriptor && descriptor->bytes &&
                            (descriptor->size == 128U ||
                             descriptor->size == 256U),
                        ESP_ERR_INVALID_ARG, ctx->log_tag, "invalid EDID descriptor");
    for (size_t block = 0; block < descriptor->size / 128U; block++) {
        uint8_t checksum = 0;
        for (size_t i = 0; i < 128; i++) {
            checksum += descriptor->bytes[block * 128U + i];
        }
        ESP_RETURN_ON_FALSE(checksum == 0, ESP_ERR_INVALID_CRC, ctx->log_tag,
                            "EDID block %u checksum invalid", (unsigned)block);
    }

    ESP_LOGI(ctx->log_tag, "program %u-byte EDID %s",
             (unsigned)descriptor->size, descriptor->label);
    error = tc_update_bits(ctx, HPD_CTL, ~MASK_HPD_OUT0, 0);
    if (error != ESP_OK) {
        ESP_LOGE(ctx->log_tag, "hpd low failed: %s", esp_err_to_name(error));
        return error;
    }
    error = tc_write_reg(ctx, EDID_LEN1, 1, descriptor->size & 0xffU);
    if (error != ESP_OK) {
        ESP_LOGE(ctx->log_tag, "edid len1 failed: %s", esp_err_to_name(error));
        goto restore_hpd;
    }
    error = tc_write_reg(ctx, EDID_LEN2, 1,
                         (descriptor->size >> 8U) & 0xffU);
    if (error != ESP_OK) {
        ESP_LOGE(ctx->log_tag, "edid len2 failed: %s", esp_err_to_name(error));
        goto restore_hpd;
    }

    for (size_t offset = 0; offset < descriptor->size;
         offset += sizeof(verify)) {
        size_t chunk = descriptor->size - offset;
        if (chunk > 16) {
            chunk = 16;
        }
        error = tc_write_bytes(ctx, EDID_RAM + offset,
                               &descriptor->bytes[offset], chunk);
        if (error != ESP_OK) {
            ESP_LOGE(ctx->log_tag, "edid write failed at %u: %s", (unsigned)offset,
                     esp_err_to_name(error));
            goto restore_hpd;
        }
        error = tc_read_bytes(ctx, EDID_RAM + offset, verify, chunk);
        if (error != ESP_OK) {
            ESP_LOGE(ctx->log_tag, "edid readback failed at %u: %s",
                     (unsigned)offset, esp_err_to_name(error));
            goto restore_hpd;
        }
        if (memcmp(verify, &descriptor->bytes[offset], chunk) != 0) {
            ESP_LOGE(ctx->log_tag, "edid verify mismatch at %u", (unsigned)offset);
            error = ESP_ERR_INVALID_RESPONSE;
            goto restore_hpd;
        }
    }

    /* Hold HPD low long enough to force the source to handshake again. */
    vTaskDelay(pdMS_TO_TICKS(TC_HPD_LOW_MIN_MS));

restore_hpd:
    {
        esp_err_t hpd_error = tc_update_bits(
            ctx, HPD_CTL, ~MASK_HPD_OUT0, MASK_HPD_OUT0);
        if (hpd_error != ESP_OK) {
            ESP_LOGE(ctx->log_tag, "hpd high failed: %s", esp_err_to_name(hpd_error));
            if (error == ESP_OK) {
                error = hpd_error;
            }
        } else {
            esp_err_t read_error = tc_read_reg(ctx, HPD_CTL, 1, &hpd_ctl);
            if (read_error != ESP_OK) {
                ESP_LOGE(ctx->log_tag, "read hpd ctl failed: %s",
                         esp_err_to_name(read_error));
                if (error == ESP_OK) {
                    error = read_error;
                }
            } else {
                ESP_LOGI(ctx->log_tag, "HPD_CTL=0x%02" PRIx32 " ctl0=%d out0=%d",
                         hpd_ctl, !!(hpd_ctl & 0x10),
                         !!(hpd_ctl & MASK_HPD_OUT0));
            }
        }
    }
    return error;
}

esp_err_t tc_program_edid_bytes(jetkvm_hdmi_bridge_t *ctx,
                                const uint8_t edid[256])
{
    const p4_edid_descriptor_t descriptor = {
        .bytes = edid, .size = 256U, .label = "OS Services guest"};
    return tc_program_edid(ctx, &descriptor);
}

esp_err_t tc_set_hpd_output(jetkvm_hdmi_bridge_t *ctx, bool advertised)
{
    ESP_RETURN_ON_ERROR(
        tc_update_bits(ctx, HPD_CTL, ~MASK_HPD_OUT0,
                       advertised ? MASK_HPD_OUT0 : 0U),
        ctx->log_tag, "set HPD output failed");
    uint32_t hpd_ctl = 0;
    ESP_RETURN_ON_ERROR(tc_read_reg(ctx, HPD_CTL, 1, &hpd_ctl), ctx->log_tag,
                        "read HPD output failed");
    ESP_RETURN_ON_FALSE(!!(hpd_ctl & MASK_HPD_OUT0) == advertised,
                        ESP_ERR_INVALID_RESPONSE, ctx->log_tag,
                        "HPD output readback mismatch requested=%d value=0x%02" PRIx32,
                        advertised, hpd_ctl);
    ESP_LOGI(ctx->log_tag, "host display HPD advertised=%d HPD_CTL=0x%02" PRIx32,
             advertised, hpd_ctl);
    return ESP_OK;
}

esp_err_t tc_initial_setup(jetkvm_hdmi_bridge_t *ctx, uint32_t refclk_hz,
                           const p4_edid_descriptor_t *edid)
{
    uint32_t sys_status = 0;

    ESP_RETURN_ON_ERROR(tc_update_bits(ctx, SYSCTL, ~(uint32_t)(MASK_IRRST | MASK_CECRST), MASK_IRRST | MASK_CECRST),
                        ctx->log_tag, "sysctl rst bits failed");
    ESP_RETURN_ON_ERROR(tc_reset_block(ctx, MASK_CTXRST | MASK_HDMIRST), ctx->log_tag, "block reset failed");
    ESP_RETURN_ON_ERROR(tc_sleep_mode(ctx, false), ctx->log_tag, "sleep clear failed");
    ESP_RETURN_ON_ERROR(tc_write_reg(ctx, FIFOCTL, 2, TC358743_FIFO_LEVEL), ctx->log_tag, "fifoctl failed");
    ESP_RETURN_ON_ERROR(tc_set_ref_clk(ctx, refclk_hz), ctx->log_tag, "set_ref_clk failed");
    ESP_RETURN_ON_ERROR(tc_update_bits(ctx, DDC_CTL, ~MASK_DDC5V_MODE, TC358743_DDC5V_DELAY_100MS), ctx->log_tag, "ddc ctl failed");
    ESP_RETURN_ON_ERROR(tc_update_bits(ctx, EDID_MODE, ~MASK_EDID_MODE, MASK_EDID_MODE_E_DDC), ctx->log_tag, "edid mode failed");
    ESP_RETURN_ON_ERROR(tc_set_hdmi_phy(ctx), ctx->log_tag, "set_hdmi_phy failed");
    ESP_RETURN_ON_ERROR(tc_write_reg(ctx, ANA_CTL, 1, MASK_APPL_PCSX_NORMAL | MASK_ANALOG_ON), ctx->log_tag, "ana ctl failed");
    ESP_RETURN_ON_ERROR(tc_update_bits(ctx, HDCP_MODE,
                                       ~MASK_MANUAL_AUTHENTICATION,
                                       MASK_MANUAL_AUTHENTICATION),
                        ctx->log_tag, "disable hdcp authentication failed");
    ESP_RETURN_ON_ERROR(tc_set_hdmi_audio_defaults(ctx), ctx->log_tag, "set_hdmi_audio_defaults failed");
    ESP_RETURN_ON_ERROR(tc_set_hdmi_info_frame_mode(ctx), ctx->log_tag, "set_hdmi_info_frame_mode failed");
    ESP_RETURN_ON_ERROR(tc_init_interrupts(ctx), ctx->log_tag, "init interrupts failed");
    ESP_RETURN_ON_ERROR(tc_update_bits(ctx, VI_MODE, ~MASK_RGB_DVI, 0), ctx->log_tag, "vi_mode failed");
    ESP_RETURN_ON_ERROR(tc_write_reg(ctx, VOUT_SET3, 1, MASK_VOUT_EXTCNT),
                        ctx->log_tag, "vout_set3 failed");
    ESP_RETURN_ON_ERROR(tc_set_yuv422_output(ctx), ctx->log_tag, "set_yuv422_output failed");
    ESP_RETURN_ON_ERROR(tc_program_edid(ctx, edid), ctx->log_tag,
                        "program_edid failed");
    ESP_RETURN_ON_ERROR(tc_read_reg(ctx, SYS_STATUS, 1, &sys_status), ctx->log_tag, "read sys_status failed");
    ESP_RETURN_ON_ERROR(tc_enable_interrupts(ctx, !!(sys_status & MASK_S_DDC5V)), ctx->log_tag, "enable interrupts failed");
    ctx->refclk_hz = refclk_hz;
    return tc_enable_stream(ctx, false);
}

esp_err_t tc_get_sys_status(jetkvm_hdmi_bridge_t *ctx, uint8_t *sys_status)
{
    uint32_t status = 0;
    esp_err_t ret = tc_read_reg(ctx, SYS_STATUS, 1, &status);
    if (ret == ESP_OK) {
        *sys_status = (uint8_t)status;
    }
    return ret;
}

static bool tc_has_signal(jetkvm_hdmi_bridge_t *ctx, uint8_t *status_out)
{
    uint32_t status = 0;
    if (tc_read_reg(ctx, SYS_STATUS, 1, &status) != ESP_OK) {
        return false;
    }
    if (status_out) {
        *status_out = status & 0xff;
    }
    return (status & MASK_S_TMDS) && (status & MASK_S_SYNC);
}

esp_err_t tc_get_detected_timings(jetkvm_hdmi_bridge_t *ctx, tc358743_timings_t *timings)
{
    uint8_t timing_raw[V_SIZE_HI - DE_WIDTH_H_LO + 1] = {0};
    uint8_t frame_interval_raw[2] = {0};
    uint32_t reg = 0;
    uint8_t status = 0;

    memset(timings, 0, sizeof(*timings));
    ESP_RETURN_ON_FALSE(tc_has_signal(ctx, &status), ESP_ERR_NOT_FOUND, ctx->log_tag,
                        "no signal sys_status=0x%02x", status);

    ESP_RETURN_ON_ERROR(tc_read_reg(ctx, HPD_CTL, 1, &reg), ctx->log_tag, "read hpd failed");
    ESP_RETURN_ON_FALSE(reg & MASK_HPD_OUT0, ESP_ERR_NOT_FOUND, ctx->log_tag, "hpd is low");

    ESP_RETURN_ON_ERROR(tc_read_reg(ctx, VI_STATUS1, 1, &reg), ctx->log_tag, "read vi_status1 failed");
    timings->interlaced = (reg & MASK_S_V_INTERLACE) != 0;

    /* Read the whole detector register window in one I2C transaction.  The
     * counters update with the incoming signal; separate high/low-byte reads
     * can otherwise combine two different samples while sync is settling. */
    ESP_RETURN_ON_ERROR(tc_read_bytes(ctx, DE_WIDTH_H_LO, timing_raw,
                                      sizeof(timing_raw)),
                        ctx->log_tag, "read timing window failed");
    timings->width =
        ((timing_raw[DE_WIDTH_H_HI - DE_WIDTH_H_LO] & 0x1f) << 8) |
        timing_raw[DE_WIDTH_H_LO - DE_WIDTH_H_LO];
    timings->height =
        ((timing_raw[DE_WIDTH_V_HI - DE_WIDTH_H_LO] & 0x1f) << 8) |
        timing_raw[DE_WIDTH_V_LO - DE_WIDTH_H_LO];
    timings->frame_width =
        ((timing_raw[H_SIZE_HI - DE_WIDTH_H_LO] & 0x1f) << 8) |
        timing_raw[H_SIZE_LO - DE_WIDTH_H_LO];
    timings->frame_height =
        ((timing_raw[V_SIZE_HI - DE_WIDTH_H_LO] & 0x3f) << 8) |
        timing_raw[V_SIZE_LO - DE_WIDTH_H_LO];
    timings->frame_height /= 2;

    ESP_RETURN_ON_ERROR(tc_read_bytes(ctx, FV_CNT_LO, frame_interval_raw,
                                      sizeof(frame_interval_raw)),
                        ctx->log_tag, "read frame interval failed");
    timings->frame_interval_x10ms =
        ((frame_interval_raw[1] & 0x03) << 8) | frame_interval_raw[0];
    timings->fps = timings->frame_interval_x10ms ? (uint16_t)((10000 + timings->frame_interval_x10ms / 2) / timings->frame_interval_x10ms) : 0;

    if (timings->interlaced) {
        timings->height *= 2;
    }

    if (timings->width < 640 || timings->height < 480 ||
        timings->width > 1920 || timings->height > 1080 ||
        timings->frame_width < timings->width ||
        timings->frame_height < timings->height) {
        ESP_LOGW(ctx->log_tag,
                 "unstable timing sys=0x%02x de=%ux%u frame=%ux%u fps=%u raw="
                 "%02x%02x %02x%02x %02x%02x %02x%02x",
                 status, timings->width, timings->height,
                 timings->frame_width, timings->frame_height, timings->fps,
                 timing_raw[DE_WIDTH_H_HI - DE_WIDTH_H_LO],
                 timing_raw[DE_WIDTH_H_LO - DE_WIDTH_H_LO],
                 timing_raw[DE_WIDTH_V_HI - DE_WIDTH_H_LO],
                 timing_raw[DE_WIDTH_V_LO - DE_WIDTH_H_LO],
                 timing_raw[H_SIZE_HI - DE_WIDTH_H_LO],
                 timing_raw[H_SIZE_LO - DE_WIDTH_H_LO],
                 timing_raw[V_SIZE_HI - DE_WIDTH_H_LO],
                 timing_raw[V_SIZE_LO - DE_WIDTH_H_LO]);
        return ESP_ERR_INVALID_RESPONSE;
    }
    return ESP_OK;
}

void tc_log_signal_status(jetkvm_hdmi_bridge_t *ctx)
{
    uint32_t sys_status = 0;
    uint32_t fs_set = 0;

    if (tc_read_reg(ctx, SYS_STATUS, 1, &sys_status) != ESP_OK ||
        tc_read_reg(ctx, FS_SET, 1, &fs_set) != ESP_OK) {
        ESP_LOGW(ctx->log_tag, "failed to read status registers");
        return;
    }

    ESP_LOGI(ctx->log_tag, "sig sys=0x%02" PRIx32 " tmds=%d sync=%d pll=%d scdt=%d fs=0x%02" PRIx32,
             sys_status,
             !!(sys_status & MASK_S_TMDS),
             !!(sys_status & MASK_S_SYNC),
             !!(sys_status & MASK_S_PHY_PLL),
             !!(sys_status & MASK_S_PHY_SCDT),
             fs_set);
}

void tc_log_top_interrupts(jetkvm_hdmi_bridge_t *ctx)
{
    uint32_t intstatus = 0;
    uint32_t intmask = 0;
    uint32_t intflag = 0;
    uint32_t intsys = 0;

    if (tc_read_reg(ctx, INTSTATUS, 2, &intstatus) != ESP_OK ||
        tc_read_reg(ctx, INTMASK, 2, &intmask) != ESP_OK ||
        tc_read_reg(ctx, INTFLAG, 2, &intflag) != ESP_OK ||
        tc_read_reg(ctx, INTSYSSTATUS, 2, &intsys) != ESP_OK) {
        ESP_LOGW(ctx->log_tag, "failed to read tc358743 top interrupt registers");
        return;
    }

    ESP_LOGI(ctx->log_tag,
             "tc-top intstatus=0x%04" PRIx32 " intmask=0x%04" PRIx32
             " intflag=0x%04" PRIx32 " intsys=0x%04" PRIx32
             " [hdmi=%d csi=%d sys=%d]",
             intstatus, intmask, intflag, intsys,
             !!(intstatus & MASK_HDMI_INT),
             !!(intstatus & MASK_CSI_INT),
             !!(intstatus & MASK_SYS_INT));
}

void tc_log_sub_interrupts(jetkvm_hdmi_bridge_t *ctx)
{
    uint32_t hdmi_int1 = 0;
    uint32_t sys_int = 0;
    uint32_t sys_intm = 0;
    uint32_t clk_int = 0;
    uint32_t clk_intm = 0;
    uint32_t misc_int = 0;
    uint32_t misc_intm = 0;
    uint32_t cbit_int = 0;
    uint32_t cbit_intm = 0;
    uint32_t audio_int = 0;
    uint32_t audio_intm = 0;

    if (tc_read_reg(ctx, HDMI_INT1, 1, &hdmi_int1) != ESP_OK ||
        tc_read_reg(ctx, SYS_INT, 1, &sys_int) != ESP_OK ||
        tc_read_reg(ctx, SYS_INTM, 1, &sys_intm) != ESP_OK ||
        tc_read_reg(ctx, CLK_INT, 1, &clk_int) != ESP_OK ||
        tc_read_reg(ctx, CLK_INTM, 1, &clk_intm) != ESP_OK ||
        tc_read_reg(ctx, MISC_INT, 1, &misc_int) != ESP_OK ||
        tc_read_reg(ctx, MISC_INTM, 1, &misc_intm) != ESP_OK ||
        tc_read_reg(ctx, CBIT_INT, 1, &cbit_int) != ESP_OK ||
        tc_read_reg(ctx, CBIT_INTM, 1, &cbit_intm) != ESP_OK ||
        tc_read_reg(ctx, AUDIO_INT, 1, &audio_int) != ESP_OK ||
        tc_read_reg(ctx, AUDIO_INTM, 1, &audio_intm) != ESP_OK) {
        ESP_LOGW(ctx->log_tag, "failed to read tc358743 sub interrupt registers");
        return;
    }

    ESP_LOGI(ctx->log_tag,
             "tc-sub hdmi1=0x%02" PRIx32 " sys=0x%02" PRIx32 "/0x%02" PRIx32
             " clk=0x%02" PRIx32 "/0x%02" PRIx32 " misc=0x%02" PRIx32 "/0x%02" PRIx32
             " cbit=0x%02" PRIx32 "/0x%02" PRIx32 " aud=0x%02" PRIx32 "/0x%02" PRIx32,
             hdmi_int1, sys_int, sys_intm, clk_int, clk_intm, misc_int, misc_intm,
             cbit_int, cbit_intm, audio_int, audio_intm);
}

void tc_poll_interrupts(jetkvm_hdmi_bridge_t *ctx)
{
    uint32_t intstatus = 0;

    if (tc_read_reg(ctx, INTSTATUS, 2, &intstatus) != ESP_OK || intstatus == 0) {
        return;
    }

    if (intstatus & MASK_HDMI_INT) {
        uint32_t sys_int_mask = 0;
        uint32_t sys_int = 0;
        uint32_t clk_int_mask = 0;
        uint32_t clk_int = 0;
        uint32_t misc_int_mask = 0;
        uint32_t misc_int = 0;
        uint32_t cbit_int_mask = 0;
        uint32_t cbit_int = 0;
        uint32_t audio_int_mask = 0;
        uint32_t audio_int = 0;
        bool need_log = false;

        if (tc_read_reg(ctx, SYS_INTM, 1, &sys_int_mask) == ESP_OK &&
            tc_read_reg(ctx, SYS_INT, 1, &sys_int) == ESP_OK) {
            sys_int &= ~sys_int_mask;
            if (sys_int) {
                need_log = true;
                ESP_ERROR_CHECK_WITHOUT_ABORT(tc_write_reg(ctx, SYS_INT, 1, sys_int));
                if (sys_int & MASK_I_DDC) {
                    uint32_t status = 0;
                    if (tc_read_reg(ctx, SYS_STATUS, 1, &status) == ESP_OK) {
                        ESP_ERROR_CHECK_WITHOUT_ABORT(tc_enable_interrupts(ctx, !!(status & MASK_S_DDC5V)));
                    }
                }
                if (sys_int & MASK_I_DVI) {
                    if (tc_no_signal(ctx) || tc_no_sync(ctx)) {
                        ESP_ERROR_CHECK_WITHOUT_ABORT(tc_reset_phy(ctx));
                    }
                }
                if (sys_int & MASK_I_HDMI) {
                    ESP_ERROR_CHECK_WITHOUT_ABORT(tc_write_reg(ctx, ANA_CTL, 1, MASK_APPL_PCSX_NORMAL | MASK_ANALOG_ON));
                }
            }
        }

        if (tc_read_reg(ctx, CLK_INTM, 1, &clk_int_mask) == ESP_OK &&
            tc_read_reg(ctx, CLK_INT, 1, &clk_int) == ESP_OK) {
            clk_int &= ~clk_int_mask;
            if (clk_int) {
                need_log = true;
                ESP_ERROR_CHECK_WITHOUT_ABORT(tc_write_reg(ctx, CLK_INT, 1, clk_int | 0x80 | MASK_I_OUT_H_CHG));
            }
        }

        if (tc_read_reg(ctx, MISC_INTM, 1, &misc_int_mask) == ESP_OK &&
            tc_read_reg(ctx, MISC_INT, 1, &misc_int) == ESP_OK) {
            misc_int &= ~misc_int_mask;
            if (misc_int) {
                need_log = true;
                ESP_ERROR_CHECK_WITHOUT_ABORT(tc_write_reg(ctx, MISC_INT, 1, misc_int));
                if ((misc_int & MASK_I_SYNC_CHG) && (tc_no_signal(ctx) || tc_no_sync(ctx))) {
                    ESP_ERROR_CHECK_WITHOUT_ABORT(tc_reset_phy(ctx));
                }
            }
        }

        if (tc_read_reg(ctx, CBIT_INTM, 1, &cbit_int_mask) == ESP_OK &&
            tc_read_reg(ctx, CBIT_INT, 1, &cbit_int) == ESP_OK) {
            cbit_int &= ~cbit_int_mask;
            if (cbit_int) {
                need_log = true;
                ESP_ERROR_CHECK_WITHOUT_ABORT(tc_write_reg(ctx, CBIT_INT, 1, cbit_int));
            }
        }

        if (tc_read_reg(ctx, AUDIO_INTM, 1, &audio_int_mask) == ESP_OK &&
            tc_read_reg(ctx, AUDIO_INT, 1, &audio_int) == ESP_OK) {
            audio_int &= ~audio_int_mask;
            if (audio_int) {
                need_log = true;
                ESP_ERROR_CHECK_WITHOUT_ABORT(tc_write_reg(ctx, AUDIO_INT, 1, audio_int));
            }
        }

        if (need_log) {
            ESP_LOGI(ctx->log_tag, "tc-poll 0x%04" PRIx32 " sys=0x%02" PRIx32 " clk=0x%02" PRIx32 " misc=0x%02" PRIx32,
                     intstatus, sys_int, clk_int, misc_int);
        }
    }

    if (intstatus & MASK_CSI_INT) {
        uint32_t csi_err = 0;
        if (tc_read_reg(ctx, CSI_ERR, 4, &csi_err) == ESP_OK) {
            ESP_LOGW(ctx->log_tag, "tc-csi interrupt err=0x%08" PRIx32, csi_err);
        }
        ESP_ERROR_CHECK_WITHOUT_ABORT(tc_write_reg(ctx, CSI_INT_CLR, 4, MASK_ICRER));
    }
}

void tc_log_csi_status(jetkvm_hdmi_bridge_t *ctx)
{
    uint32_t csi_control = 0;
    uint32_t csi_int = 0;
    uint32_t csi_int_ena = 0;
    uint32_t csi_err = 0;
    uint32_t csi_err_intena = 0;
    uint32_t csi_err_halt = 0;

    if (tc_read_reg(ctx, CSI_CONTROL, 2, &csi_control) != ESP_OK ||
        tc_read_reg(ctx, CSI_INT, 4, &csi_int) != ESP_OK ||
        tc_read_reg(ctx, CSI_INT_ENA, 4, &csi_int_ena) != ESP_OK ||
        tc_read_reg(ctx, CSI_ERR, 4, &csi_err) != ESP_OK ||
        tc_read_reg(ctx, CSI_ERR_INTENA, 4, &csi_err_intena) != ESP_OK ||
        tc_read_reg(ctx, CSI_ERR_HALT, 4, &csi_err_halt) != ESP_OK) {
        ESP_LOGW(ctx->log_tag, "failed to read tc358743 CSI status");
        return;
    }

    ESP_LOGI(ctx->log_tag,
             "tc-csi ctl=0x%04" PRIx32 " int=0x%08" PRIx32 " int_ena=0x%08" PRIx32
             " err=0x%08" PRIx32 " err_ena=0x%08" PRIx32 " err_halt=0x%08" PRIx32,
             csi_control, csi_int, csi_int_ena, csi_err, csi_err_intena, csi_err_halt);
}

void tc_log_video_output(jetkvm_hdmi_bridge_t *ctx)
{
    uint32_t confctl = 0;
    uint32_t fifoctl = 0;
    uint32_t vout_set2 = 0;
    uint32_t vout_set3 = 0;
    uint32_t vi_rep = 0;
    uint32_t vi_mute = 0;
    uint32_t txoption = 0;
    uint32_t csi_status = 0;
    uint32_t de_h_lo = 0;
    uint32_t de_h_hi = 0;
    uint32_t de_v_lo = 0;
    uint32_t de_v_hi = 0;
    uint32_t h_size_lo = 0;
    uint32_t h_size_hi = 0;
    uint32_t v_size_lo = 0;
    uint32_t v_size_hi = 0;
    uint32_t clw = 0;
    uint32_t d0w = 0;
    uint32_t d1w = 0;
    uint32_t d2w = 0;
    uint32_t d3w = 0;
    uint32_t hstxvregen = 0;

    if (tc_read_reg(ctx, CONFCTL, 2, &confctl) != ESP_OK ||
        tc_read_reg(ctx, FIFOCTL, 2, &fifoctl) != ESP_OK ||
        tc_read_reg(ctx, VOUT_SET2, 1, &vout_set2) != ESP_OK ||
        tc_read_reg(ctx, VOUT_SET3, 1, &vout_set3) != ESP_OK ||
        tc_read_reg(ctx, VI_REP, 1, &vi_rep) != ESP_OK ||
        tc_read_reg(ctx, VI_MUTE, 1, &vi_mute) != ESP_OK ||
        tc_read_reg(ctx, TXOPTIONCNTRL, 4, &txoption) != ESP_OK ||
        tc_read_reg(ctx, CSI_STATUS, 2, &csi_status) != ESP_OK ||
        tc_read_reg(ctx, DE_WIDTH_H_LO, 1, &de_h_lo) != ESP_OK ||
        tc_read_reg(ctx, DE_WIDTH_H_HI, 1, &de_h_hi) != ESP_OK ||
        tc_read_reg(ctx, DE_WIDTH_V_LO, 1, &de_v_lo) != ESP_OK ||
        tc_read_reg(ctx, DE_WIDTH_V_HI, 1, &de_v_hi) != ESP_OK ||
        tc_read_reg(ctx, H_SIZE_LO, 1, &h_size_lo) != ESP_OK ||
        tc_read_reg(ctx, H_SIZE_HI, 1, &h_size_hi) != ESP_OK ||
        tc_read_reg(ctx, V_SIZE_LO, 1, &v_size_lo) != ESP_OK ||
        tc_read_reg(ctx, V_SIZE_HI, 1, &v_size_hi) != ESP_OK ||
        tc_read_reg(ctx, CLW_CNTRL, 4, &clw) != ESP_OK ||
        tc_read_reg(ctx, D0W_CNTRL, 4, &d0w) != ESP_OK ||
        tc_read_reg(ctx, D1W_CNTRL, 4, &d1w) != ESP_OK ||
        tc_read_reg(ctx, D2W_CNTRL, 4, &d2w) != ESP_OK ||
        tc_read_reg(ctx, D3W_CNTRL, 4, &d3w) != ESP_OK ||
        tc_read_reg(ctx, HSTXVREGEN, 4, &hstxvregen) != ESP_OK) {
        ESP_LOGW(ctx->log_tag, "failed to read tc358743 video output registers");
        return;
    }

    uint32_t raw_frame_width = h_size_lo | (h_size_hi << 8);
    uint32_t raw_frame_height = v_size_lo | (v_size_hi << 8);
    uint32_t frame_height_linux = raw_frame_height / 2;

    ESP_LOGI(ctx->log_tag,
             "tc-video confctl=0x%04" PRIx32 " fifoctl=%" PRIu32
             " vout2=0x%02" PRIx32 " vout3=0x%02" PRIx32
             " vi_rep=0x%02" PRIx32 " vi_mute=0x%02" PRIx32 " txopt=0x%08" PRIx32
             " csi_status=0x%04" PRIx32 " txact=%d rxact=%d hlt=%d wsync=%d"
             " de=%" PRIu32 "x%" PRIu32 " size_raw=%" PRIu32 "x%" PRIu32 " size_linux=%" PRIu32 "x%" PRIu32
             " lane_ctl=[clw=0x%08" PRIx32 " d0=0x%08" PRIx32 " d1=0x%08" PRIx32 " d2=0x%08" PRIx32 " d3=0x%08" PRIx32 "]"
             " hstxvregen=0x%08" PRIx32,
             confctl, fifoctl, vout_set2, vout_set3, vi_rep, vi_mute, txoption,
             csi_status,
             !!(csi_status & MASK_S_TXACT),
             !!(csi_status & MASK_S_RXACT),
             !!(csi_status & MASK_S_HLT),
             !!(csi_status & MASK_S_WSYNC),
             de_h_lo | (de_h_hi << 8),
             de_v_lo | (de_v_hi << 8),
             raw_frame_width,
             raw_frame_height,
             raw_frame_width,
             frame_height_linux,
             clw, d0w, d1w, d2w, d3w,
             hstxvregen);
}

void p4_log_csi_status(jetkvm_hdmi_bridge_t *ctx)
{
    ESP_LOGI(ctx->log_tag,
             "p4-csi brg raw=0x%08" PRIx32 " st=0x%08" PRIx32 " ena=0x%08" PRIx32
             " frame=0x%08" PRIx32 " dtype=0x%08" PRIx32 " flow=0x%08" PRIx32
             " depth=%" PRIu32 " clk_en=%d csi_en=%d host_ctrl=0x%08" PRIx32
             " cm=0x%08" PRIx32 " cm[en=%d bp=%d rx=%u yuv=%u tx=%u lane=%u sw16=%d sw8=%d]"
             " cm_size=0x%08" PRIx32 "[v=%u h=%u]"
             " dma_req=0x%08" PRIx32 " burst=%u updblk=%u flowctl=%u intv=%u blk=%u"
             " stop[d0=%d d1=%d clk=%d]"
             " lanes=%" PRIu32
             " host_main=0x%08" PRIx32 " phy_rx=0x%08" PRIx32 " phy_cal=0x%08" PRIx32
             " resetn=%d shutdownz=%d dphy_rstz=%d"
             " stop=0x%08" PRIx32
             " phy=0x%08" PRIx32 " phy_fatal=0x%08" PRIx32 " pkt_fatal=0x%08" PRIx32
             " bndry=0x%08" PRIx32 " seq=0x%08" PRIx32 " crc=0x%08" PRIx32
             " pld_crc=0x%08" PRIx32 " data_id=0x%08" PRIx32 " ecc_corr=0x%08" PRIx32,
             MIPI_CSI_BRIDGE.int_raw.val,
             MIPI_CSI_BRIDGE.int_st.val,
             MIPI_CSI_BRIDGE.int_ena.val,
             MIPI_CSI_BRIDGE.frame_cfg.val,
             MIPI_CSI_BRIDGE.data_type_cfg.val,
             MIPI_CSI_BRIDGE.buf_flow_ctl.val,
             MIPI_CSI_BRIDGE.buf_flow_ctl.csi_buf_depth,
             MIPI_CSI_BRIDGE.clk_en.clk_en,
             MIPI_CSI_BRIDGE.csi_en.csi_brg_en,
             MIPI_CSI_BRIDGE.host_ctrl.val,
             MIPI_CSI_BRIDGE.host_cm_ctrl.val,
             MIPI_CSI_BRIDGE.host_cm_ctrl.csi_host_cm_en,
             MIPI_CSI_BRIDGE.host_cm_ctrl.csi_host_cm_bypass,
             MIPI_CSI_BRIDGE.host_cm_ctrl.csi_host_cm_rx,
             MIPI_CSI_BRIDGE.host_cm_ctrl.csi_host_cm_rx_yuv422_format,
             MIPI_CSI_BRIDGE.host_cm_ctrl.csi_host_cm_tx,
             MIPI_CSI_BRIDGE.host_cm_ctrl.csi_host_cm_lane_num,
             MIPI_CSI_BRIDGE.host_cm_ctrl.csi_host_cm_16bit_swap,
             MIPI_CSI_BRIDGE.host_cm_ctrl.csi_host_cm_8bit_swap,
             MIPI_CSI_BRIDGE.host_size_ctrl.val,
             MIPI_CSI_BRIDGE.host_size_ctrl.csi_host_cm_vnum,
             MIPI_CSI_BRIDGE.host_size_ctrl.csi_host_cm_hnum,
             MIPI_CSI_BRIDGE.dma_req_cfg.val,
             MIPI_CSI_BRIDGE.dma_req_cfg.dma_burst_len,
             MIPI_CSI_BRIDGE.dma_req_cfg.dma_cfg_upd_by_blk,
             MIPI_CSI_BRIDGE.dma_req_cfg.csi_dma_flow_controller,
             MIPI_CSI_BRIDGE.dma_req_interval.dma_req_interval,
             MIPI_CSI_BRIDGE.dmablk_size.dmablk_size,
             MIPI_CSI_HOST.phy_stopstate.phy_stopstatedata_0,
             MIPI_CSI_HOST.phy_stopstate.phy_stopstatedata_1,
             MIPI_CSI_HOST.phy_stopstate.phy_stopstateclk,
             MIPI_CSI_HOST.n_lanes.n_lanes + 1,
             MIPI_CSI_HOST.int_st_main.val,
             MIPI_CSI_HOST.phy_rx.val,
             MIPI_CSI_HOST.phy_cal.val,
             MIPI_CSI_HOST.csi2_resetn.csi2_resetn,
             MIPI_CSI_HOST.phy_shutdownz.phy_shutdownz,
             MIPI_CSI_HOST.dphy_rstz.dphy_rstz,
             MIPI_CSI_HOST.phy_stopstate.val,
             MIPI_CSI_HOST.int_st_phy.val,
             MIPI_CSI_HOST.int_st_phy_fatal.val,
             MIPI_CSI_HOST.int_st_pkt_fatal.val,
             MIPI_CSI_HOST.int_st_bndry_frame_fatal.val,
             MIPI_CSI_HOST.int_st_seq_frame_fatal.val,
             MIPI_CSI_HOST.int_st_crc_frame_fatal.val,
             MIPI_CSI_HOST.int_st_pld_crc_fatal.val,
             MIPI_CSI_HOST.int_st_data_id.val,
             MIPI_CSI_HOST.int_st_ecc_corrected.val);
    for (int channel = 0; channel < 4; channel++) {
        volatile dmac_channel_reg_t *dma = &DW_GDMA.ch[channel];
        if ((DW_GDMA.chen0.val & BIT(channel)) || dma->cfg0.val || dma->int_st0.val) {
            ESP_LOGI(ctx->log_tag,
                     "p4-vdma ch=%d en=%d int=0x%08" PRIx32
                     " cfg0=0x%08" PRIx32 " cfg1=0x%08" PRIx32
                     " ctl0=0x%08" PRIx32 " ctl1=0x%08" PRIx32
                     " block_ts=%" PRIu32 " done=%" PRIu32 " fifo=%" PRIu32
                     " sar=0x%08" PRIx32 " dar=0x%08" PRIx32 " llp=0x%08" PRIx32,
                     channel, !!(DW_GDMA.chen0.val & BIT(channel)), dma->int_st0.val,
                     dma->cfg0.val, dma->cfg1.val, dma->ctl0.val, dma->ctl1.val,
                     dma->block_ts0.block_ts, dma->status0.cmpltd_blk_tfr_size,
                     dma->status1.data_left_in_fifo, dma->sar0.val, dma->dar0.val,
                     dma->llp0.val);
        }
    }
}

void p4_log_isp_status(const jetkvm_hdmi_bridge_t *ctx)
{
    if (!ctx || !ctx->isp) {
        /* ctx is an embedded member at every call site and cannot be NULL in
         * practice; keep the original NULL-tolerant guard without dereferencing
         * it by falling back to the historical tag value. */
        ESP_LOGI(ctx ? ctx->log_tag : "hdmi_csi", "p4-isp inactive");
        return;
    }
    ESP_LOGI(ctx->log_tag, "p4-isp cntl=0x%08" PRIx32 " frame_cfg=0x%08" PRIx32
             " hadr=%" PRIu32 " vadr=%" PRIu32 " int_raw=0x%08" PRIx32,
             ISP.cntl.val,
             ISP.frame_cfg.val,
             ISP.frame_cfg.hadr_num,
             ISP.frame_cfg.vadr_num,
             ISP.int_raw.val);
}

bool p4_wait_stopstate(jetkvm_hdmi_bridge_t *ctx, uint8_t lane_num, uint32_t timeout_ms)
{
    TickType_t start = xTaskGetTickCount();
    TickType_t timeout_ticks = pdMS_TO_TICKS(timeout_ms);

    while ((xTaskGetTickCount() - start) < timeout_ticks) {
        bool clk = MIPI_CSI_HOST.phy_stopstate.phy_stopstateclk;
        bool data0 = MIPI_CSI_HOST.phy_stopstate.phy_stopstatedata_0;
        bool data1 = lane_num < 2 || MIPI_CSI_HOST.phy_stopstate.phy_stopstatedata_1;

        if (clk && data0 && data1) {
            ESP_LOGI(ctx->log_tag, "p4-csi stopstate ready lane_num=%u stop=0x%08" PRIx32,
                     lane_num, MIPI_CSI_HOST.phy_stopstate.val);
            return true;
        }
        vTaskDelay(pdMS_TO_TICKS(1));
    }

    ESP_LOGW(ctx->log_tag, "p4-csi stopstate timeout lane_num=%u stop=0x%08" PRIx32 " phy_rx=0x%08" PRIx32,
             lane_num, MIPI_CSI_HOST.phy_stopstate.val, MIPI_CSI_HOST.phy_rx.val);
    return false;
}

/**
 * Consume at most one EDID command from the capture-owner task.
 *
 * This is deliberately the only function that applies a runtime EDID.  RPC,
 * HTTP, WebRTC, and NVS tasks never touch TC358743 I2C state.  It disables TC
 * output but leaves the active CSI/peer generation alive long enough for the
 * RPC response: csi_destroy() waits for that same peer task and would deadlock
 * if called while setEDID is waiting for this owner.  The disabled stream then
 * forces the normal capture timeout/mode-change path to rebuild CSI.
 */
bool p4_edid_control_process(jetkvm_hdmi_bridge_t *ctx)
{
    if (!__atomic_exchange_n(&s_edid_control.command_pending, false,
                             __ATOMIC_ACQ_REL)) {
        return false;
    }

    const jetkvm_video_edid_preset_t requested =
        (jetkvm_video_edid_preset_t)__atomic_load_n(
            &s_edid_control.requested_preset, __ATOMIC_ACQUIRE);
    const jetkvm_video_edid_preset_t previous =
        (jetkvm_video_edid_preset_t)__atomic_load_n(
            &s_edid_control.applied_preset, __ATOMIC_ACQUIRE);
    const p4_edid_descriptor_t *target = p4_edid_descriptor(requested);
    const p4_edid_descriptor_t *rollback = p4_edid_descriptor(previous);
    esp_err_t error = target ? ESP_OK : ESP_ERR_INVALID_ARG;

    ESP_LOGI(ctx->log_tag, "EDID owner apply requested=%u previous=%u",
             (unsigned)requested, (unsigned)previous);
    if (error == ESP_OK) {
        error = tc_enable_stream(ctx, false);
    }
    if (error == ESP_OK) {
        error = tc_program_edid(ctx, target);
    }
    if (error == ESP_OK) {
        __atomic_store_n(&s_edid_control.applied_preset,
                         (uint32_t)requested, __ATOMIC_RELEASE);
        ESP_LOGI(ctx->log_tag, "EDID owner applied preset=%s", target->label);
    } else {
        ESP_LOGE(ctx->log_tag, "EDID owner apply failed requested=%u: %s",
                 (unsigned)requested, esp_err_to_name(error));
        if (target && rollback && requested != previous) {
            esp_err_t rollback_error = tc_program_edid(ctx, rollback);
            if (rollback_error != ESP_OK) {
                ESP_LOGE(ctx->log_tag,
                         "EDID owner hardware rollback failed preset=%s: %s",
                         rollback->label, esp_err_to_name(rollback_error));
            } else {
                ESP_LOGW(ctx->log_tag,
                         "EDID owner restored previous hardware preset=%s",
                         rollback->label);
            }
        }
    }

    if (s_edid_apply_result_fn) {
        s_edid_apply_result_fn(requested,
                               (jetkvm_video_edid_preset_t)__atomic_load_n(
                                   &s_edid_control.applied_preset, __ATOMIC_ACQUIRE),
                               error, s_edid_apply_result_ctx);
    }
    __atomic_store_n(&s_edid_control.result, error, __ATOMIC_RELEASE);
    __atomic_store_n(&s_edid_control.in_flight, false, __ATOMIC_RELEASE);
    xSemaphoreGive(s_edid_control.completion);
    return true;
}
