#!/usr/bin/env python3
"""Execute real ring construction checks, startup, EOF rotation and publication.

Hardware calls are mocked. In particular, the marker mock deliberately writes
VALID first, so the publisher cannot depend on the current IDF store order.
This does not model DMA prefetch timing or replace the camera hardware test.
"""

from pathlib import Path
import re
import subprocess
import tempfile


COMPONENT = Path(__file__).resolve().parents[1]
source = (COMPONENT / "p4_cam_ctlr_csi.c").read_text()
public = (COMPONENT / "include/p4_csi_capture.h").read_text()


def extract_function(signature):
    start = source.index(signature)
    opening = source.index("{", start)
    depth = 1
    end = opening + 1
    while depth:
        depth += (source[end] == "{") - (source[end] == "}")
        end += 1
    return source[start:end] + "\n"


ring_enum = re.search(
    r"enum\s*\{[^}]*P4_CSI_DMA_RING_COMPACT[^}]*\};", public
).group()
publisher = extract_function("static esp_err_t IRAM_ATTR csi_dma_publish_lli(")
publisher = publisher.replace(
    '__asm__ volatile("fence iorw, iorw" ::: "memory");', "publish_barrier();"
)
callback = extract_function("IRAM_ATTR static bool csi_dma_trans_done_callback(")
start_capture = extract_function(
    "esp_err_t s_ctlr_csi_start(esp_cam_ctlr_handle_t handle)\n{"
)
default_constructor = extract_function("esp_err_t p4_cam_new_csi_ctlr(")

# Execute the actual constructor through count validation/allocation/assignment.
# Its remaining peripheral initialization needs hardware and is tested on board.
begin = source.index("esp_err_t p4_cam_new_csi_ctlr_with_dma_ring(")
end = source.index("    ctlr->spinlock =", begin)
constructor = source[begin:end] + """
    (void)ret;
    *ret_handle = &ctlr->base;
    return ESP_OK;
}
"""

