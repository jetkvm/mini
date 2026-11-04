#!/usr/bin/env python3
"""Fail closed if LVGL escapes its fixed PSRAM sub-heap."""

from pathlib import Path
import re


COMPONENT = Path(__file__).resolve().parents[1]
REPO = COMPONENT.parents[1]
POLICY_HEADER = (
    REPO / "components/jetkvm_memory_policy/include/jetkvm_memory_policy.h"
).read_text()
POLICY_SOURCE = (
    REPO / "components/jetkvm_memory_policy/jetkvm_memory_policy.c"
).read_text()
LVGL_CORE = (COMPONENT / "jetkvm_lvgl_memory.c").read_text()
SCREEN_CMAKE = (COMPONENT / "CMakeLists.txt").read_text()
SDK_DEFAULTS = (REPO / "sdkconfig.defaults").read_text()
HTTP_STATS = (
    REPO / "components/webrtc_transport/webrtc_stats_json.c"
).read_text()
SCREENS = (COMPONENT / "ui/screens.c").read_text()


assert "CONFIG_LV_USE_CUSTOM_MALLOC=y" in SDK_DEFAULTS
assert "# CONFIG_LV_USE_CLIB_MALLOC is not set" in SDK_DEFAULTS
assert '"jetkvm_lvgl_memory.c"' in SCREEN_CMAKE
assert "jetkvm_memory_policy" in SCREEN_CMAKE
assert '"-Wl,--undefined=lv_mem_init"' in SCREEN_CMAKE

assert "JETKVM_MEMORY_CONTROL_ARENA_BYTES (1280U * 1024U)" in POLICY_HEADER
assert "JETKVM_MEMORY_LVGL_ARENA_BYTES (256U * 1024U)" in POLICY_HEADER
assert "JETKVM_MEMORY_POLICY_ARENAS_TOTAL_BYTES" in POLICY_HEADER
assert (
    "JETKVM_MEMORY_CONTROL_CLOUD_QUEUE_MAX_BYTES (512U * 1024U)"
    in POLICY_HEADER
)
assert "control-plane quotas must exactly partition" in POLICY_HEADER

assert POLICY_SOURCE.count("heap_caps_aligned_alloc(") == 1
assert (
    "64U, JETKVM_MEMORY_POLICY_ARENAS_TOTAL_BYTES" in POLICY_SOURCE
)
assert POLICY_SOURCE.count("multi_heap_register(") == 2
assert (
    "s_policy_arenas + JETKVM_MEMORY_CONTROL_ARENA_BYTES"
    in POLICY_SOURCE
)
assert "multi_heap_set_lock(s_control_heap" in POLICY_SOURCE
assert "multi_heap_set_lock(s_lvgl_heap" in POLICY_SOURCE

for symbol in (
    "lv_mem_init",
    "lv_mem_deinit",
    "lv_mem_add_pool",
    "lv_mem_remove_pool",
    "lv_malloc_core",
    "lv_realloc_core",
    "lv_free_core",
    "lv_mem_monitor_core",
    "lv_mem_test_core",
):
    assert re.search(rf"\b{symbol}\s*\(", LVGL_CORE), symbol

assert "jetkvm_memory_policy_lvgl_alloc(size)" in LVGL_CORE
assert "jetkvm_memory_policy_lvgl_realloc(memory, new_size)" in LVGL_CORE
assert "jetkvm_memory_policy_lvgl_free(memory)" in LVGL_CORE
assert not re.search(
    r"\b(?:malloc|calloc|realloc|free|heap_caps_malloc)\s*\(", LVGL_CORE
)

for field in (
    "lvglArenaCapacity",
    "lvglArenaUsed",
    "lvglArenaPeakUsed",
    "lvglArenaFree",
    "lvglArenaMinimum",
    "lvglArenaLargest",
    "lvglAllocationFailures",
):
    assert f'"{field}"' in HTTP_STATS, field

object_count = len(
    re.findall(
        r"\blv_(?:obj|label|image|button|spinner)_create\s*\(", SCREENS
    )
)
assert object_count == 183, object_count

print("LVGL private memory-domain static contract passed")
