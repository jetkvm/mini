#!/usr/bin/env python3
"""Static fail-closed contracts for persisted resumable-upload identity."""

from pathlib import Path


COMPONENT = Path(__file__).resolve().parents[1]
HEADER = (COMPONENT / "include/jetkvm_virtual_media.h").read_text(encoding="utf-8")
PUBLIC_IDENTITY = (
    COMPONENT / "include/jetkvm_virtual_media_upload_identity.h"
).read_text(encoding="utf-8")
SOURCE = (COMPONENT / "jetkvm_virtual_media.c").read_text(encoding="utf-8")
IDENTITY = (COMPONENT / "upload_identity_contract.h").read_text(encoding="utf-8")


def body(symbol: str) -> str:
    start = SOURCE.index(symbol)
    opening = SOURCE.index("{", start)
    depth = 1
    cursor = opening + 1
    while depth and cursor < len(SOURCE):
        if SOURCE[cursor] == "{":
            depth += 1
        elif SOURCE[cursor] == "}":
            depth -= 1
        cursor += 1
    assert depth == 0, f"unterminated function {symbol}"
    return SOURCE[opening + 1 : cursor - 1]


assert (
    "const char *filename, uint64_t size, const char *sha256_hex,\n"
    "    jetkvm_virtual_media_upload_t *upload"
) in HEADER
for contract in (
    '#define TF_UPLOAD_INCOMPLETE_SUFFIX ".incomplete"',
    '#define TF_UPLOAD_IDENTITY_SUFFIX ".identity"',
    '#define TF_UPLOAD_IDENTITY_TEMP_SUFFIX ".tmp"',
    "char upload_identity_path[",
    "char upload_sha256_hex[JETKVM_UPLOAD_SHA256_HEX_LENGTH + 1U]",
    "FIL upload_metadata;",
    "tf_upload_metadata_role_t upload_metadata_role;",
    "bool upload_metadata_open;",
    "bool upload_metadata_close_pending;",
    "bool upload_metadata_cleanup_pending;",
    "char upload_metadata_path[",
):
    assert contract in SOURCE, f"missing persistent identity state: {contract}"
for contract in (
    "JETKVM_UPLOAD_IDENTITY_RECORD_PAYLOAD_SIZE 88U",
    "JETKVM_UPLOAD_IDENTITY_RECORD_SIZE 92U",
    "jetkvm_upload_identity_crc32(",
    "record + JETKVM_UPLOAD_IDENTITY_RECORD_PAYLOAD_SIZE",
    "if (stored_crc != computed_crc)",
):
    assert contract in IDENTITY, f"missing identity CRC contract: {contract}"
assert "JETKVM_VIRTUAL_MEDIA_UPLOAD_SHA256_UNAVAILABLE" in PUBLIC_IDENTITY
assert PUBLIC_IDENTITY.count('"00000000000000000000000000000000"') == 2
for contract in (
    '#include "jetkvm_virtual_media_upload_identity.h"',
    "jetkvm_upload_resume_identity_matches(",
    "JETKVM_VIRTUAL_MEDIA_UPLOAD_SHA256_UNAVAILABLE",
):
    assert contract in IDENTITY, f"missing Go-compatible identity contract: {contract}"

publish = body("static esp_err_t publish_upload_identity_locked(")
write_at = publish.index("f_write(&s_ctx.upload_metadata")
sync_at = publish.index("f_sync(&s_ctx.upload_metadata)", write_at)
close_at = publish.index("close_upload_metadata_locked(", sync_at)
rename_at = publish.index("f_rename(temporary_path, identity_path)", close_at)
assert write_at < sync_at < close_at < rename_at
assert "FA_WRITE | FA_CREATE_NEW" in publish
assert "if (close_error != ESP_OK)" in publish
assert publish.index("if (close_error != ESP_OK)") < rename_at

read = body("static esp_err_t read_upload_identity_locked(")
for contract in (
    "info.fsize != JETKVM_UPLOAD_IDENTITY_RECORD_SIZE",
    "open_upload_metadata_locked(",
    "identity_path, FA_READ | FA_OPEN_EXISTING",
    "f_read(&s_ctx.upload_metadata, record, sizeof(record), &read_size)",
    "close_upload_metadata_locked(false)",
    "jetkvm_upload_identity_decode",
):
    assert contract in read

service = body("static esp_err_t service_upload_metadata_locked(")
close_at = service.index("f_close(&s_ctx.upload_metadata)")
cleanup_at = service.index("unlink_upload_artifact_locked(", close_at)
clear_at = service.index("clear_upload_metadata_tracking_locked()", cleanup_at)
assert close_at < cleanup_at < clear_at
assert "if (close_result != FR_OK)" in service[close_at:cleanup_at]
metadata_close = body("static esp_err_t close_upload_metadata_locked(")
assert "s_ctx.upload_metadata_cleanup_pending = true" in metadata_close
assert "s_ctx.upload_metadata_close_pending = true" in metadata_close
assert metadata_close.index("f_close(&s_ctx.upload_metadata)") < metadata_close.index(
    "s_ctx.upload_metadata_cleanup_pending = true"
)

