/*
 * SPDX-FileCopyrightText: 2023-2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <esp_types.h>
#include <sys/lock.h>
#include "sdkconfig.h"
#include "esp_log.h"
#include "esp_check.h"
#include "esp_heap_caps.h"
#include "freertos/FreeRTOS.h"
#include "freertos/idf_additions.h"
#include "freertos/task.h"
#include "esp_memory_utils.h"
#include "esp_clk_tree.h"
#include "esp_cam_ctlr.h"
#include "esp_cam_ctlr_csi.h"
#include "esp_cam_ctlr_interface.h"
#include "p4_csi_capture.h"
#include "p4_cam_ctlr_csi_internal.h"
#include "hal/mipi_csi_ll.h"
#include "hal/color_hal.h"
#include "hal/efuse_hal.h"
#include "hal/dw_gdma_ll.h"
#include "soc/chip_revision.h"
#include "esp_private/periph_ctrl.h"
#include "esp_private/mipi_csi_share_hw_ctrl.h"
#include "esp_private/esp_cache_private.h"
#include "esp_private/esp_clk_tree_common.h"
#include "esp_cache.h"
#include "esp_rom_sys.h"

#define CSI_DMA_STOP_TIMEOUT_MS 20
_Static_assert(DW_GDMA_LL_GROUPS == 1, "CSI DMA register access requires the P4 single VDMA group");

#if CONFIG_CAM_CTLR_MIPI_CSI_ISR_CACHE_SAFE
#define CSI_MEM_ALLOC_CAPS   (MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT)
#else
#define CSI_MEM_ALLOC_CAPS   MALLOC_CAP_DEFAULT
#endif

typedef struct csi_platform_t {
    _lock_t         mutex;
    csi_controller_t *controllers[MIPI_CSI_LL_HOST_CTLR_NUMS];
    soc_module_clk_t phy_clk_sources[MIPI_CSI_LL_HOST_CTLR_NUMS];
    bool phy_clk_source_enabled[MIPI_CSI_LL_HOST_CTLR_NUMS];
} csi_platform_t;

static const char *TAG = "p4_csi";
static csi_platform_t s_platform;

static bool csi_dma_trans_done_callback(dw_gdma_channel_handle_t chan, const dw_gdma_trans_done_event_data_t *event_data, void *user_data);
static esp_err_t s_del_csi_ctlr(csi_controller_t *ctlr);
static esp_err_t s_ctlr_del(esp_cam_ctlr_t *cam_ctlr);
static esp_err_t s_register_event_callbacks(esp_cam_ctlr_handle_t handle, const esp_cam_ctlr_evt_cbs_t *cbs, void *user_data);
static esp_err_t s_csi_ctlr_get_internal_buffer(esp_cam_ctlr_handle_t handle, uint32_t fb_num, const void **fb0, ...);
static esp_err_t s_csi_ctlr_get_buffer_length(esp_cam_ctlr_handle_t handle, size_t *ret_fb_len);
static esp_err_t s_csi_ctlr_enable(esp_cam_ctlr_handle_t ctlr);
static esp_err_t s_ctlr_csi_start(esp_cam_ctlr_handle_t handle);
static esp_err_t s_ctlr_csi_stop(esp_cam_ctlr_handle_t handle);
static esp_err_t s_csi_ctlr_disable(esp_cam_ctlr_handle_t ctlr);
static esp_err_t s_ctlr_csi_receive(esp_cam_ctlr_handle_t handle, esp_cam_ctlr_trans_t *trans, uint32_t timeout_ms);
static void *s_csi_ctlr_alloc_buffer(esp_cam_ctlr_t *handle, size_t size, uint32_t buf_caps);
static esp_err_t s_csi_ctlr_format_conversion(esp_cam_ctlr_t *handle, const cam_ctlr_format_conv_config_t *config);
static bool s_is_color_format_conversion_supported(cam_ctlr_color_t color_format);

static void s_save_first_error(esp_err_t *first_error, esp_err_t error, const char *operation)
{
    if (error == ESP_OK) {
        return;
    }
    ESP_LOGE(TAG, "%s failed during CSI cleanup: %s", operation, esp_err_to_name(error));
    if (*first_error == ESP_OK) {
        *first_error = error;
    }
}

static int csi_dma_control_core(void)
{
#if CONFIG_FREERTOS_UNICORE
    return 0;
#elif CONFIG_FREERTOS_SMP
    UBaseType_t affinity = vTaskCoreAffinityGet(NULL);
    return __builtin_popcount(affinity) == 1 ? __builtin_ctz(affinity) : -1;
#else
    return xTaskGetCoreID(NULL);
#endif
}

static bool IRAM_ATTR csi_dma_callback_enter(csi_controller_t *ctlr)
{
    portENTER_CRITICAL_ISR(&ctlr->spinlock);
    bool allowed = !ctlr->dma_stopping;
    if (allowed) {
        ++ctlr->dma_callbacks_active;
    }
    portEXIT_CRITICAL_ISR(&ctlr->spinlock);
    return allowed;
}

static void IRAM_ATTR csi_dma_callback_exit(csi_controller_t *ctlr)
{
    portENTER_CRITICAL_ISR(&ctlr->spinlock);
    assert(ctlr->dma_callbacks_active != 0);
    --ctlr->dma_callbacks_active;
    portEXIT_CRITICAL_ISR(&ctlr->spinlock);
}

/* EN=0 requests a stop; it does not complete it. TRM 7.5.7 and CHEN0's
 * register description require the final AXI transfer to finish before EN
 * reads zero. Keep the ring, raw destinations and callback context owned
 * until both hardware and any callback already owning a descriptor drain.
 * The raw DISABLED event alone is insufficient: a previous stop may have left
 * it set, and IDF's default ISR clears every status bit it observes. */
