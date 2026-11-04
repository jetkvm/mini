#!/usr/bin/env python3
"""Static ownership contract across VM and the production HTTP producer."""

from pathlib import Path


COMPONENT = Path(__file__).resolve().parents[1]
REPO = COMPONENT.parents[1]
HEADER = (COMPONENT / "include/jetkvm_virtual_media.h").read_text(encoding="utf-8")
SOURCE = (COMPONENT / "jetkvm_virtual_media.c").read_text(encoding="utf-8")
HTTP = (
    REPO / "components/jetkvm_virtual_media_http/jetkvm_virtual_media_http.c"
).read_text(encoding="utf-8")


def body(source: str, symbol: str) -> str:
    start = source.index(symbol)
    opening = source.index("{", start)
    depth = 1
    cursor = opening + 1
    while depth and cursor < len(source):
        if source[cursor] == "{":
            depth += 1
        elif source[cursor] == "}":
            depth -= 1
        cursor += 1
    assert depth == 0, f"unterminated function {symbol}"
    return source[opening + 1 : cursor - 1]


for contract in (
    "JETKVM_VIRTUAL_MEDIA_UPLOAD_OWNER_HTTP",
    "JETKVM_VIRTUAL_MEDIA_UPLOAD_OWNER_WEBRTC",
    "jetkvm_virtual_media_upload_token_t",
    "jetkvm_virtual_media_claim_upload",
    "jetkvm_virtual_media_release_upload",
    "jetkvm_virtual_media_write_upload_owned",
    "jetkvm_virtual_media_finish_upload_owned",
    "jetkvm_virtual_media_abort_upload_owned",
):
    assert contract in HEADER, f"missing public upload ownership API: {contract}"

# Claim preflights under the VM mutex, reserves/allocates without holding it,
# then rechecks and atomically publishes staging plus token. A second producer
# cannot pass that recheck, and a stale release cannot clear the winner.
claim = body(SOURCE, "jetkvm_virtual_media_claim_upload(")
assert claim.index("lock_context()") < claim.index("s_ctx.upload_owner_token !=")
assert "return ESP_ERR_INVALID_STATE" in claim
assert "s_ctx.upload_close_pending" in claim
claim_gate_at = claim.index("s_ctx.upload_claim_pending = true")
reserve_at = claim.index("jetkvm_memory_policy_reserve")
allocate_at = claim.index("heap_caps_aligned_alloc", reserve_at)
recheck_at = claim.index("esp_err_t lock_error = lock_context()", allocate_at)
publish_staging_at = claim.index("s_ctx.upload_staging = staging", recheck_at)
publish_token_at = claim.index("s_ctx.upload_owner_token = claimed", publish_staging_at)
assert claim_gate_at < reserve_at < allocate_at < recheck_at
assert recheck_at < publish_staging_at < publish_token_at
assert claim.index("unlock_context();") < reserve_at
assert "finish_upload_claim_attempt(upload_id, true)" in claim
assert "bool claim_current = s_ctx.upload_claim_pending" in claim
assert claim.index("s_ctx.upload_claim_pending = false", recheck_at) < publish_staging_at
assert "s_ctx.upload_close_pending" in claim[recheck_at:publish_staging_at]
assert "!claim_current" in claim[recheck_at:publish_staging_at]
assert "TF_UPLOAD_OWNER_STAGING_BYTES" in claim
assert "MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA | MALLOC_CAP_8BIT" in claim
assert "s_ctx.upload_owner_token = claimed" in claim
release = body(SOURCE, "jetkvm_virtual_media_release_upload(")
assert "upload_token_matches(upload_id, token)" in release
assert release.index("upload_token_matches") < release.index("clear_upload_owner_locked")
assert release.index("clear_upload_owner_locked") < release.index(
    "heap_caps_free(staging)"
)

# Only the claimed writer writes or finishes; the token-less abort fails closed
# while either transport owns the upload.
write_core = body(SOURCE, "write_upload_with_token(")
assert "upload_token_matches(upload_id, token)" in write_core
finish_core = body(SOURCE, "finish_upload_with_token(")
abort_core = body(SOURCE, "abort_upload_with_token(")
assert "!s_ctx.upload_claim_pending" in abort_core
for operation in (finish_core, abort_core):
    assert "upload_token_matches(upload_id, token)" in operation
    assert "ESP_ERR_INVALID_STATE" in operation
    assert "clear_upload_owner_locked" in operation
    assert "heap_caps_free(staging)" in operation

# The writer hook follows the lease: it is told of a browser upload's lease in
# the statement that publishes it, and of its end in the one function that
# ends a lease. Every terminal path above goes through that function, so a
# path that ends the lease also ends what the hook holds (video, on the Wi-Fi
# Mini).
assert publish_token_at < claim.index(
    "notify_upload_writer_locked(owner, true)"
) < claim.index("unlock_context();", publish_token_at)
notify = body(SOURCE, "notify_upload_writer_locked(")
assert "JETKVM_VIRTUAL_MEDIA_UPLOAD_OWNER_HTTP" in notify
assert "JETKVM_VIRTUAL_MEDIA_UPLOAD_OWNER_DOWNLOAD" not in notify
# Only two statements take the token away. One is that function. The other
# sets up a new session, which a start reaches only after it has returned
# for a session that still has a writer.
ends_lease = "s_ctx.upload_owner_token = JETKVM_VIRTUAL_MEDIA_UPLOAD_TOKEN_INVALID"
assert SOURCE.count(ends_lease) == 2
assert ends_lease in body(SOURCE, "clear_upload_owner_locked(")
assert ends_lease in body(SOURCE, "initialize_upload_session_locked(")
new_session = body(SOURCE, "jetkvm_virtual_media_start_upload(")
writer_guard_at = new_session.index("if (s_ctx.upload_owner_token !=")
assert "return ESP_ERR_INVALID_STATE" in new_session[
    writer_guard_at : new_session.index("f_stat(", writer_guard_at)
]
assert writer_guard_at < new_session.index("initialize_upload_session_locked(")

