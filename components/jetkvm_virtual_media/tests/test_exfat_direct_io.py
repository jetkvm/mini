#!/usr/bin/env python3
"""Static and sparse-file boundary contract for the exFAT media backend."""

from pathlib import Path
import os
import tempfile


COMPONENT = Path(__file__).resolve().parents[1]
REPO = COMPONENT.parents[1]
SOURCE = (COMPONENT / "jetkvm_virtual_media.c").read_text(encoding="utf-8")
HEADER = (COMPONENT / "include/jetkvm_virtual_media.h").read_text(encoding="utf-8")
DEFAULTS = (REPO / "sdkconfig.defaults").read_text(encoding="utf-8")
IDF_PATCH = (REPO / "patches/esp-idf-6.0.2-fatfs-exfat-config.patch").read_text(
    encoding="utf-8"
)
ROOT_CMAKE = (REPO / "CMakeLists.txt").read_text(encoding="utf-8")
COMPONENT_CMAKE = (COMPONENT / "CMakeLists.txt").read_text(encoding="utf-8")

assert "CONFIG_FATFS_EXFAT=y" in DEFAULTS
assert 'config FATFS_EXFAT' in IDF_PATCH
assert "#ifdef CONFIG_FATFS_EXFAT" in IDF_PATCH
assert "#ifdef CONFIG_FATFS_USE_LABEL" in IDF_PATCH
assert "apply_idf_patch(patches/esp-idf-6.0.2-fatfs-exfat-config.patch)" in ROOT_CMAKE
assert ROOT_CMAKE.index("apply_idf_patch(") < ROOT_CMAKE.index("project(jetkvm-mini)")
assert 'set(JETKVM_TF_HTTP_PSRAM_DIRECT "1" CACHE STRING' in ROOT_CMAKE

for contract in (
    'if(JETKVM_BOARD_VARIANT STREQUAL "wifi")',
    'set(_jetkvm_tf_sdmmc_default_slot "0")',
    'set(_jetkvm_tf_sdmmc_default_slot "1")',
    'set(JETKVM_TF_SDMMC_SLOT "${_jetkvm_tf_sdmmc_default_slot}" CACHE STRING',
    'set(JETKVM_TF_SDMMC_FREQ_KHZ "40000" CACHE STRING',
    'set(JETKVM_TF_SDMMC_WIDTH "4" CACHE STRING',
    'set(JETKVM_TF_INTERNAL_PULLUPS "0" CACHE STRING',
    'set(JETKVM_TF_POWER_OFF_MS "500" CACHE STRING',
    'set(JETKVM_TF_BOOT_SELFTEST "0" CACHE STRING',
    'set(JETKVM_TF_BOOT_SELFTEST_MIB "32" CACHE STRING',
    'set(JETKVM_TF_RMT_TRACE "0" CACHE STRING',
    'set(JETKVM_TF_Q1_RC_TEST "0" CACHE STRING',
    'set(JETKVM_TF_MSC_PREFETCH "1" CACHE STRING',
    'set(JETKVM_TF_HTTP_PSRAM_DIRECT "1" CACHE STRING',
    "JETKVM_TF_SDMMC_SLOT=${JETKVM_TF_SDMMC_SLOT}",
    "JETKVM_TF_SDMMC_FREQ_KHZ=${JETKVM_TF_SDMMC_FREQ_KHZ}",
    "JETKVM_TF_SDMMC_WIDTH=${JETKVM_TF_SDMMC_WIDTH}",
    "JETKVM_TF_INTERNAL_PULLUPS=${JETKVM_TF_INTERNAL_PULLUPS}",
    "JETKVM_TF_POWER_OFF_MS=${JETKVM_TF_POWER_OFF_MS}",
    "JETKVM_TF_BOOT_SELFTEST=${JETKVM_TF_BOOT_SELFTEST}",
    "JETKVM_TF_BOOT_SELFTEST_MIB=${JETKVM_TF_BOOT_SELFTEST_MIB}",
    "JETKVM_TF_RMT_TRACE=${JETKVM_TF_RMT_TRACE}",
    "JETKVM_TF_Q1_RC_TEST=${JETKVM_TF_Q1_RC_TEST}",
    "JETKVM_TF_MSC_PREFETCH=${JETKVM_TF_MSC_PREFETCH}",
    "JETKVM_TF_HTTP_PSRAM_DIRECT=${JETKVM_TF_HTTP_PSRAM_DIRECT}",
):
    assert contract in COMPONENT_CMAKE, f"missing TF A/B build contract: {contract}"

