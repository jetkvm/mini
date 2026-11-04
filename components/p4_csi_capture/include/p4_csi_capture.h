/*
 * SPDX-FileCopyrightText: 2026
 *
 * SPDX-License-Identifier: MIT
 */

#pragma once

#include <stddef.h>

#include "esp_cam_ctlr_csi.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Callback issued from the CSI DMA ISR when the next frame transaction
 *        becomes the active destination.
 *
 * The callback is observational: it must not replace or release @p trans.
 * Its implementation must be in IRAM when the CSI ISR cache-safe option is
 * enabled.
 */
typedef bool (*p4_cam_ctlr_csi_frame_started_cb_t)(
    esp_cam_ctlr_handle_t handle, const esp_cam_ctlr_trans_t *trans,
    void *user_data);

/**
 * The driver's backup buffer is allocated in multiples of this many bytes, so
 * a DMA block bounded to a frame rounded up to whole bridge bursts of at most
 * this size stays inside it.
 */
#define P4_CSI_BACKUP_BUFFER_ALIGN_BYTES 512U

/** Supported circular DMA descriptor counts. */
enum {
    P4_CSI_DMA_RING_COMPACT = 2,
    P4_CSI_DMA_RING_DEFAULT = 3,
};

/**
 * @brief Create a CSI controller with an explicit circular DMA ring size.
 *
 * The caller chooses the ring size using its known frame rate and measured
 * interrupt-service margin. Compact mode must rearm a completed descriptor
 * before the following frame ends. The ring is allocated with the requested
 * size during construction and cannot be resized while enabled or running.
 * Construction and start/stop/delete must run on tasks pinned to the same
 * CPU core. This is the core on which the DMA interrupt is registered.
 * Stop/delete returning ESP_ERR_TIMEOUT or ESP_ERR_NOT_ALLOWED retain the
 * handle and all DMA-owned buffers; retry on the owner core or reset before
 * reusing or freeing those buffers. Other delete errors consume the handle.
 *
 * @param descriptor_count P4_CSI_DMA_RING_COMPACT or P4_CSI_DMA_RING_DEFAULT.
 */
esp_err_t p4_cam_new_csi_ctlr_with_dma_ring(
    const esp_cam_ctlr_csi_config_t *config, size_t descriptor_count,
    esp_cam_ctlr_handle_t *ret_handle);

/**
 * @brief Create a CSI controller using the default three-descriptor ring.
 *
 * The owner-core and retained-resource rules above also apply here.
 */
esp_err_t p4_cam_new_csi_ctlr(const esp_cam_ctlr_csi_config_t *config,
                              esp_cam_ctlr_handle_t *ret_handle);

/**
 * @brief Register an optional active-frame notification before starting CSI.
 */
esp_err_t p4_cam_ctlr_csi_register_frame_started_callback(
    esp_cam_ctlr_handle_t handle,
    p4_cam_ctlr_csi_frame_started_cb_t callback,
    void *user_data);

#ifdef __cplusplus
}
#endif