clear_owner = body(SOURCE, "clear_upload_owner_locked(")
assert clear_owner.index(
    "notify_upload_writer_locked(s_ctx.upload_owner, false)"
) < clear_owner.index("s_ctx.upload_owner = 0")
for contract in (
    "s_ctx.upload_staging = NULL",
    "s_ctx.upload_staging_size = 0U",
    "staging_releases++",
    "staging_active = false",
    "staging_bytes = 0U",
    "s_ctx.upload_owner = 0",
    "JETKVM_VIRTUAL_MEDIA_UPLOAD_TOKEN_INVALID",
):
    assert contract in clear_owner

# A failed FatFs close keeps the FIL/upload identity retryable but relinquishes
# the transport token and dynamic staging allocation. Starting the exact same
# filename and size reuses that ownerless live upload when the caller uses the
# reserved no-digest identity; callers supplying a real digest remain strict.
close_at = abort_core.index("f_close(&s_ctx.upload)")
close_failure_at = abort_core.index("if (result != FR_OK)", close_at)
clear_failure_at = abort_core.index("clear_upload_owner_locked", close_failure_at)
return_failure_at = abort_core.index("return fat_result(result)", clear_failure_at)
mark_closed_at = abort_core.index("s_ctx.upload_open = false", return_failure_at)
assert close_at < close_failure_at < clear_failure_at < return_failure_at < mark_closed_at
start_upload = body(SOURCE, "jetkvm_virtual_media_start_upload(")
assert "bool resumable_open" in start_upload
for contract in (
    "s_ctx.upload_owner_token ==",
    "s_ctx.upload_staging == NULL",
    "!s_ctx.upload_claim_pending",
    "jetkvm_upload_resume_identity_matches(",
    "size, sha256_hex, s_ctx.upload_size",
    "s_ctx.upload_sha256_hex",
    "strcmp(s_ctx.upload_final_path, final_path) == 0",
    "upload->already_uploaded_bytes = s_ctx.upload_written",
    "if (s_ctx.upload_close_pending)",
    "FRESULT close_result = f_close(&s_ctx.upload)",
    "f_lseek(&s_ctx.upload, (FSIZE_t)s_ctx.upload_written)",
    "if (close_result != FR_OK)",
    "initialize_upload_session_locked(",
    "size, effective_sha256_hex, existing, true)",
):
    assert contract in start_upload
pending_close_at = start_upload.index("if (s_ctx.upload_close_pending)")
retry_close_at = start_upload.index(
    "FRESULT close_result = f_close(&s_ctx.upload)", pending_close_at
)
reopen_path_at = start_upload.index("f_open(&s_ctx.upload", retry_close_at)
assert pending_close_at < retry_close_at < reopen_path_at
resume_seek_at = start_upload.index(
    "f_lseek(&s_ctx.upload, (FSIZE_t)s_ctx.upload_written)", retry_close_at
)
resume_seek_failure_at = start_upload.index(
    "if (seek_result != FR_OK)", resume_seek_at
)
resume_close_at = start_upload.index(
    "FRESULT close_result = f_close(&s_ctx.upload)", resume_seek_failure_at
)
resume_mark_pending_at = start_upload.index(
    "s_ctx.upload_close_pending = true", resume_close_at
)
assert resume_seek_at < resume_seek_failure_at < resume_close_at
assert resume_close_at < resume_mark_pending_at < reopen_path_at
setup_close_at = start_upload.index("FRESULT close_result = f_close")
setup_close_failure_at = start_upload.index(
    "if (close_result != FR_OK)", setup_close_at
)
track_live_fil_at = start_upload.index(
    "initialize_upload_session_locked(",
    setup_close_failure_at,
)
return_setup_error_at = start_upload.index("return fat_result(result)", track_live_fil_at)
assert setup_close_at < setup_close_failure_at < track_live_fil_at < return_setup_error_at

# HTTP claims before reserving buffers. Admission/setup failure only releases
# that exact capability; data/terminal paths always carry it into VM APIs.
http_handler = body(HTTP, "upload_handler_sync(")
claim_at = http_handler.index("jetkvm_virtual_media_claim_upload")
reserve_at = http_handler.index("jetkvm_memory_policy_reserve")
assert claim_at < reserve_at
admission_failure = http_handler[
    reserve_at : http_handler.index("uint8_t *buffers", reserve_at)
]
assert "jetkvm_virtual_media_release_upload(upload_id, owner_token)" in admission_failure
assert "jetkvm_virtual_media_abort_upload" not in admission_failure
assert "jetkvm_virtual_media_finish_upload_owned" in http_handler
assert "jetkvm_virtual_media_abort_upload_owned" in http_handler
http_writer = body(HTTP, "upload_writer_task(")
assert "jetkvm_virtual_media_write_upload_owned" in http_writer
assert "pipeline->owner_token" in http_writer

print("virtual-media HTTP exclusive upload owner contract: PASS")