HARNESS = r"""
#include <assert.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define IRAM_ATTR
#define CONFIG_IDF_TARGET_ESP32P4 0
#define ESP_OK 0
#define ESP_FAIL (-1)
#define ESP_ERR_INVALID_ARG 1
#define ESP_ERR_INVALID_STATE 2
#define ESP_ERR_NO_MEM 3
#define ESP_ERR_NOT_SUPPORTED 4
#define ESP_ERR_NOT_ALLOWED 5
#define portNUM_PROCESSORS 2
#define CSI_MEM_ALLOC_CAPS 1
#define MIPI_CSI_HOST_LL_LANE_NUM_MAX 2
#define CSI_FSM_ENABLED 2
#define CSI_FSM_STARTED 3
#define MIPI_CSI_BRG_MEM_BASE 0x2000
#define ESP_CACHE_MSYNC_FLAG_DIR_M2C 1
#define DW_GDMA_BURST_MODE_FIXED 0
#define DW_GDMA_BURST_MODE_INCREMENT 1
#define DW_GDMA_BURST_ITEMS_64 64
#define DW_GDMA_TRANS_WIDTH_64 64
#define pdFALSE 0
#define pdTRUE 1
#define ESP_RETURN_ON_FALSE(condition, error, ...) do { \
    if (!(condition)) return (error); \
} while (0)
#define ESP_RETURN_ON_ERROR(call, ...) do { \
    esp_err_t result_ = (call); if (result_ != ESP_OK) return result_; \
} while (0)
#define ESP_EARLY_LOGD(...) ((void)0)
#define portENTER_CRITICAL(mux) ((void)(mux))
#define portEXIT_CRITICAL(mux) ((void)(mux))
#define __containerof(ptr, type, field) \
    ((type *)((char *)(ptr) - offsetof(type, field)))

typedef int esp_err_t;
typedef int BaseType_t;
typedef struct { int unused; } esp_cam_ctlr_t;
typedef esp_cam_ctlr_t *esp_cam_ctlr_handle_t;
typedef struct {
    void *buffer;
    size_t buflen, received_size;
} esp_cam_ctlr_trans_t;
typedef struct {
    unsigned data_lane_num;
    struct { unsigned bits_per_pixel; } data_type;
    int input_data_color_type, output_data_color_type;
    bool input_8bit_swap_en, input_16bit_swap_en;
} esp_cam_ctlr_csi_config_t;
typedef struct {
    bool is_valid, is_last, en_trans_done_intr;
} dw_gdma_block_markers_t;
typedef struct {
    uint32_t addr;
    int burst_mode, burst_items, burst_len, width;
} endpoint_t;
typedef struct {
    endpoint_t src, dst;
    size_t size;
} dw_gdma_block_transfer_config_t;
typedef struct {
    bool valid, last, irq, fields_ready;
    uint32_t dst;
} mock_lli_t;
typedef mock_lli_t *dw_gdma_lli_handle_t;
typedef struct { unsigned count; mock_lli_t items[3]; } mock_list_t;
typedef void *dw_gdma_channel_handle_t;
typedef struct { int unused; } dw_gdma_trans_done_event_data_t;

/* RING_ENUM */

typedef struct csi_controller_t {
    int csi_id, csi_fsm, spinlock;
    esp_cam_ctlr_t base;
    struct { void *bridge_dev; } hal;
    dw_gdma_channel_handle_t dma_chan;
    mock_list_t *dma_link_list;
    esp_cam_ctlr_trans_t dma_trans[P4_CSI_DMA_RING_DEFAULT];
    uint8_t dma_lli_count, dma_completed_index;
    int8_t dma_owner_core;
    bool dma_stopping;
    size_t csi_transfer_size, fb_size_in_bytes;
    struct {
        bool (*on_get_new_trans)(esp_cam_ctlr_handle_t,
                                  esp_cam_ctlr_trans_t *, void *);
        bool (*on_trans_finished)(esp_cam_ctlr_handle_t,
                                   esp_cam_ctlr_trans_t *, void *);
    } cbs;
    void *cbs_user_data, *frame_started_user_data;
    bool (*frame_started_cb)(esp_cam_ctlr_handle_t,
                              const esp_cam_ctlr_trans_t *, void *);
    bool bk_buffer_dis, bk_buffer_exposed;
    void *backup_buffer, *trans_que;
} csi_controller_t;

static unsigned allocations, barriers, marker_calls, publications;
static unsigned fail_marker_call, cache_calls, last_item_index;
static unsigned next_buffer, finished_calls, started_calls;
static unsigned callbacks_active, callback_enters, callback_exits;
static unsigned quiesce_calls, resume_calls;
static int control_core;
static bool provide_buffer, dma_enabled, bridge_enabled;
static void *last_finished, *last_started;

static int csi_dma_control_core(void) { return control_core; }

/* Stop synchronization itself has a separate production-helper test. These
 * mocks verify that startup/EOF call that interface and balance admission. */
static bool csi_dma_callback_enter(csi_controller_t *ctlr)
{
    if (ctlr->dma_stopping) return false;
    assert(callbacks_active == 0);
    ++callbacks_active;
    ++callback_enters;
    return true;
}

static void csi_dma_callback_exit(csi_controller_t *ctlr)
{
    (void)ctlr;
    assert(callbacks_active == 1);
    --callbacks_active;
    ++callback_exits;
}

static esp_err_t csi_dma_quiesce(csi_controller_t *ctlr)
{
    assert(callbacks_active == 0);
    ctlr->dma_stopping = true;
    ++quiesce_calls;
    return ESP_OK;
}

static esp_err_t csi_dma_resume_callbacks(csi_controller_t *ctlr)
{
    assert(callbacks_active == 0 && ctlr->dma_stopping);
    ctlr->dma_stopping = false;
    ++resume_calls;
    return ESP_OK;
}

static void *heap_caps_calloc(size_t count, size_t bytes, int caps)
{
    assert(caps == CSI_MEM_ALLOC_CAPS);
    ++allocations;
    return calloc(count, bytes);
}

static void publish_barrier(void) { ++barriers; }

static void device_observe(const mock_lli_t *item)
{
    if (item->valid) {
        assert(item->fields_ready && item->irq && !item->last);
        assert(barriers >= 1);
    }
}

static esp_err_t dw_gdma_lli_set_block_markers(dw_gdma_lli_handle_t item,
                                              dw_gdma_block_markers_t markers)
{
    if (++marker_calls == fail_marker_call) return ESP_FAIL;
    /* Adversarial implementation: publish VALID before the other markers. */
    item->valid = markers.is_valid;
    if (item->valid) ++publications;
    device_observe(item);
    item->last = markers.is_last;
    device_observe(item);
    item->irq = markers.en_trans_done_intr;
    device_observe(item);
    return ESP_OK;
}

static esp_err_t dw_gdma_lli_config_transfer(
    dw_gdma_lli_handle_t item, const dw_gdma_block_transfer_config_t *config)
{
    assert(!item->valid);
    assert(config->size > 0 && config->dst.addr != 0);
    item->dst = config->dst.addr;
    item->fields_ready = true;
    return ESP_OK;
}

static dw_gdma_lli_handle_t dw_gdma_link_list_get_item(mock_list_t *list,
                                                      unsigned index)
{
    assert(index < list->count);
    last_item_index = index;
    return &list->items[index];
}

static esp_err_t dw_gdma_channel_use_link_list(dw_gdma_channel_handle_t chan,
                                               mock_list_t *list)
{
    (void)chan;
    for (unsigned i = 0; i < list->count; ++i) device_observe(&list->items[i]);
    return ESP_OK;
}

static esp_err_t dw_gdma_channel_enable_ctrl(dw_gdma_channel_handle_t chan,
                                             bool enabled)
{
    (void)chan;
    dma_enabled = enabled;
    return ESP_OK;
}

static void mipi_csi_brg_ll_enable(void *dev, bool enabled)
{
    (void)dev;
    assert(!enabled || dma_enabled);
    bridge_enabled = enabled;
}

static esp_err_t esp_cache_msync(void *buffer, size_t bytes, int flags)
{
    assert(buffer && bytes >= 64 && flags == ESP_CACHE_MSYNC_FLAG_DIR_M2C);
    ++cache_calls;
    return ESP_OK;
}

static BaseType_t xQueueReceiveFromISR(void *queue, esp_cam_ctlr_trans_t *trans,
                                       BaseType_t *task_woken)
{
    (void)queue; (void)trans; (void)task_woken;
    return pdFALSE;
}

static bool get_trans(esp_cam_ctlr_handle_t handle,
                       esp_cam_ctlr_trans_t *trans, void *user)
{
    (void)handle; (void)user;
    if (provide_buffer) {
        trans->buffer = (void *)(uintptr_t)++next_buffer;
        trans->buflen = 128;
    }
    return false;
}

static bool finished(esp_cam_ctlr_handle_t handle,
                      esp_cam_ctlr_trans_t *trans, void *user)
{
    (void)handle; (void)user;
    assert(trans->received_size == 64);
    last_finished = trans->buffer;
    ++finished_calls;
    return false;
}

static bool started(esp_cam_ctlr_handle_t handle,
                     const esp_cam_ctlr_trans_t *trans, void *user)
{
    (void)handle; (void)user;
    last_started = trans->buffer;
    ++started_calls;
    return false;
}

/* PRODUCTION_FUNCTIONS */

static void reset_observers(void)
{
    barriers = marker_calls = publications = fail_marker_call = 0;
    cache_calls = finished_calls = started_calls = 0;
    callbacks_active = callback_enters = callback_exits = 0;
    quiesce_calls = resume_calls = 0;
    next_buffer = 100;
    provide_buffer = true;
    dma_enabled = bridge_enabled = false;
    last_finished = last_started = NULL;
}

static void test_constructor_bounds(void)
{
    const esp_cam_ctlr_csi_config_t config = {
        .data_lane_num = 2, .input_data_color_type = 1,
        .output_data_color_type = 1,
    };
    const size_t invalid[] = {0, 1, 4, 258, SIZE_MAX};
    for (size_t i = 0; i < sizeof(invalid) / sizeof(invalid[0]); ++i) {
        unsigned before = allocations;
        esp_cam_ctlr_handle_t handle = (void *)(uintptr_t)1;
        assert(p4_cam_new_csi_ctlr_with_dma_ring(&config, invalid[i], &handle)
               == ESP_ERR_INVALID_ARG);
        assert(handle == NULL && allocations == before);
    }
    for (size_t count = 2; count <= 3; ++count) {
        esp_cam_ctlr_handle_t handle = NULL;
        assert(p4_cam_new_csi_ctlr_with_dma_ring(&config, count, &handle) == ESP_OK);
        csi_controller_t *ctlr = __containerof(handle, csi_controller_t, base);
        assert(ctlr->dma_lli_count == count);
        assert(ctlr->dma_owner_core == control_core && ctlr->dma_stopping);
        free(ctlr);
    }
    esp_cam_ctlr_handle_t handle = NULL;
    assert(p4_cam_new_csi_ctlr(&config, &handle) == ESP_OK);
    csi_controller_t *ctlr = __containerof(handle, csi_controller_t, base);
    assert(ctlr->dma_lli_count == P4_CSI_DMA_RING_DEFAULT);
    free(ctlr);
    unsigned before = allocations;
    assert(p4_cam_new_csi_ctlr_with_dma_ring(NULL, 2, &handle) == ESP_ERR_INVALID_ARG);
    assert(p4_cam_new_csi_ctlr_with_dma_ring(&config, 2, NULL) == ESP_ERR_INVALID_ARG);
    assert(allocations == before);
    const int invalid_cores[] = {-1, portNUM_PROCESSORS};
    for (size_t i = 0; i < sizeof(invalid_cores) / sizeof(invalid_cores[0]); ++i) {
        control_core = invalid_cores[i];
        handle = (void *)(uintptr_t)1;
        assert(p4_cam_new_csi_ctlr_with_dma_ring(&config, 2, &handle)
               == ESP_ERR_NOT_ALLOWED);
        assert(handle == NULL && allocations == before);
    }
    control_core = 0;
}

static void test_publication(void)
{
    reset_observers();
    mock_lli_t item = {.last = true, .fields_ready = true};
    assert(csi_dma_publish_lli(&item) == ESP_OK);
    assert(item.valid && !item.last && item.irq);
    assert(marker_calls == 2 && publications == 1 && barriers == 2);
    reset_observers();
    item = (mock_lli_t){.last = true, .fields_ready = true};
    fail_marker_call = 1;
    assert(csi_dma_publish_lli(&item) == ESP_FAIL);
    assert(!item.valid && publications == 0 && barriers == 0);
}

static void test_start_and_rotation(unsigned count)
{
    reset_observers();
    mock_list_t list = {.count = count};
    csi_controller_t ctlr = {
        .csi_fsm = CSI_FSM_ENABLED,
        .dma_lli_count = count, .dma_link_list = &list,
        .fb_size_in_bytes = 64, .csi_transfer_size = 8,
        .bk_buffer_dis = true,
        .cbs = {.on_get_new_trans = get_trans, .on_trans_finished = finished},
        .frame_started_cb = started,
    };
    assert(s_ctlr_csi_start(&ctlr.base) == ESP_OK);
    assert(dma_enabled && bridge_enabled && ctlr.csi_fsm == CSI_FSM_STARTED);
    assert(cache_calls == count && publications == count);
    assert(quiesce_calls == 1 && resume_calls == 1 && !ctlr.dma_stopping);
    assert(ctlr.dma_completed_index == 0);
    if (count == 2) assert(ctlr.dma_trans[2].buffer == NULL);
    for (unsigned frame = 0; frame < 12; ++frame) {
        unsigned completed_index = frame % count;
        unsigned following_index = (completed_index + 1) % count;
        void *old_buffer = ctlr.dma_trans[completed_index].buffer;
        void *next_active = ctlr.dma_trans[following_index].buffer;
        list.items[completed_index].valid = false; /* Simulated HW writeback. */
        assert(!csi_dma_trans_done_callback(NULL, NULL, &ctlr));
        assert(last_item_index == completed_index);
        assert(ctlr.dma_completed_index == following_index);
        assert(last_finished == old_buffer && last_started == next_active);
        assert(ctlr.dma_trans[completed_index].buffer != old_buffer);
        assert(list.items[completed_index].dst ==
               (uint32_t)(uintptr_t)ctlr.dma_trans[completed_index].buffer);
        assert(finished_calls == frame + 1 && started_calls == frame + 1);
        assert(callbacks_active == 0 && callback_enters == callback_exits);
        if (count == 2) assert(ctlr.dma_trans[2].buffer == NULL);
    }

    /* Without a replacement, the driver must rearm the same buffer and
     * suppress EOF ownership transfer while continuing descriptor rotation. */
    provide_buffer = false;
    unsigned index = ctlr.dma_completed_index;
    void *retained = ctlr.dma_trans[index].buffer;
    unsigned before_cache = cache_calls, before_finished = finished_calls;
    list.items[index].valid = false;
    assert(!csi_dma_trans_done_callback(NULL, NULL, &ctlr));
    assert(ctlr.dma_trans[index].buffer == retained);
    assert(finished_calls == before_finished && cache_calls == before_cache);
    assert(ctlr.dma_completed_index == (index + 1) % count);
    assert(callbacks_active == 0 && callback_enters == callback_exits);

    /* A late IRQ must not touch descriptors or deliver EOF after admission
     * closes. The real stop helper's races are covered by its own test. */
    assert(csi_dma_quiesce(&ctlr) == ESP_OK);
    unsigned stopped_index = ctlr.dma_completed_index;
    unsigned stopped_enters = callback_enters;
    assert(!csi_dma_trans_done_callback(NULL, NULL, &ctlr));
    assert(ctlr.dma_completed_index == stopped_index);
    assert(finished_calls == before_finished && cache_calls == before_cache);
    assert(callback_enters == stopped_enters && callbacks_active == 0);
}

int main(void)
{
    test_constructor_bounds();
    test_publication();
    test_start_and_rotation(2);
    test_start_and_rotation(3);
    puts("CSI DMA ring validation, startup, rotation and VALID publication: PASS");
}
"""

with tempfile.TemporaryDirectory(prefix="csi-dma-ring-") as directory:
    path = Path(directory)
    (path / "test.c").write_text(
        HARNESS.replace("/* RING_ENUM */", ring_enum).replace(
            "/* PRODUCTION_FUNCTIONS */",
            constructor + default_constructor + publisher + callback + start_capture,
        )
    )
    subprocess.run(
        ["cc", "-std=c11", "-Wall", "-Wextra", "-Werror", "-O2",
         "-Wno-pointer-to-int-cast", str(path / "test.c"), "-o", str(path / "test")],
        check=True,
    )
    subprocess.run([str(path / "test")], check=True)