static esp_err_t csi_dma_quiesce(csi_controller_t *ctlr)
{
    /* IDF reads IRQ status before entering our callback. A callback counter
     * alone cannot cover that outer ISR window on another CPU. Lifecycle
     * calls therefore stay on the pinned task core that registered the ISR;
     * masking propagation inside its critical section drains the whole ISR
     * without an unbounded cross-core IPC wait. */
    ESP_RETURN_ON_FALSE(csi_dma_control_core() == ctlr->dma_owner_core,
                        ESP_ERR_NOT_ALLOWED, TAG, "CSI lifecycle must run on its IRQ owner core");
    if (!ctlr->dma_chan) {
        if (ctlr->hal.bridge_dev) {
            mipi_csi_brg_ll_enable(ctlr->hal.bridge_dev, false);
        }
        return ESP_OK;
    }

    int channel_id = -1;
    esp_err_t id_ret = dw_gdma_channel_get_id(ctlr->dma_chan, &channel_id);
    /* The live handle was allocated by this driver. Fail closed on a broken
     * invariant rather than returning a secondary, handle-consuming error. */
    assert(id_ret == ESP_OK);
    (void)id_ret;
    ESP_RETURN_ON_FALSE(channel_id >= 0 && channel_id < DW_GDMA_LL_CHANNELS_PER_GROUP,
                        ESP_ERR_TIMEOUT, TAG, "invalid CSI DMA channel; retaining resources");
    dw_gdma_dev_t *dev = DW_GDMA_LL_GET_HW(0);
    const uint32_t channel_mask = 1U << channel_id;
    portENTER_CRITICAL(&ctlr->spinlock);
    ctlr->dma_stopping = true;
    if (!ctlr->dma_irq_mask_saved) {
        ctlr->dma_saved_irq_mask = dev->ch[channel_id].int_sig_ena0.val;
        ctlr->dma_irq_mask_saved = true;
    }
    dw_gdma_ll_channel_enable_intr_propagation(dev, channel_id, UINT32_MAX, false);
    portEXIT_CRITICAL(&ctlr->spinlock);
    if (ctlr->hal.bridge_dev) {
        mipi_csi_brg_ll_enable(ctlr->hal.bridge_dev, false);
    }

    /* A never-started or already disabled channel need not generate another
     * DISABLED edge. An INVALID-LLI halt still has EN=1 and must be disabled. */
    if (dev->chen0.val & channel_mask) {
        esp_err_t ret = dw_gdma_channel_enable_ctrl(ctlr->dma_chan, false);
        ESP_RETURN_ON_FALSE(ret == ESP_OK, ESP_ERR_TIMEOUT,
                            TAG, "failed to request CSI DMA stop; retaining resources");
    }

    const TickType_t started = xTaskGetTickCount();
    const TickType_t timeout = pdMS_TO_TICKS(CSI_DMA_STOP_TIMEOUT_MS) > 0 ?
        pdMS_TO_TICKS(CSI_DMA_STOP_TIMEOUT_MS) : 1;
    for (;;) {
        portENTER_CRITICAL(&ctlr->spinlock);
        const uint32_t callbacks_active = ctlr->dma_callbacks_active;
        portEXIT_CRITICAL(&ctlr->spinlock);
        const bool hardware_active = (dev->chen0.val & channel_mask) != 0;
        if (!hardware_active && callbacks_active == 0) {
            break;
        }
        if ((TickType_t)(xTaskGetTickCount() - started) >= timeout) {
            ESP_LOGE(TAG, "CSI DMA stop timeout channel=%d enabled=%d callbacks=%" PRIu32,
                     channel_id, hardware_active, callbacks_active);
            /* Keep IRQs masked and stopping set. A caller may retry stop or
             * delete, but must not restart, return buffers, or free the ring. */
            return ESP_ERR_TIMEOUT;
        }
        esp_rom_delay_us(1);
    }
    /* No producer or old callback can create a new EOF after this point. */
    dw_gdma_ll_channel_clear_intr(dev, channel_id, UINT32_MAX);
    return ESP_OK;
}

static esp_err_t csi_dma_resume_callbacks(csi_controller_t *ctlr)
{
    int channel_id = -1;
    ESP_RETURN_ON_ERROR(dw_gdma_channel_get_id(ctlr->dma_chan, &channel_id),
                        TAG, "failed to identify CSI DMA channel");
    ESP_RETURN_ON_FALSE(channel_id >= 0 && channel_id < DW_GDMA_LL_CHANNELS_PER_GROUP,
                        ESP_ERR_INVALID_STATE, TAG, "invalid CSI DMA channel");
    dw_gdma_dev_t *dev = DW_GDMA_LL_GET_HW(0);
    portENTER_CRITICAL(&ctlr->spinlock);
    bool idle = !(dev->chen0.val & (1U << channel_id)) &&
                ctlr->dma_callbacks_active == 0 && ctlr->dma_irq_mask_saved;
    if (idle) {
        dw_gdma_ll_channel_clear_intr(dev, channel_id, UINT32_MAX);
        ctlr->dma_stopping = false;
        dev->ch[channel_id].int_sig_ena0.val = ctlr->dma_saved_irq_mask;
        ctlr->dma_irq_mask_saved = false;
    }
    portEXIT_CRITICAL(&ctlr->spinlock);
    ESP_RETURN_ON_FALSE(idle, ESP_ERR_INVALID_STATE, TAG, "CSI DMA callbacks resumed before quiescence");
    return ESP_OK;
}

static esp_err_t s_csi_claim_controller(csi_controller_t *controller)
{
    assert(controller);

    _lock_acquire(&s_platform.mutex);
    bool found = false;
    for (int i = 0; i < MIPI_CSI_LL_HOST_CTLR_NUMS; i ++) {
        found = !s_platform.controllers[i];
        if (found) {
            s_platform.controllers[i] = controller;
            s_platform.phy_clk_source_enabled[i] = false;
            controller->csi_id = i;
            PERIPH_RCC_ATOMIC() {
                mipi_csi_ll_enable_host_bus_clock(i, 0);
                mipi_csi_ll_enable_host_bus_clock(i, 1);
                mipi_csi_ll_reset_host_clock(i);
            }
            break;
        }
    }
    _lock_release(&s_platform.mutex);

    if (!found) {
        return ESP_ERR_NOT_FOUND;
    }
    return ESP_OK;
}

static esp_err_t s_csi_declaim_controller(csi_controller_t *controller)
{
    assert(controller);

    int csi_id = controller->csi_id;
    ESP_RETURN_ON_FALSE(csi_id >= 0 && csi_id < MIPI_CSI_LL_HOST_CTLR_NUMS,
                        ESP_ERR_INVALID_STATE, TAG, "invalid CSI controller id");

    _lock_acquire(&s_platform.mutex);
    esp_err_t ret = ESP_OK;
    if (s_platform.controllers[csi_id] == controller) {
        s_platform.controllers[csi_id] = NULL;
        s_platform.phy_clk_source_enabled[csi_id] = false;
        PERIPH_RCC_ATOMIC() {
            mipi_csi_ll_enable_host_bus_clock(csi_id, 0);
        }
        controller->csi_id = -1;
    } else if (s_platform.controllers[csi_id] != NULL) {
        ret = ESP_ERR_INVALID_STATE;
    }
    _lock_release(&s_platform.mutex);

    return ret;
}

static esp_err_t s_csi_enable_phy_clock_source(csi_controller_t *controller, soc_module_clk_t clk_src)
{
    esp_err_t ret = esp_clk_tree_enable_src(clk_src, true);
    if (ret != ESP_OK) {
        return ret;
    }

    _lock_acquire(&s_platform.mutex);
    int csi_id = controller->csi_id;
    if (csi_id < 0 || csi_id >= MIPI_CSI_LL_HOST_CTLR_NUMS ||
        s_platform.controllers[csi_id] != controller) {
        ret = ESP_ERR_INVALID_STATE;
    } else {
        s_platform.phy_clk_sources[csi_id] = clk_src;
        s_platform.phy_clk_source_enabled[csi_id] = true;
    }
    _lock_release(&s_platform.mutex);

    if (ret != ESP_OK) {
        (void)esp_clk_tree_enable_src(clk_src, false);
    }
    return ret;
}

static esp_err_t s_csi_disable_phy_clock_source(csi_controller_t *controller)
{
    int csi_id = controller->csi_id;
    ESP_RETURN_ON_FALSE(csi_id >= 0 && csi_id < MIPI_CSI_LL_HOST_CTLR_NUMS,
                        ESP_ERR_INVALID_STATE, TAG, "invalid CSI controller id");

    _lock_acquire(&s_platform.mutex);
    bool enabled = s_platform.controllers[csi_id] == controller &&
                   s_platform.phy_clk_source_enabled[csi_id];
    soc_module_clk_t clk_src = s_platform.phy_clk_sources[csi_id];
    _lock_release(&s_platform.mutex);
    if (!enabled) {
        return ESP_OK;
    }

    esp_err_t ret = esp_clk_tree_enable_src(clk_src, false);
    if (ret == ESP_OK) {
        _lock_acquire(&s_platform.mutex);
        if (s_platform.controllers[csi_id] == controller) {
            s_platform.phy_clk_source_enabled[csi_id] = false;
        }
        _lock_release(&s_platform.mutex);
    }
    return ret;
}