for contract in (
    "#define TF_PIN_CLK GPIO_NUM_43",
    "#define TF_PIN_CMD GPIO_NUM_44",
    "#define TF_PIN_D0 GPIO_NUM_39",
    "#define TF_PIN_D1 GPIO_NUM_40",
    "#define TF_PIN_D2 GPIO_NUM_41",
    "#define TF_PIN_D3 GPIO_NUM_42",
    "#define TF_POWER_GPIO GPIO_NUM_46",
    "TF_BUS_PIN_MASK",
    '"TF bus forced low before power-off',
    "gpio_set_level(TF_POWER_GPIO, 1)",
    "vTaskDelay(pdMS_TO_TICKS(TF_POWER_OFF_DELAY_MS))",
    "gpio_set_level(TF_POWER_GPIO, 0)",
    "vTaskDelay(pdMS_TO_TICKS(TF_POWER_LOW_DELAY_MS))",
    "#define TF_POWER_LOW_DELAY_MS 50",
    "gpio_reset_pin(bus_pins[i])",
    "vTaskDelay(pdMS_TO_TICKS(TF_POWER_RELEASE_DELAY_MS))",
    '"TF bus released %d ms external-idle',
    "sdmmc_host_t host = SDMMC_HOST_DEFAULT()",
    "host.slot = JETKVM_TF_SDMMC_SLOT",
    "host.max_freq_khz = JETKVM_TF_SDMMC_FREQ_KHZ",
    "slot.flags |= SDMMC_SLOT_FLAG_UHS1",
    "host.flags |= SDMMC_HOST_FLAG_ALLOC_ALIGNED_BUF",
    '"TF card negotiated width=%u real_clock=%d kHz card_max=%" PRIu32',
    '"TF filesystem space used=%" PRIu64',
    "slot.width = JETKVM_TF_SDMMC_WIDTH",
    "slot.clk = TF_PIN_CLK",
    "slot.cmd = TF_PIN_CMD",
    "esp_vfs_fat_sdmmc_mount",
    'JETKVM_TF_SDMMC_FREQ_KHZ > SDMMC_FREQ_HIGHSPEED ? "on" : "off"',
    'JETKVM_TF_INTERNAL_PULLUPS ? "on" : "off"',
    "TF_AB SELFTEST_%s",
    "FIL file = {0}",
    "TF_SELFTEST_BYTES ((uint32_t)JETKVM_TF_BOOT_SELFTEST_MIB * 1024U * 1024U)",
    '"write_mib_s=%llu.%02llu read_ms=%lld read_mib_s=%llu.%02llu "',
    '"TF_BENCH FILE_PASS bytes=%u chunk=%u write_ms=%lld "',
    '"TF_BENCH RAW_READ_PASS bytes=%u chunk=%u elapsed_ms=%lld "',
):
    assert contract in SOURCE, f"missing TF pull-up/A/B contract: {contract}"

assert SOURCE.index("sdmmc_host_t host = SDMMC_HOST_DEFAULT()") < SOURCE.index(
    "host.slot = JETKVM_TF_SDMMC_SLOT"
), "selected slot must override the host default before use"
assert SOURCE.index("host.slot = JETKVM_TF_SDMMC_SLOT") < SOURCE.index(
    "esp_vfs_fat_sdmmc_mount"
), "native host slot must be selected before mounting"
for contract in (
    '#include "sd_pwr_ctrl_by_on_chip_ldo.h"',
    ".ldo_chan_id = 4",
    "host.pwr_ctrl_handle = s_ctx.io_power",
):
    assert contract in SOURCE, f"missing P4 VDD_IO_5 power contract: {contract}"
assert SOURCE.index("sd_pwr_ctrl_new_on_chip_ldo") < SOURCE.index(
    "gpio_config(&bus_low)"
), "VDD_IO_5 must be at 3.3 V before GPIO39..46 are touched"
powered_rc = SOURCE.index('run_tf_q1_rc_test("powered")')
power_off = SOURCE.index("gpio_set_level(TF_POWER_GPIO, 1)", powered_rc)
off_rc = SOURCE.index('run_tf_q1_rc_test("off_5s")', power_off)
power_on = SOURCE.index("gpio_set_level(TF_POWER_GPIO, 0)", off_rc)
assert powered_rc < power_off < off_rc < power_on, (
    "TF Q1 RC A/B must measure powered, discharge, measure off, then repower"
)
assert power_on < SOURCE.index(
    "esp_vfs_fat_sdmmc_mount"
), "TF power cycle must finish before native mounting"
assert SOURCE.index("TF bus forced low before power-off") < SOURCE.index(
    "gpio_set_level(TF_POWER_GPIO, 1)"
), "TF IOs must be clamped before removing card power"
assert SOURCE.index("gpio_reset_pin(bus_pins[i])") < SOURCE.index(
    "esp_vfs_fat_sdmmc_mount"
), "TF IOs must be released before the SDMMC driver takes ownership"

for forbidden in (
    "SDSPI_HOST_DEFAULT",
    "SDSPI_DEVICE_CONFIG_DEFAULT",
    "esp_vfs_fat_sdspi_mount",
    "spi_bus_initialize",
    "spi_bus_free",
    "driver/sdspi_host.h",
    "driver/spi_master.h",
    "JETKVM_TF_SPI_FREQ_KHZ",
):
    assert forbidden not in SOURCE + COMPONENT_CMAKE, (
        f"SDSPI experiment leaked into native production path: {forbidden}"
    )