ensure = body("static esp_err_t ensure_upload_identity_locked(")
match_at = ensure.index("jetkvm_upload_resume_identity_matches(")
nonzero_gate_at = ensure.index("if (existing > 0U)", match_at)
replace_at = ensure.index("unlink_upload_artifact_locked(identity_path", nonzero_gate_at)
publish_at = ensure.index("publish_upload_identity_locked", replace_at)
assert match_at < nonzero_gate_at < replace_at < publish_at
assert "ESP_ERR_INVALID_STATE" in ensure[nonzero_gate_at:replace_at]
assert "stored_sha256_hex, effective_sha256_hex" in ensure
assert "expected_sha256_hex, effective_sha256_hex" in ensure

start = body("esp_err_t jetkvm_virtual_media_start_upload(")
for contract in (
    "jetkvm_upload_sha256_hex_valid(sha256_hex)",
    "jetkvm_upload_resume_identity_matches(\n"
    "                size, sha256_hex, s_ctx.upload_size,",
    "ensure_upload_identity_locked(\n"
    "        identity_path, temporary_path, existing, size, sha256_hex,\n"
    "        effective_sha256_hex)",
    "initialize_upload_session_locked(\n"
    "        final_path, incomplete_path, identity_path,",
    "size, effective_sha256_hex, existing, false",
):
    assert contract in start
assert start.index("service_upload_metadata_locked()") < start.index(
    "if (s_ctx.upload_open)"
)
identity_at = start.index("ensure_upload_identity_locked(")
open_at = start.index("f_open(&s_ctx.upload, incomplete_path", identity_at)
assert identity_at < open_at, "identity must be durable before payload creation"
final_stat_at = start.index("f_stat(final_path")
final_reject_at = start.index("if (final_result == FR_OK)", final_stat_at)
incomplete_stat_at = start.index("f_stat(incomplete_path", final_reject_at)
assert final_stat_at < final_reject_at < incomplete_stat_at

listing = body("esp_err_t jetkvm_virtual_media_list_files(")
assert listing.index("service_upload_metadata_locked()") < listing.index("f_opendir(")
assert "filename_is_upload_identity_metadata(entry.fname)" in listing
assert listing.index("filename_is_upload_identity_metadata(entry.fname)") < listing.index(
    "list->files[list->count++]"
)

delete = body("esp_err_t jetkvm_virtual_media_delete_file(")
for contract in (
    "!filename_is_upload_identity_metadata(filename)",
    "filename_has_suffix_ascii_case_insensitive(\n"
    "            filename, TF_UPLOAD_INCOMPLETE_SUFFIX)",
    "s_ctx.image_open ? ESP_ERR_INVALID_STATE",
    "close_idle_upload_locked()",
    "append_path_suffix(\n"
    "        path, TF_UPLOAD_IDENTITY_SUFFIX",
    "unlink_upload_artifact_locked(\n"
    "            identity_path, &identity_removed)",
    "unlink_upload_artifact_locked(\n"
    "            temporary_path, &temporary_removed)",
):
    assert contract in delete
assert delete.index("service_upload_metadata_locked()") < delete.index("f_unlink(path)")

valid_filename = body("static bool valid_filename(")
assert "filename[length - 1U] == ' '" in valid_filename
assert "filename[length - 1U] == '.'" in valid_filename

mount = body("esp_err_t jetkvm_virtual_media_mount(")
assert mount.index("service_upload_metadata_locked()") < mount.index("f_stat(path")
assert "s_ctx.image_open ? ESP_ERR_INVALID_STATE" in mount
assert "close_idle_upload_locked()" in mount

suffix = body("static bool filename_has_suffix_ascii_case_insensitive(")
assert "jetkvm_upload_filename_has_suffix_ci(filename, suffix)" in suffix
assert "jetkvm_upload_ascii_lower(candidate[i])" in IDENTITY

finish = body("static esp_err_t finish_upload_with_token(")
payload_rename_at = finish.index(
    "f_rename(s_ctx.upload_incomplete_path, s_ctx.upload_final_path)"
)
identity_unlink_at = finish.index(
    "queue_upload_metadata_cleanup_locked(\n"
    "            s_ctx.upload_identity_path)",
    payload_rename_at,
)
clear_at = finish.index("clear_upload_session_locked()", identity_unlink_at)
assert payload_rename_at < identity_unlink_at < clear_at
assert "if (result == FR_OK)" in finish[payload_rename_at:identity_unlink_at]

abort = body("static esp_err_t abort_upload_with_token(")
assert "unlink_upload_artifact_locked" not in abort
assert abort.index("f_close(&s_ctx.upload)") < abort.index(
    "clear_upload_session_locked()"
)

print("virtual-media persistent upload identity contract: PASS")