esp_err_t p4_cam_new_csi_ctlr(const esp_cam_ctlr_csi_config_t *config, esp_cam_ctlr_handle_t *ret_handle)
{
    return p4_cam_new_csi_ctlr_with_dma_ring(
        config, P4_CSI_DMA_RING_DEFAULT, ret_handle);
}

esp_err_t p4_cam_new_csi_ctlr_with_dma_ring(
    const esp_cam_ctlr_csi_config_t *config, size_t descriptor_count,
    esp_cam_ctlr_handle_t *ret_handle)
{
    esp_err_t ret = ESP_FAIL;
    ESP_RETURN_ON_FALSE(config && ret_handle, ESP_ERR_INVALID_ARG, TAG, "invalid argument: null pointer");
    *ret_handle = NULL;
    ESP_RETURN_ON_FALSE(
        descriptor_count == P4_CSI_DMA_RING_COMPACT ||
        descriptor_count == P4_CSI_DMA_RING_DEFAULT,
        ESP_ERR_INVALID_ARG, TAG, "CSI DMA ring must contain 2 or 3 descriptors");
    const int owner_core = csi_dma_control_core();
    ESP_RETURN_ON_FALSE(owner_core >= 0 && owner_core < portNUM_PROCESSORS,
                        ESP_ERR_NOT_ALLOWED, TAG, "CSI construction requires a task pinned to one core");
    ESP_RETURN_ON_FALSE(config->data_lane_num <= MIPI_CSI_HOST_LL_LANE_NUM_MAX, ESP_ERR_INVALID_ARG, TAG, "lane num should be equal or smaller than %d", MIPI_CSI_HOST_LL_LANE_NUM_MAX);
    if (!config->data_type.bits_per_pixel) {
        ESP_RETURN_ON_FALSE(config->input_data_color_type != 0, ESP_ERR_INVALID_ARG, TAG, "input_data_color_type must be specified");
        ESP_RETURN_ON_FALSE(config->output_data_color_type != 0, ESP_ERR_INVALID_ARG, TAG, "output_data_color_type must be specified");
    }

    bool is_less_v1 = false;  //version 1 since P4 rev3
#if CONFIG_IDF_TARGET_ESP32P4
    unsigned chip_version = efuse_hal_chip_revision();
    if (!ESP_CHIP_REV_ABOVE(chip_version, 300)) {
        is_less_v1 = true;
    }
#endif
    ESP_RETURN_ON_FALSE(!(is_less_v1 && (config->input_8bit_swap_en || config->input_16bit_swap_en)), ESP_ERR_NOT_SUPPORTED, TAG, "input 8bit or 16bit swap is not supported on this chip");

    csi_controller_t *ctlr = heap_caps_calloc(1, sizeof(csi_controller_t), CSI_MEM_ALLOC_CAPS);
    ESP_RETURN_ON_FALSE(ctlr, ESP_ERR_NO_MEM, TAG, "no mem for csi controller context");
    ctlr->csi_id = -1;
    ctlr->dma_owner_core = (int8_t)owner_core;
    ctlr->dma_lli_count = (uint8_t)descriptor_count;
    ctlr->dma_stopping = true;
    ctlr->spinlock = (portMUX_TYPE)portMUX_INITIALIZER_UNLOCKED;

    ret = s_csi_claim_controller(ctlr);
    if (ret != ESP_OK) {
        //claim fail, clean and return directly
        free(ctlr);
        ESP_RETURN_ON_ERROR(ret, TAG, "no available csi controller");
    }
    /* From this point every failure goes through s_del_csi_ctlr().  Mark the
     * object deletable immediately; setting the FSM only after full creation
     * made partial-init failures leak the sole P4 controller slot forever. */
    ctlr->csi_fsm = CSI_FSM_INIT;

    ESP_LOGD(TAG, "config->queue_items: %d", config->queue_items);
    ctlr->trans_que = xQueueCreateWithCaps(config->queue_items, sizeof(esp_cam_ctlr_trans_t), MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    ESP_GOTO_ON_FALSE(ctlr->trans_que, ESP_ERR_NO_MEM, err, TAG, "no memory for transaction queue");

#if SOC_ISP_SHARE_CSI_BRG
    ESP_GOTO_ON_ERROR(mipi_csi_brg_claim(MIPI_CSI_BRG_USER_CSI, &ctlr->csi_brg_id), err, TAG, "csi bridge is in use already");
    ctlr->csi_brg_in_use = true;
#endif

    mipi_csi_phy_clock_source_t clk_src = !config->clk_src ? MIPI_CSI_PHY_CLK_SRC_DEFAULT : config->clk_src;
    ESP_GOTO_ON_ERROR(s_csi_enable_phy_clock_source(ctlr, (soc_module_clk_t)clk_src), err,
                      TAG, "clock source enable failed");
    PERIPH_RCC_ATOMIC() {
        // phy clock source setting
        mipi_csi_ll_set_phy_clock_source(ctlr->csi_id, clk_src);
        // phy clock reset
        mipi_csi_ll_enable_phy_config_clock(ctlr->csi_id, 0);
        mipi_csi_ll_enable_phy_config_clock(ctlr->csi_id, 1);
    }

    ctlr->h_res = config->h_res;
    ctlr->v_res = config->v_res;
    ESP_LOGD(TAG, "ctlr->h_res: 0d %"PRId32, ctlr->h_res);
    ESP_LOGD(TAG, "ctlr->v_res: 0d %"PRId32, ctlr->v_res);

    ctlr->custom_data_depth = config->data_type.bits_per_pixel;
    //in color type
    if (!config->data_type.bits_per_pixel) {
        int in_bits_per_pixel = color_hal_pixel_format_fourcc_get_bit_depth(config->input_data_color_type);
        ESP_GOTO_ON_FALSE(in_bits_per_pixel != 0, ESP_ERR_INVALID_ARG, err, TAG, "unsupported input color format");
        ctlr->in_color_format = config->input_data_color_type;
        ctlr->in_bpp = in_bits_per_pixel;
    } else {
        ctlr->in_bpp = config->data_type.bits_per_pixel;
    }
    ESP_LOGD(TAG, "ctlr->in_bpp: 0d %d", ctlr->in_bpp);

    if (!config->data_type.bits_per_pixel) {
        //out color type
        int out_bits_per_pixel = color_hal_pixel_format_fourcc_get_bit_depth(config->output_data_color_type);

        ESP_GOTO_ON_FALSE(out_bits_per_pixel != 0, ESP_ERR_INVALID_ARG, err, TAG, "unsupported output color format");
        ctlr->out_color_format = config->output_data_color_type;
        ctlr->out_bpp = out_bits_per_pixel;
    } else {
        ctlr->out_bpp = config->data_type.bits_per_pixel;
    }
    ESP_LOGD(TAG, "ctlr->out_bpp: 0d %d", ctlr->out_bpp);

    // Note: Width * Height * BitsPerPixel must be divisible by 8
    int fb_size_in_bits = config->v_res * config->h_res * ctlr->out_bpp;
    ESP_GOTO_ON_FALSE((fb_size_in_bits % 8 == 0), ESP_ERR_INVALID_ARG, err, TAG, "framesize not 8bit aligned");
    ctlr->fb_size_in_bytes = fb_size_in_bits / 8;
    ESP_LOGD(TAG, "ctlr->fb_size_in_bytes=%d", ctlr->fb_size_in_bytes);

    ctlr->bk_buffer_dis = config->bk_buffer_dis;
    if (!ctlr->bk_buffer_dis) {
        size_t dma_alignment = 4;  //TODO: IDF-9126, replace with dwgdma alignment API
        size_t cache_alignment = 1;
        ESP_GOTO_ON_ERROR(esp_cache_get_alignment(MALLOC_CAP_SPIRAM | MALLOC_CAP_DMA, &cache_alignment), err, TAG, "failed to get cache alignment");
        size_t alignment = MAX(cache_alignment, dma_alignment);
        ESP_LOGD(TAG, "alignment: 0x%x\n", alignment);

        /* The application bounds a CSI DMA block to the frame rounded up to
         * whole bridge bursts; the backup buffer is a DMA destination too. */
        const size_t backup_bytes =
            (ctlr->fb_size_in_bytes + P4_CSI_BACKUP_BUFFER_ALIGN_BYTES - 1U) /
            P4_CSI_BACKUP_BUFFER_ALIGN_BYTES * P4_CSI_BACKUP_BUFFER_ALIGN_BYTES;
        ctlr->backup_buffer = heap_caps_aligned_alloc(alignment, backup_bytes, MALLOC_CAP_SPIRAM);
        ESP_GOTO_ON_FALSE(ctlr->backup_buffer, ESP_ERR_NO_MEM, err, TAG, "no mem for backup buffer");
        ESP_LOGD(TAG, "ctlr->backup_buffer: %p\n", ctlr->backup_buffer);
        esp_cache_msync((void *)(ctlr->backup_buffer), ctlr->fb_size_in_bytes, ESP_CACHE_MSYNC_FLAG_DIR_C2M);
    }

    mipi_csi_hal_config_t hal_config;
    hal_config.frame_height = config->h_res;
    hal_config.frame_width = config->v_res;
    hal_config.lane_bit_rate_mbps = config->lane_bit_rate_mbps;
    hal_config.lanes_num = config->data_lane_num;
    hal_config.byte_swap_en = config->byte_swap_en;
    mipi_csi_hal_init(&ctlr->hal, &hal_config);
    mipi_csi_brg_ll_set_burst_len(ctlr->hal.bridge_dev, 64);
    if (!config->data_type.bits_per_pixel) {
        //for yuv, rgb and raw
        mipi_csi_brg_ll_set_data_type_min(ctlr->hal.bridge_dev, 0x12);
        mipi_csi_brg_ll_set_data_type_max(ctlr->hal.bridge_dev, 0x2f);
    } else {
        mipi_csi_brg_ll_set_data_type_min(ctlr->hal.bridge_dev, config->data_type.data_type_min);
        mipi_csi_brg_ll_set_data_type_max(ctlr->hal.bridge_dev, config->data_type.data_type_max);
    }
    const bool color_conversion_required =
        config->input_data_color_type != config->output_data_color_type;
    /* Conversion parameters must be programmed before its first enable edge.
     * For a same-format bypass, retain the stock driver's ordering: CM is
     * already enabled when BYPASS is selected. */
    mipi_csi_brg_ll_enable_color_conversion(
        ctlr->hal.bridge_dev, !color_conversion_required);

    cam_ctlr_format_conv_config_t format_conv_config = {
        .src_format = config->input_data_color_type,
        .dst_format = config->output_data_color_type,
    };
    ESP_GOTO_ON_ERROR(s_csi_ctlr_format_conversion(&(ctlr->base), &format_conv_config), err, TAG, "failed to configure format conversion");

    /* ESP-IDF currently leaves HOST_SIZE_CTRL at reset. Program the complete
     * packed-YUV422 IDI32 input dimensions before the first CSI_CM edge. */
    if ((config->input_data_color_type == CAM_CTLR_COLOR_YUV422_YVYU ||
         config->input_data_color_type == CAM_CTLR_COLOR_YUV422_YUYV ||
         config->input_data_color_type == CAM_CTLR_COLOR_YUV422_UYVY ||
         config->input_data_color_type == CAM_CTLR_COLOR_YUV422_VYUY) &&
        config->output_data_color_type == CAM_CTLR_COLOR_YUV420) {
        ctlr->hal.bridge_dev->host_size_ctrl.csi_host_cm_vnum = config->v_res - 1U;
        ctlr->hal.bridge_dev->host_size_ctrl.csi_host_cm_hnum =
            ((uint32_t)config->h_res * 16U / 32U) - 1U;
    }

    /* Write every CSI_CM field explicitly.  The bridge is shared with ISP
     * and survives software mode rebuilds, so relying on reset values can
     * leak a previous mode's byte order into the next stream. */
    mipi_csi_brg_ll_set_8bit_swap(ctlr->hal.bridge_dev,
                                  config->input_8bit_swap_en);
    mipi_csi_brg_ll_set_16bit_swap(ctlr->hal.bridge_dev,
                                   config->input_16bit_swap_en);
    /* This field encodes the active CSI input lane count as N - 1.  Leaving
     * it at zero on a two-lane YUV stream makes CSI_CM terminate the frame
     * early; a two-lane stream needs lane_num == 1. */
    ctlr->hal.bridge_dev->host_cm_ctrl.csi_host_cm_lane_num =
        config->data_lane_num > 1;
    mipi_csi_brg_ll_enable_color_conversion(ctlr->hal.bridge_dev, true);
    //---------------DWGDMA Init For CSI------------------//
    dw_gdma_channel_handle_t csi_dma_chan = NULL;
    dw_gdma_channel_alloc_config_t csi_dma_alloc_config = {
        .src = {
            .block_transfer_type = DW_GDMA_BLOCK_TRANSFER_LIST,
            .role = DW_GDMA_ROLE_PERIPH_CSI,
            .handshake_type = DW_GDMA_HANDSHAKE_HW,
            .num_outstanding_requests = 5,
            .status_fetch_addr = MIPI_CSI_BRG_MEM_BASE,
        },
        .dst = {
            .block_transfer_type = DW_GDMA_BLOCK_TRANSFER_LIST,
            .role = DW_GDMA_ROLE_MEM,
            .handshake_type = DW_GDMA_HANDSHAKE_HW,
            .num_outstanding_requests = 5,
        },
        .flow_controller = DW_GDMA_FLOW_CTRL_SRC,
        .chan_priority = 1,
    };
    ESP_GOTO_ON_ERROR(dw_gdma_new_channel(&csi_dma_alloc_config, &csi_dma_chan), err, TAG, "failed to new dwgdma channel");
    ctlr->dma_chan = csi_dma_chan;
    dw_gdma_link_list_config_t link_list_config = {
        .num_items = ctlr->dma_lli_count,
        .link_type = DW_GDMA_LINKED_LIST_TYPE_CIRCULAR,
    };
    ESP_GOTO_ON_ERROR(dw_gdma_new_link_list(&link_list_config, &ctlr->dma_link_list), err,
                      TAG, "failed to allocate dwgdma link list");

    /* DW-GDMA reads the CSI bridge's post-conversion stream.  For conversions
     * such as packed YUV422 (16 bpp) -> O_UYY_E_VYY YUV420 (12 bpp), sizing
     * the block from the input depth waits for beats the bridge never emits,
     * so no transaction-complete callback can fire. */
    size_t csi_transfer_size = ctlr->h_res * ctlr->v_res * ctlr->out_bpp / 64;
    ctlr->csi_transfer_size = csi_transfer_size;
    ESP_LOGD(TAG, "csi_transfer_size: 0d %d", csi_transfer_size);

    dw_gdma_event_callbacks_t csi_dma_cbs = {
        .on_block_trans_done = csi_dma_trans_done_callback,
    };
    ESP_GOTO_ON_ERROR(dw_gdma_channel_register_event_callbacks(csi_dma_chan, &csi_dma_cbs, ctlr), err, TAG, "failed to register dwgdma callback");

#if CONFIG_PM_ENABLE
    ESP_GOTO_ON_ERROR(esp_pm_lock_create(ESP_PM_APB_FREQ_MAX, 0, "cam_csi_ctlr", &ctlr->pm_lock), err, TAG, "failed to create pm lock");
#endif //CONFIG_PM_ENABLE

    ctlr->base.del = s_ctlr_del;
    ctlr->base.enable = s_csi_ctlr_enable;
    ctlr->base.start = s_ctlr_csi_start;
    ctlr->base.stop = s_ctlr_csi_stop;
    ctlr->base.disable = s_csi_ctlr_disable;
    ctlr->base.receive = s_ctlr_csi_receive;
    ctlr->base.register_event_callbacks = s_register_event_callbacks;
    ctlr->base.get_internal_buffer = s_csi_ctlr_get_internal_buffer;
    ctlr->base.get_buffer_len = s_csi_ctlr_get_buffer_length;
    ctlr->base.alloc_buffer = s_csi_ctlr_alloc_buffer;
    ctlr->base.format_conversion = s_csi_ctlr_format_conversion;

    *ret_handle = &(ctlr->base);

    return ESP_OK;

err:
    {
        esp_err_t cleanup_ret = s_del_csi_ctlr(ctlr);
        if (cleanup_ret != ESP_OK) {
            ESP_LOGE(TAG, "partial CSI controller cleanup failed: %s", esp_err_to_name(cleanup_ret));
        }
    }

    return ret;
}

esp_err_t s_del_csi_ctlr(csi_controller_t *ctlr)
{
    ESP_RETURN_ON_FALSE(ctlr, ESP_ERR_INVALID_ARG, TAG, "invalid argument: null pointer");
    ESP_RETURN_ON_FALSE(ctlr->csi_fsm >= CSI_FSM_INIT && ctlr->csi_fsm <= CSI_FSM_STARTED,
                        ESP_ERR_INVALID_STATE, TAG, "invalid CSI controller state");

    esp_err_t first_error = ESP_OK;

    /* On a quiescence failure this deleter deliberately retains the handle
     * and every DMA-owned allocation; ordinary secondary cleanup errors
     * below still consume the handle as before. */
    ESP_RETURN_ON_ERROR(csi_dma_quiesce(ctlr), TAG, "CSI DMA still owns resources");

#if CONFIG_PM_ENABLE
    if (ctlr->csi_fsm != CSI_FSM_INIT && ctlr->pm_lock) {
        s_save_first_error(&first_error, esp_pm_lock_release(ctlr->pm_lock),
                           "release pm lock");
    }
#endif // CONFIG_PM_ENABLE
    ctlr->csi_fsm = CSI_FSM_INIT;

#if CONFIG_PM_ENABLE
    if (ctlr->pm_lock) {
        esp_err_t ret = esp_pm_lock_delete(ctlr->pm_lock);
        s_save_first_error(&first_error, ret, "delete pm lock");
        if (ret == ESP_OK) {
            ctlr->pm_lock = NULL;
        }
    }
#endif // CONFIG_PM_ENABLE

    if (ctlr->dma_chan) {
        esp_err_t ret = dw_gdma_del_channel(ctlr->dma_chan);
        s_save_first_error(&first_error, ret, "delete dwgdma channel/interrupt");
        if (ret == ESP_OK) {
            ctlr->dma_chan = NULL;
        }
    }
    if (ctlr->dma_link_list) {
        esp_err_t ret = dw_gdma_del_link_list(ctlr->dma_link_list);
        s_save_first_error(&first_error, ret, "delete dwgdma link list");
        if (ret == ESP_OK) {
            ctlr->dma_link_list = NULL;
        }
    }
#if SOC_ISP_SHARE_CSI_BRG
    if (ctlr->csi_brg_in_use) {
        esp_err_t ret = mipi_csi_brg_declaim(ctlr->csi_brg_id);
        s_save_first_error(&first_error, ret, "declaim CSI bridge");
        if (ret == ESP_OK) {
            ctlr->csi_brg_in_use = false;
        }
    }
#endif
    if (ctlr->csi_id >= 0 && ctlr->csi_id < MIPI_CSI_LL_HOST_CTLR_NUMS) {
        PERIPH_RCC_ATOMIC() {
            mipi_csi_ll_enable_phy_config_clock(ctlr->csi_id, 0);
        }
        s_save_first_error(&first_error, s_csi_disable_phy_clock_source(ctlr),
                           "disable CSI PHY clock source");
        s_save_first_error(&first_error, s_csi_declaim_controller(ctlr),
                           "declaim CSI controller");
    }

    if (ctlr->backup_buffer) {
        free(ctlr->backup_buffer);
        ctlr->backup_buffer = NULL;
    }
    if (ctlr->trans_que) {
        vQueueDeleteWithCaps(ctlr->trans_que);
        ctlr->trans_que = NULL;
    }
    free(ctlr);

    return first_error;
}

static esp_err_t s_ctlr_del(esp_cam_ctlr_t *cam_ctlr)
{
    csi_controller_t *csi_ctlr = __containerof(cam_ctlr, csi_controller_t, base);
    ESP_RETURN_ON_ERROR(s_del_csi_ctlr(csi_ctlr), TAG, "failed to del csi_ctlr");
    return ESP_OK;
}

static esp_err_t s_csi_ctlr_get_internal_buffer(esp_cam_ctlr_handle_t handle, uint32_t fb_num, const void **fb0, ...)
{
    csi_controller_t *csi_ctlr = __containerof(handle, csi_controller_t, base);
    ESP_RETURN_ON_FALSE((csi_ctlr->csi_fsm >= CSI_FSM_INIT) && (csi_ctlr->backup_buffer), ESP_ERR_INVALID_STATE, TAG, "driver don't initialized or back_buffer not available");
    ESP_RETURN_ON_FALSE(fb_num && fb_num <= 1, ESP_ERR_INVALID_ARG, TAG, "invalid frame buffer number");
#if CONFIG_CAM_CTLR_MIPI_CSI_ISR_CACHE_SAFE
    /* An exposed backup buffer is read by the CPU, so the DMA interrupt would
     * have to invalidate it for every frame, and the interrupt may run while
     * the flash driver has the cache disabled. */
    ESP_RETURN_ON_FALSE(false, ESP_ERR_NOT_SUPPORTED, TAG,
                        "backup buffer cannot be exposed with a cache-safe CSI interrupt");
#endif

    csi_ctlr->bk_buffer_exposed = true;
    const void **fb_itor = fb0;
    va_list args;
    va_start(args, fb0);
    for (uint32_t i = 0; i < fb_num; i++) {
        if (fb_itor) {
            *fb_itor = csi_ctlr->backup_buffer;
            fb_itor = va_arg(args, const void **);
        }
    }
    va_end(args);

    return ESP_OK;
}

static esp_err_t s_csi_ctlr_get_buffer_length(esp_cam_ctlr_handle_t handle, size_t *ret_fb_len)
{
    csi_controller_t *csi_ctlr = __containerof(handle, csi_controller_t, base);
    ESP_RETURN_ON_FALSE((csi_ctlr->csi_fsm >= CSI_FSM_INIT) && (csi_ctlr->backup_buffer), ESP_ERR_INVALID_STATE, TAG, "driver don't initialized or back_buffer not available");

    *ret_fb_len = csi_ctlr->fb_size_in_bytes;
    return ESP_OK;
}

/* Prepare every marker while INVALID, then publish VALID after all
 * descriptor stores. The second call only changes VALID's value, regardless
 * of the order in which IDF writes its individual marker fields. Descriptors
 * use the IDF's internal-SRAM non-cacheable alias, so no cache flush is needed. */
static esp_err_t IRAM_ATTR csi_dma_publish_lli(dw_gdma_lli_handle_t lli)
{
    dw_gdma_block_markers_t markers = {
        .is_valid = false,
        .is_last = false,
        .en_trans_done_intr = true,
    };
    esp_err_t ret = dw_gdma_lli_set_block_markers(lli, markers);
    if (ret != ESP_OK) {
        return ret;
    }
    __asm__ volatile("fence iorw, iorw" ::: "memory");
    markers.is_valid = true;
    ret = dw_gdma_lli_set_block_markers(lli, markers);
    __asm__ volatile("fence iorw, iorw" ::: "memory");
    return ret;
}

IRAM_ATTR static bool csi_dma_trans_done_callback(dw_gdma_channel_handle_t chan, const dw_gdma_trans_done_event_data_t *event_data, void *user_data)
{
    (void)chan;
    (void)event_data;
    bool need_yield = false;
    BaseType_t high_task_woken = pdFALSE;
    csi_controller_t *ctlr = (csi_controller_t *)user_data;
    if (!csi_dma_callback_enter(ctlr)) {
        return false;
    }
    bool has_new_trans = false;

    dw_gdma_block_transfer_config_t csi_dma_transfer_config = {};
    csi_dma_transfer_config = (dw_gdma_block_transfer_config_t) {
        .src = {
            .addr = MIPI_CSI_BRG_MEM_BASE,
            .burst_mode = DW_GDMA_BURST_MODE_FIXED,
            .burst_items = DW_GDMA_BURST_ITEMS_64,
            .burst_len = 16,
            .width = DW_GDMA_TRANS_WIDTH_64,
        },
        .dst = {
            .addr = 0,
            .burst_mode = DW_GDMA_BURST_MODE_INCREMENT,
            .burst_items = DW_GDMA_BURST_ITEMS_64,
            .burst_len = 16,
            .width = DW_GDMA_TRANS_WIDTH_64,
        },
        .size = ctlr->csi_transfer_size,
    };
    esp_cam_ctlr_trans_t new_trans = {};
    uint8_t completed_index = ctlr->dma_completed_index;
    esp_cam_ctlr_trans_t completed_trans = ctlr->dma_trans[completed_index];
    bool report_completed_trans = true;

    if (ctlr->cbs.on_get_new_trans) {
        need_yield = ctlr->cbs.on_get_new_trans(&(ctlr->base), &new_trans, ctlr->cbs_user_data);
        if (new_trans.buffer && new_trans.buflen >= ctlr->fb_size_in_bytes) {
            csi_dma_transfer_config.dst.addr = (uint32_t)(new_trans.buffer);
            has_new_trans = true;
        }
    } else if (xQueueReceiveFromISR(ctlr->trans_que, &new_trans, &high_task_woken) == pdTRUE) {
        if (new_trans.buffer && new_trans.buflen >= ctlr->fb_size_in_bytes) {
            csi_dma_transfer_config.dst.addr = (uint32_t)(new_trans.buffer);
            has_new_trans = true;
        }
    }

    if (!has_new_trans) {
        if (!ctlr->bk_buffer_dis) {
            new_trans.buffer = ctlr->backup_buffer;
            new_trans.buflen = ctlr->fb_size_in_bytes;
            ESP_EARLY_LOGD(TAG, "no new buffer or no long enough new buffer, use driver internal buffer");
            csi_dma_transfer_config.dst.addr = (uint32_t)ctlr->backup_buffer;
        } else {
            /* The application has not yet been told that completed_trans is
             * done, so it still owns this buffer as INFLIGHT and cannot touch
             * it. Keep that ownership unchanged, re-arm the just-completed
             * descriptor with the same destination, and deliberately suppress
             * the completion callback. This keeps the CSI ring running without
             * either a private backup allocation or a DMA/app ownership race. */
            assert(completed_trans.buffer &&
                   completed_trans.buflen >= ctlr->fb_size_in_bytes);
            new_trans = completed_trans;
            csi_dma_transfer_config.dst.addr = (uint32_t)completed_trans.buffer;
            report_completed_trans = false;
        }
    }

    /* With CAM_CTLR_MIPI_CSI_ISR_CACHE_SAFE this interrupt also runs while
     * the flash driver has the cache disabled, where esp_cache_msync() must
     * not operate on the cache. Application buffers are registered with
     * dma_cache_fastpath as device-exclusive, so the call returns without a
     * cache operation for them. The private backup buffer is never read by
     * the CPU unless it has been exposed, so it needs no invalidation. */
    if (report_completed_trans &&
        (new_trans.buffer != ctlr->backup_buffer || ctlr->bk_buffer_exposed)) {
        esp_err_t ret = esp_cache_msync((void *)(new_trans.buffer), new_trans.buflen,
                                        ESP_CACHE_MSYNC_FLAG_DIR_M2C);
        assert(ret == ESP_OK);
        (void)ret;
    }

    ESP_EARLY_LOGD(TAG, "new_trans.buffer: %p, new_trans.buflen: %d", new_trans.buffer, new_trans.buflen);
    dw_gdma_lli_handle_t lli = dw_gdma_link_list_get_item(ctlr->dma_link_list, completed_index);
    dw_gdma_lli_config_transfer(lli, &csi_dma_transfer_config);
    esp_err_t publish_ret = csi_dma_publish_lli(lli);
    assert(publish_ret == ESP_OK);
    (void)publish_ret;
    ctlr->dma_trans[completed_index] = new_trans;
    ctlr->dma_completed_index = (completed_index + 1) % ctlr->dma_lli_count;

    /* The circular DMA has advanced to the next descriptor by the time the
     * completed-block interrupt is serviced.  Expose that exact transaction
     * before the completed-frame callback so a consumer can establish a
     * line-safe producer lead without guessing the descriptor rotation. */
    if (ctlr->frame_started_cb) {
        const esp_cam_ctlr_trans_t *started_trans =
            &ctlr->dma_trans[ctlr->dma_completed_index];
        need_yield |= ctlr->frame_started_cb(
            &(ctlr->base), started_trans, ctlr->frame_started_user_data);
    }

    if (report_completed_trans &&
        ((completed_trans.buffer != ctlr->backup_buffer) || ctlr->bk_buffer_exposed)) {
        /* The application uses explicit FREE -> INFLIGHT -> READY -> LOCKED
         * ownership.  A buffer is invalidated immediately before it becomes
         * an INFLIGHT DMA destination above, and no CPU task may touch it
         * until this completion callback changes its ownership to READY.
         * Therefore its cache lines remain invalid for the whole DMA write.
         * Invalidating the entire completed 1.8/4.1 MiB frame again here is
         * redundant and consumes the exact frame-time margin needed for
         * accepted 720p30/1080p30 modes. */
        assert(ctlr->cbs.on_trans_finished);
        if (ctlr->cbs.on_trans_finished) {
            completed_trans.received_size = ctlr->fb_size_in_bytes;
            need_yield |= ctlr->cbs.on_trans_finished(&(ctlr->base), &completed_trans, ctlr->cbs_user_data);
        }
    }

    need_yield |= high_task_woken == pdTRUE;
    csi_dma_callback_exit(ctlr);
    return need_yield;
}

esp_err_t s_register_event_callbacks(esp_cam_ctlr_handle_t handle, const esp_cam_ctlr_evt_cbs_t *cbs, void *user_data)
{
    ESP_RETURN_ON_FALSE(handle && cbs, ESP_ERR_INVALID_ARG, TAG, "invalid argument");
    csi_controller_t *ctlr = __containerof(handle, csi_controller_t, base);
    ESP_RETURN_ON_FALSE(ctlr->csi_fsm == CSI_FSM_INIT, ESP_ERR_INVALID_STATE, TAG, "driver starts already, not allow cbs register");

#if CONFIG_CAM_CTLR_MIPI_CSI_ISR_CACHE_SAFE
    if (cbs->on_get_new_trans) {
        ESP_RETURN_ON_FALSE(esp_ptr_in_iram(cbs->on_get_new_trans), ESP_ERR_INVALID_ARG, TAG, "on_get_new_trans callback not in IRAM");
    }
    if (cbs->on_trans_finished) {
        ESP_RETURN_ON_FALSE(esp_ptr_in_iram(cbs->on_trans_finished), ESP_ERR_INVALID_ARG, TAG, "on_trans_finished callback not in IRAM");
    }
#endif

    ctlr->cbs.on_get_new_trans = cbs->on_get_new_trans;
    ctlr->cbs.on_trans_finished = cbs->on_trans_finished;
    ctlr->cbs_user_data = user_data;

    return ESP_OK;
}

esp_err_t p4_cam_ctlr_csi_register_frame_started_callback(
    esp_cam_ctlr_handle_t handle,
    p4_cam_ctlr_csi_frame_started_cb_t callback,
    void *user_data)
{
    ESP_RETURN_ON_FALSE(handle, ESP_ERR_INVALID_ARG, TAG,
                        "invalid argument: null handle");
    csi_controller_t *ctlr = __containerof(handle, csi_controller_t, base);
    ESP_RETURN_ON_FALSE(ctlr->csi_fsm == CSI_FSM_INIT,
                        ESP_ERR_INVALID_STATE, TAG,
                        "driver starts already, callback registration closed");
#if CONFIG_CAM_CTLR_MIPI_CSI_ISR_CACHE_SAFE
    if (callback) {
        ESP_RETURN_ON_FALSE(esp_ptr_in_iram(callback), ESP_ERR_INVALID_ARG,
                            TAG, "frame-start callback is not in IRAM");
    }
#endif
    ctlr->frame_started_cb = callback;
    ctlr->frame_started_user_data = user_data;
    return ESP_OK;
}

esp_err_t s_csi_ctlr_enable(esp_cam_ctlr_handle_t handle)
{
    ESP_RETURN_ON_FALSE(handle, ESP_ERR_INVALID_ARG, TAG, "invalid argument: null pointer");
    csi_controller_t *ctlr = __containerof(handle, csi_controller_t, base);
    ESP_RETURN_ON_FALSE(ctlr->csi_fsm == CSI_FSM_INIT, ESP_ERR_INVALID_STATE, TAG, "processor isn't in init state");

    portENTER_CRITICAL(&ctlr->spinlock);
    ctlr->csi_fsm = CSI_FSM_ENABLED;
#if CONFIG_PM_ENABLE
    if (ctlr->pm_lock) {
        ESP_RETURN_ON_ERROR(esp_pm_lock_acquire(ctlr->pm_lock), TAG, "acquire pm_lock failed");
    }
#endif // CONFIG_PM_ENABLE
    portEXIT_CRITICAL(&ctlr->spinlock);

    return ESP_OK;
}

esp_err_t s_csi_ctlr_disable(esp_cam_ctlr_handle_t handle)
{
    ESP_RETURN_ON_FALSE(handle, ESP_ERR_INVALID_ARG, TAG, "invalid argument: null pointer");
    csi_controller_t *ctlr = __containerof(handle, csi_controller_t, base);
    ESP_RETURN_ON_FALSE(ctlr->csi_fsm == CSI_FSM_ENABLED, ESP_ERR_INVALID_STATE, TAG, "processor isn't in enable state");

    portENTER_CRITICAL(&ctlr->spinlock);
    ctlr->csi_fsm = CSI_FSM_INIT;
#if CONFIG_PM_ENABLE
    if (ctlr->pm_lock) {
        ESP_RETURN_ON_ERROR(esp_pm_lock_release(ctlr->pm_lock), TAG, "release pm_lock failed");
    }
#endif // CONFIG_PM_ENABLE
    portEXIT_CRITICAL(&ctlr->spinlock);

    return ESP_OK;
}

esp_err_t s_ctlr_csi_start(esp_cam_ctlr_handle_t handle)
{
    ESP_RETURN_ON_FALSE(handle, ESP_ERR_INVALID_ARG, TAG, "invalid argument");
    csi_controller_t *ctlr = __containerof(handle, csi_controller_t, base);
    ESP_RETURN_ON_FALSE(ctlr->csi_fsm == CSI_FSM_ENABLED, ESP_ERR_INVALID_STATE, TAG, "driver starts already, should not start again");
    ESP_RETURN_ON_FALSE(ctlr->cbs.on_trans_finished, ESP_ERR_INVALID_STATE, TAG, "no on_trans_finished callback registered");
    ESP_RETURN_ON_ERROR(csi_dma_quiesce(ctlr), TAG, "CSI DMA not idle before start");

    dw_gdma_block_transfer_config_t csi_dma_transfer_config = {};
    csi_dma_transfer_config = (dw_gdma_block_transfer_config_t) {
        .src = {
            .addr = MIPI_CSI_BRG_MEM_BASE,
            .burst_mode = DW_GDMA_BURST_MODE_FIXED,
            .burst_items = DW_GDMA_BURST_ITEMS_64,
            .burst_len = 16,
            .width = DW_GDMA_TRANS_WIDTH_64,
        },
        .dst = {
            .addr = 0,
            .burst_mode = DW_GDMA_BURST_MODE_INCREMENT,
            .burst_items = DW_GDMA_BURST_ITEMS_64,
            .burst_len = 16,
            .width = DW_GDMA_TRANS_WIDTH_64,
        },
        .size = ctlr->csi_transfer_size,
    };
    memset(ctlr->dma_trans, 0, sizeof(ctlr->dma_trans));
    ctlr->dma_completed_index = 0;
    for (uint8_t index = 0; index < ctlr->dma_lli_count; index++) {
        esp_cam_ctlr_trans_t trans = {};
        if (ctlr->cbs.on_get_new_trans) {
            ctlr->cbs.on_get_new_trans(handle, &trans, ctlr->cbs_user_data);
        }
        if (!trans.buffer || trans.buflen < ctlr->fb_size_in_bytes) {
            ESP_RETURN_ON_FALSE(!ctlr->bk_buffer_dis, ESP_ERR_INVALID_STATE, TAG,
                                "no ready transaction, and no backup buffer");
            trans.buffer = ctlr->backup_buffer;
            trans.buflen = ctlr->fb_size_in_bytes;
        }
        ESP_RETURN_ON_ERROR(esp_cache_msync((void *)(trans.buffer), trans.buflen, ESP_CACHE_MSYNC_FLAG_DIR_M2C),
                            TAG, "failed to sync(M2C) trans buffer");
        csi_dma_transfer_config.dst.addr = (uint32_t)trans.buffer;
        dw_gdma_lli_handle_t lli = dw_gdma_link_list_get_item(ctlr->dma_link_list, index);
        ESP_RETURN_ON_ERROR(dw_gdma_lli_config_transfer(lli, &csi_dma_transfer_config), TAG,
                            "failed to configure dwgdma transfer");
        ESP_RETURN_ON_ERROR(csi_dma_publish_lli(lli), TAG,
                            "failed to validate dwgdma transfer");
        ctlr->dma_trans[index] = trans;
    }

    portENTER_CRITICAL(&ctlr->spinlock);
    ctlr->csi_fsm = CSI_FSM_STARTED;
    portEXIT_CRITICAL(&ctlr->spinlock);

    ESP_RETURN_ON_ERROR(dw_gdma_channel_use_link_list(ctlr->dma_chan, ctlr->dma_link_list), TAG, "failed to apply dwgdma link list");
    ESP_RETURN_ON_ERROR(csi_dma_resume_callbacks(ctlr), TAG, "failed to restore CSI DMA callbacks");
    ESP_RETURN_ON_ERROR(dw_gdma_channel_enable_ctrl(ctlr->dma_chan, true), TAG, "failed to enable dwgdma");

    //enable CSI bridge
    mipi_csi_brg_ll_enable(ctlr->hal.bridge_dev, true);

    return ESP_OK;
}

esp_err_t s_ctlr_csi_stop(esp_cam_ctlr_handle_t handle)
{
    ESP_RETURN_ON_FALSE(handle, ESP_ERR_INVALID_ARG, TAG, "invalid argument");
    csi_controller_t *ctlr = __containerof(handle, csi_controller_t, base);
    ESP_RETURN_ON_FALSE(ctlr->csi_fsm == CSI_FSM_STARTED, ESP_ERR_INVALID_STATE, TAG, "driver isn't started");

    ESP_RETURN_ON_ERROR(csi_dma_quiesce(ctlr), TAG, "failed to stop CSI DMA");

    portENTER_CRITICAL(&ctlr->spinlock);
    ctlr->csi_fsm = CSI_FSM_ENABLED;
    portEXIT_CRITICAL(&ctlr->spinlock);

    return ESP_OK;
}

esp_err_t s_ctlr_csi_receive(esp_cam_ctlr_handle_t handle, esp_cam_ctlr_trans_t *trans, uint32_t timeout_ms)
{
    esp_err_t ret = ESP_OK;
    ESP_RETURN_ON_FALSE(handle && trans, ESP_ERR_INVALID_ARG, TAG, "invalid argument: null pointer");
    csi_controller_t *ctlr = __containerof(handle, csi_controller_t, base);

    ESP_RETURN_ON_FALSE(trans->buffer, ESP_ERR_INVALID_ARG, TAG, "invalid argument: no trans buffer");
    ESP_RETURN_ON_FALSE((trans->buflen >= ctlr->fb_size_in_bytes), ESP_ERR_INVALID_ARG, TAG, "invalid argument: trans buffer smaller than framebuffer size");

    TickType_t ticks_to_wait = timeout_ms / portTICK_PERIOD_MS;
    if (timeout_ms == ESP_CAM_CTLR_MAX_DELAY) {
        ticks_to_wait = portMAX_DELAY;
    }

    BaseType_t r = xQueueSend(ctlr->trans_que, trans, ticks_to_wait);
    if (r != pdTRUE) {
        ret = ESP_ERR_TIMEOUT;
        ESP_LOGW(TAG, "csi recv API, transaction queue is full, failed to send transaction to the queue");
        return ret;
    }

    return ESP_OK;
}

static void *s_csi_ctlr_alloc_buffer(esp_cam_ctlr_t *handle, size_t size, uint32_t buf_caps)
{
    csi_controller_t *ctlr = __containerof(handle, csi_controller_t, base);

    if (!ctlr) {
        ESP_LOGE(TAG, "invalid argument: handle is null");
        return NULL;
    }

    void *buffer = heap_caps_calloc(1, size, buf_caps);
    if (!buffer) {
        ESP_LOGE(TAG, "failed to allocate buffer");
        return NULL;
    }

    ESP_LOGD(TAG, "Allocated camera buffer: %p, size: %zu", buffer, size);

    return buffer;
}

static bool s_is_color_format_conversion_supported(cam_ctlr_color_t color_format)
{
    return (color_format == CAM_CTLR_COLOR_RGB888 ||
            color_format == CAM_CTLR_COLOR_RGB565 ||
            color_format == CAM_CTLR_COLOR_YUV420 ||
            color_format == CAM_CTLR_COLOR_YUV422_YVYU ||
            color_format == CAM_CTLR_COLOR_YUV422_YUYV ||
            color_format == CAM_CTLR_COLOR_YUV422_UYVY ||
            color_format == CAM_CTLR_COLOR_YUV422_VYUY);
}

static esp_err_t s_csi_ctlr_format_conversion(esp_cam_ctlr_t *handle, const cam_ctlr_format_conv_config_t *config)
{
    ESP_RETURN_ON_FALSE(handle && config, ESP_ERR_INVALID_ARG, TAG, "invalid argument: null pointer");
    csi_controller_t *ctlr = __containerof(handle, csi_controller_t, base);

    if (ctlr->custom_data_depth || config->src_format == config->dst_format) {
        mipi_csi_brg_ll_set_color_mode_bypass(ctlr->hal.bridge_dev, true);
        return ESP_OK;
    } else {
#if CONFIG_IDF_TARGET_ESP32P4
        //If ESP32P4 chip version is less than v3.0, not support color format conversion
        unsigned chip_version = efuse_hal_chip_revision();
        if (!ESP_CHIP_REV_ABOVE(chip_version, 300)) {
            return ESP_ERR_NOT_SUPPORTED;
        }
#endif

        if (!s_is_color_format_conversion_supported(config->src_format) || !s_is_color_format_conversion_supported(config->dst_format)) {
            return ESP_ERR_NOT_SUPPORTED;
        } else {
            mipi_csi_brg_ll_set_input_color_format(ctlr->hal.bridge_dev, config->src_format);
            mipi_csi_brg_ll_set_output_color_format(ctlr->hal.bridge_dev, config->dst_format);
            mipi_csi_brg_ll_set_color_mode_bypass(ctlr->hal.bridge_dev, false);
        }
        ctlr->in_color_format = config->src_format;
        ctlr->out_color_format = config->dst_format;
        return ESP_OK;
    }
}