for contract in (
    "_Static_assert(FF_FS_EXFAT == 1",
    "_Static_assert(sizeof(FSIZE_t) == sizeof(uint64_t)",
    "ff_diskio_get_pdrv_card(card)",
    '"%u:%s/%s"',
    "f_open(&s_ctx.image",
    "f_tell(&s_ctx.image) != (FSIZE_t)offset",
    "f_lseek(&s_ctx.image, (FSIZE_t)offset)",
    "f_read(&s_ctx.image",
    "f_open(&s_ctx.upload",
    "f_expand(&s_ctx.upload, (FSIZE_t)size, 0)",
    "f_lseek(&s_ctx.upload, (FSIZE_t)existing)",
    "f_write(&s_ctx.upload",
    "s_ctx.upload_written += slice_written",
    "f_close(&s_ctx.upload)",
    "f_rename(s_ctx.upload_incomplete_path",
    'filename_has_suffix_ascii_case_insensitive(filename, ".incomplete")',
):
    assert contract in SOURCE, f"missing direct FatFs contract: {contract}"

for contract in (
    "TF_UPLOAD_DMA_BOUNCE_BYTES",
    "TF_UPLOAD_OWNER_STAGING_BYTES (32U * 1024U)",
    "TF_UPLOAD_DMA_ALIGNMENT 128U",
    "static uint8_t s_tf_upload_dma_bounce",
    "__attribute__((aligned(TF_UPLOAD_DMA_ALIGNMENT)))",
    '#include "upload_dma_contract.h"',
    "jetkvm_upload_direct_write_eligible(",
    "jetkvm_upload_owner_staging_required(",
    "jetkvm_upload_bounce_slice_size(",
    "s_ctx.card->host.check_buffer_alignment(",
    "remaining - remaining % sector_size",
    "memcpy(bounce, source, slice_size)",
    "write_data = bounce",
    '"tf-upload-staging"',
    "MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA | MALLOC_CAP_8BIT",
):
    assert contract in SOURCE, f"missing upload DMA bounce contract: {contract}"

for contract in (
    'xTaskCreatePinnedToCoreWithCaps(',
    '"tf_prefetch"',
    'MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT',
    's_ctx.upload_owner_token !=',
    's_ctx.prefetch_ready_size >= size',
    'prefetch_schedule_locked(offset + size)',
    'prefetch_invalidate_locked();',
):
    assert contract in SOURCE, f"missing bounded MSC prefetch contract: {contract}"

claim_start = SOURCE.index("esp_err_t jetkvm_virtual_media_claim_upload")
claim_end = SOURCE.index("esp_err_t jetkvm_virtual_media_release_upload", claim_start)
claim_source = SOURCE[claim_start:claim_end]
assert claim_source.index("prefetch_invalidate_locked();") < claim_source.index(
    "s_ctx.upload_owner_token = claimed"
), "the writer token must invalidate the shared scratch before publication"

delete_start = SOURCE.index("esp_err_t jetkvm_virtual_media_delete_file")
delete_end = SOURCE.index("esp_err_t jetkvm_virtual_media_mount(", delete_start)
delete_source = SOURCE[delete_start:delete_end]
assert "s_ctx.image_open ? ESP_ERR_INVALID_STATE" in delete_source
assert "close_idle_upload_locked()" in delete_source

read_start = SOURCE.index("esp_err_t jetkvm_virtual_media_read(")
read_end = SOURCE.index("esp_err_t jetkvm_virtual_media_get_io_stats", read_start)
read_source = SOURCE[read_start:read_end]
assert "JETKVM_VIRTUAL_MEDIA_SOURCE_HTTP" in read_source
assert "s_ctx.upload_owner_token !=" in read_source

finish_start = SOURCE.index("static esp_err_t finish_upload_with_token")
finish_end = SOURCE.index(
    "esp_err_t jetkvm_virtual_media_finish_upload_owned", finish_start
)
finish_source = SOURCE[finish_start:finish_end]
assert "f_close(&s_ctx.upload)" in finish_source
assert "f_sync(&s_ctx.upload)" not in finish_source, (
    "f_close already performs the final FatFs synchronization"
)

for forbidden in (
    "fopen(",
    "fseeko(",
    "fread(",
    "fwrite(",
    "statvfs(",
    "JETKVM_VIRTUAL_MEDIA_MAX_FILE_SIZE",
):
    assert forbidden not in SOURCE + HEADER, f"legacy size-limited path remains: {forbidden}"

# Exercise host-side sparse offsets around both historical truncation edges.
with tempfile.TemporaryDirectory() as directory:
    image = Path(directory) / "large-image.iso"
    markers = {
        (2 << 30) - 1: b"A",
        (2 << 30) + 17: b"B",
        (4 << 30) - 1: b"C",
        (4 << 30) + 17: b"D",
        (5 << 30) + 12345: b"E",
    }
    with image.open("wb") as stream:
        for offset, marker in markers.items():
            stream.seek(offset)
            stream.write(marker)
    assert os.stat(image).st_size == max(markers) + 1
    with image.open("rb") as stream:
        for offset, marker in markers.items():
            stream.seek(offset)
            assert stream.read(1) == marker

size = (5 << 30) + 12346
assert (size + 2047) // 2048 < (1 << 32)
assert (size + 511) // 512 < (1 << 32)

print("virtual-media exFAT direct-I/O and >4GiB boundary contract: PASS")
