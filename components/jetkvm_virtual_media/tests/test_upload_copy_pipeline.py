#!/usr/bin/env python3
"""Static safety contract for the bounded upload copy/write pipeline."""

from pathlib import Path


COMPONENT = Path(__file__).resolve().parents[1]
SOURCE = (COMPONENT / "jetkvm_virtual_media.c").read_text(encoding="utf-8")
CMAKE = (COMPONENT / "CMakeLists.txt").read_text(encoding="utf-8")


def body(source: str, signature: str) -> str:
    start = source.index(signature)
    opening = source.index("{", start)
    depth = 1
    cursor = opening + 1
    while depth and cursor < len(source):
        if source[cursor] == "{":
            depth += 1
        elif source[cursor] == "}":
            depth -= 1
        cursor += 1
    assert depth == 0, f"unterminated function {signature}"
    return source[opening + 1 : cursor - 1]


# The unbenchmarked path is an explicit A/B switch and can only reuse the
# already-present prefetch worker; it must not silently add another task.
assert 'set(JETKVM_TF_UPLOAD_COPY_PIPELINE "0" CACHE STRING' in CMAKE
assert "JETKVM_TF_UPLOAD_COPY_PIPELINE=${JETKVM_TF_UPLOAD_COPY_PIPELINE}" in CMAKE
assert "JETKVM_TF_UPLOAD_COPY_PIPELINE reuses the MSC prefetch worker" in CMAKE

service = body(SOURCE, "static bool upload_copy_service(")
assert "atomic_compare_exchange_strong_explicit" in service
assert service.index("memcpy(destination, source, size)") < service.index(
    "xSemaphoreGive(s_ctx.upload_copy_done)"
)

prefetch = body(SOURCE, "static void tf_prefetch_task(")
assert prefetch.index("upload_copy_service()") < prefetch.index(
    "xSemaphoreTake(s_ctx.mutex, 0)"
), "copy jobs must not wait on the mutex held by the upload writer"
assert "TF_PREFETCH_MUTEX_POLL_TICKS" in prefetch

pipeline = body(SOURCE, "static FRESULT write_upload_staged_pipeline_locked(")
for contract in (
    "s_ctx.upload_staging + half_capacity",
    "jetkvm_upload_bounce_slice_size(",
    "upload_copy_schedule_locked(",
    "f_write(&s_ctx.upload",
    "upload_copy_wait_locked()",
    "s_ctx.upload_written += slice_written",
):
    assert contract in pipeline
assert "heap_caps_" not in pipeline
assert "xTaskCreate" not in pipeline
schedule = pipeline.index("upload_copy_schedule_locked(")
write = pipeline.index("f_write(&s_ctx.upload", schedule)
join = pipeline.index("upload_copy_wait_locked()", write)
failure = pipeline.index("if (result != FR_OK", join)
advance = pipeline.index("source += slice_written", failure)
assert schedule < write < join < failure < advance

write_core = body(SOURCE, "static esp_err_t write_upload_with_token(")
assert "jetkvm_upload_pipeline_half_capacity(" in write_core
assert "write_upload_staged_pipeline_locked(" in write_core
assert "pipeline_handled" in write_core

print("virtual-media 64 KiB upload copy pipeline contract: PASS")
