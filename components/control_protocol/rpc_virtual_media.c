/*
 * SPDX-FileCopyrightText: 2026
 * SPDX-License-Identifier: MIT
 */

#ifndef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 200809L
#endif

#include "control_protocol_internal.h"

#include <stdio.h>
#include <string.h>
#include <time.h>

#include "jetkvm_virtual_media_upload_identity.h"
#if JETKVM_TF_CARD_TEST
#include "jetkvm_virtual_media.h"
#endif

static bool set_virtual_media_error(
    rpc_dispatch_result_t *dispatch,
    jetkvm_virtual_media_change_result_t result, const char *operation)
{
    if (result == JETKVM_VIRTUAL_MEDIA_CHANGE_COMPLETED) {
        dispatch->result = cJSON_CreateNull();
        return dispatch->result != NULL;
    }
    dispatch->error_code = result == JETKVM_VIRTUAL_MEDIA_CHANGE_INVALID ?
                               -32602 : -32603;
    dispatch->error_message = result == JETKVM_VIRTUAL_MEDIA_CHANGE_INVALID ?
                                  "Invalid params" : "Internal error";
    switch (result) {
    case JETKVM_VIRTUAL_MEDIA_CHANGE_NOT_FOUND:
        dispatch->error_data = "storage image not found";
        break;
    case JETKVM_VIRTUAL_MEDIA_CHANGE_BUSY:
        dispatch->error_data = "another virtual media operation is active";
        break;
    case JETKVM_VIRTUAL_MEDIA_CHANGE_UNAVAILABLE:
        dispatch->error_data = "virtual media storage unavailable";
        break;
    case JETKVM_VIRTUAL_MEDIA_CHANGE_URL_UNAVAILABLE:
        /* Upstream's text; the UI and its e2e suite match on it. */
        dispatch->error_data =
            "The URL is not available. Check the image URL and try again.";
        break;
    case JETKVM_VIRTUAL_MEDIA_CHANGE_INVALID:
        dispatch->error_data = "invalid virtual media parameters";
        break;
    case JETKVM_VIRTUAL_MEDIA_CHANGE_ALREADY_MOUNTED:
        dispatch->error_data = "An image is already mounted. Unmount it first.";
        break;
    default:
        dispatch->error_data = operation;
        break;
    }
    return true;
}

/* A write to the TF card fails beside the flash writes of a firmware update,
 * so no upload, download or format of the card starts while an update is in
 * progress. */
static bool refuse_card_write_during_update(
    const jetkvm_control_context_t *context, rpc_dispatch_result_t *dispatch,
    const char *message)
{
    const jetkvm_ota_context_t *ota = ota_context(context);
    if (!ota->is_update_pending || !ota->is_update_pending(ota->ctx)) {
        return false;
    }
    dispatch->error_code = -32603;
    dispatch->error_message = "Internal error";
    dispatch->error_data = message;
    return true;
}

static bool refuse_during_update(const jetkvm_control_context_t *context,
                                 rpc_dispatch_result_t *dispatch)
{
    return refuse_card_write_during_update(
        context, dispatch,
        "A firmware update is in progress. Upload the file after the update "
        "has finished.");
}

bool rpc_get_virtual_media_state(
    const jetkvm_control_context_t *context, const cJSON *params,
    rpc_dispatch_result_t *dispatch)
{
    if (!require_no_rpc_params(params, dispatch)) {
        return true;
    }
    jetkvm_control_virtual_media_state_t state = {0};
    if (!context->virtual_media.get_state ||
        !context->virtual_media.get_state(&state, context->virtual_media.ctx)) {
        return set_virtual_media_error(
            dispatch, JETKVM_VIRTUAL_MEDIA_CHANGE_UNAVAILABLE,
            "failed to read virtual media state");
    }
    if (!state.mounted) {
        dispatch->result = cJSON_CreateNull();
        return dispatch->result != NULL;
    }
    cJSON *result = cJSON_CreateObject();
    const char *source =
        state.source == JETKVM_CONTROL_VIRTUAL_MEDIA_SOURCE_HTTP ? "HTTP" :
        state.source == JETKVM_CONTROL_VIRTUAL_MEDIA_SOURCE_BUILTIN ?
            "Builtin" : "Storage";
    if (!result || !cJSON_AddStringToObject(result, "source", source) ||
        !cJSON_AddStringToObject(
            result, "mode",
            state.mode == JETKVM_CONTROL_VIRTUAL_MEDIA_MODE_CDROM ?
                "CDROM" : "Disk")) {
        cJSON_Delete(result);
        return false;
    }
    bool source_ok = state.source == JETKVM_CONTROL_VIRTUAL_MEDIA_SOURCE_HTTP ?
        cJSON_AddStringToObject(result, "url", state.url) != NULL :
        cJSON_AddStringToObject(result, "filename", state.filename) != NULL;
    if (!source_ok ||
        !cJSON_AddNumberToObject(result, "size", (double)state.size)) {
        cJSON_Delete(result);
        return false;
    }
    dispatch->result = result;
    return true;
}

bool rpc_get_storage_state(const jetkvm_control_context_t *context,
                           const cJSON *params,
                           rpc_dispatch_result_t *dispatch)
{
    if (!require_no_rpc_params(params, dispatch)) {
        return true;
    }
    jetkvm_control_virtual_media_state_t state = {0};
    if (!context->virtual_media.get_state ||
        !context->virtual_media.get_state(&state, context->virtual_media.ctx)) {
        return set_virtual_media_error(
            dispatch, JETKVM_VIRTUAL_MEDIA_CHANGE_UNAVAILABLE,
            "failed to read storage state");
    }
    cJSON *result = cJSON_CreateObject();
    if (!result ||
        !cJSON_AddBoolToObject(result, "initialized", state.initialized) ||
        !cJSON_AddBoolToObject(result, "cardPresent", state.card_present) ||
        !cJSON_AddNumberToObject(result, "cardGeneration",
                                 (double)state.card_generation) ||
        !cJSON_AddBoolToObject(result, "filesystemMounted",
                              state.filesystem_mounted)) {
        cJSON_Delete(result);
        return false;
    }
    dispatch->result = result;
    return true;
}

static const char *storage_format_refusal(
    jetkvm_control_storage_format_result_t result)
{
    switch (result) {
    case JETKVM_CONTROL_STORAGE_FORMAT_NOT_READY:
        return "virtual media storage unavailable";
    case JETKVM_CONTROL_STORAGE_FORMAT_NO_CARD:
        return "No TF card is inserted.";
    case JETKVM_CONTROL_STORAGE_FORMAT_IMAGE_MOUNTED:
        return "An image is mounted. Unmount it, then format the card.";
    case JETKVM_CONTROL_STORAGE_FORMAT_UPLOAD_ACTIVE:
        return "A file is being written to the TF card. Format the card "
               "after the upload has finished.";
    case JETKVM_CONTROL_STORAGE_FORMAT_BUSY:
        return "another virtual media operation is active";
    default:
        return "failed to format storage as exFAT";
    }
}

/* The card is erased: a new exFAT filesystem with an empty images folder.
 * The result is the new filesystem's space, as getStorageSpace gives it. */
bool rpc_format_storage(const jetkvm_control_context_t *context,
                        const cJSON *params,
                        rpc_dispatch_result_t *dispatch)
{
    if (!require_no_rpc_params(params, dispatch)) {
        return true;
    }
    if (refuse_card_write_during_update(
            context, dispatch,
            "A firmware update is in progress. Format the card after the "
            "update has finished.")) {
        return true;
    }
    jetkvm_control_storage_space_t space = {0};
    const jetkvm_control_storage_format_result_t result =
        context->virtual_media.format_storage ?
            context->virtual_media.format_storage(
                &space, context->virtual_media.ctx) :
            JETKVM_CONTROL_STORAGE_FORMAT_NOT_READY;
    if (result != JETKVM_CONTROL_STORAGE_FORMAT_COMPLETED) {
        dispatch->error_code = -32603;
        dispatch->error_message = "Internal error";
        dispatch->error_data = storage_format_refusal(result);
        return true;
    }
    cJSON *object = cJSON_CreateObject();
    if (!object ||
        !cJSON_AddNumberToObject(object, "bytesUsed",
                                 (double)space.bytes_used) ||
        !cJSON_AddNumberToObject(object, "bytesFree",
                                 (double)space.bytes_free)) {
        cJSON_Delete(object);
        return false;
    }
    dispatch->result = object;
    return true;
}

bool rpc_get_storage_space(const jetkvm_control_context_t *context,
                                  const cJSON *params,
                                  rpc_dispatch_result_t *dispatch)
{
    if (!require_no_rpc_params(params, dispatch)) {
        return true;
    }
    jetkvm_control_storage_space_t space = {0};
    if (!context->virtual_media.get_space ||
        !context->virtual_media.get_space(&space, context->virtual_media.ctx)) {
        return set_virtual_media_error(
            dispatch, JETKVM_VIRTUAL_MEDIA_CHANGE_UNAVAILABLE,
            "failed to read storage space");
    }
    cJSON *result = cJSON_CreateObject();
    if (!result ||
        !cJSON_AddNumberToObject(result, "bytesUsed", (double)space.bytes_used) ||
        !cJSON_AddNumberToObject(result, "bytesFree", (double)space.bytes_free)) {
        cJSON_Delete(result);
        return false;
    }
    dispatch->result = result;
    return true;
}

typedef struct {
    cJSON *array;
    bool render_failed;
} storage_file_render_context_t;

static bool render_storage_file(const jetkvm_control_storage_file_t *file,
                                void *ctx)
{
    storage_file_render_context_t *render = ctx;
    char created_at[32];
    time_t seconds = (time_t)file->created_at;
    struct tm utc;
    bool ok = gmtime_r(&seconds, &utc) != NULL &&
        strftime(created_at, sizeof(created_at),
                 "%Y-%m-%dT%H:%M:%SZ", &utc) > 0U;
    cJSON *entry = ok ? cJSON_CreateObject() : NULL;
    ok = entry != NULL &&
        cJSON_AddStringToObject(entry, "filename", file->filename) &&
        cJSON_AddNumberToObject(entry, "size", (double)file->size) &&
        cJSON_AddStringToObject(entry, "createdAt", created_at);
    if (ok) {
        cJSON_AddItemToArray(render->array, entry);
    } else {
        cJSON_Delete(entry);
        render->render_failed = true;
    }
    return ok;
}

bool rpc_list_storage_files(const jetkvm_control_context_t *context,
                                   const cJSON *params,
                                   rpc_dispatch_result_t *dispatch)
{
    if (!require_no_rpc_params(params, dispatch)) {
        return true;
    }
    cJSON *result = cJSON_CreateObject();
    cJSON *array = result ? cJSON_AddArrayToObject(result, "files") : NULL;
    if (array == NULL) {
        cJSON_Delete(result);
        return false;
    }
    storage_file_render_context_t render = {
        .array = array,
    };
    if (!context->virtual_media.list_files ||
        !context->virtual_media.list_files(
            render_storage_file, &render, context->virtual_media.ctx)) {
        cJSON_Delete(result);
        if (render.render_failed) {
            return false;
        }
        return set_virtual_media_error(
            dispatch, JETKVM_VIRTUAL_MEDIA_CHANGE_UNAVAILABLE,
            "failed to list storage files");
    }
    dispatch->result = result;
    return true;
}

static bool parse_virtual_media_mode(
    const cJSON *item, jetkvm_control_virtual_media_mode_t *mode)
{
    if (!cJSON_IsString(item) || item->valuestring == NULL) {
        return false;
    }
    if (strcmp(item->valuestring, "CDROM") == 0) {
        *mode = JETKVM_CONTROL_VIRTUAL_MEDIA_MODE_CDROM;
        return true;
    }
    if (strcmp(item->valuestring, "Disk") == 0) {
        *mode = JETKVM_CONTROL_VIRTUAL_MEDIA_MODE_DISK;
        return true;
    }
    return false;
}

static bool object_has_exactly_two_members(const cJSON *object,
                                           const cJSON *first,
                                           const cJSON *second)
{
    if (!cJSON_IsObject(object) || first == NULL || second == NULL ||
        first == second) {
        return false;
    }
    size_t count = 0U;
    for (const cJSON *item = object->child; item != NULL; item = item->next) {
        if (item != first && item != second) {
            return false;
        }
        count++;
    }
    return count == 2U;
}

static bool object_has_exactly_three_members(const cJSON *object,
                                             const cJSON *first,
                                             const cJSON *second,
                                             const cJSON *third)
{
    if (!cJSON_IsObject(object) || first == NULL || second == NULL ||
        third == NULL || first == second || first == third || second == third) {
        return false;
    }
    size_t count = 0U;
    for (const cJSON *item = object->child; item != NULL; item = item->next) {
        if (item != first && item != second && item != third) {
            return false;
        }
        count++;
    }
    return count == 3U;
}

bool rpc_mount_with_storage(const jetkvm_control_context_t *context,
                                   const cJSON *params,
                                   rpc_dispatch_result_t *dispatch)
{
    const cJSON *filename = cJSON_IsObject(params) ?
        cJSON_GetObjectItemCaseSensitive(params, "filename") : NULL;
    const cJSON *mode_item = cJSON_IsObject(params) ?
        cJSON_GetObjectItemCaseSensitive(params, "mode") : NULL;
    jetkvm_control_virtual_media_mode_t mode;
    if (!cJSON_IsString(filename) || filename->valuestring == NULL ||
        !parse_virtual_media_mode(mode_item, &mode) ||
        !object_has_exactly_two_members(params, filename, mode_item)) {
        return set_virtual_media_error(
            dispatch, JETKVM_VIRTUAL_MEDIA_CHANGE_INVALID,
            "failed to mount storage image");
    }
    return set_virtual_media_error(
        dispatch, context->virtual_media.mount_storage ?
            context->virtual_media.mount_storage(
                filename->valuestring, mode, context->virtual_media.ctx) :
            JETKVM_VIRTUAL_MEDIA_CHANGE_UNAVAILABLE,
        "failed to mount storage image");
}

bool rpc_check_mount_url(const jetkvm_control_context_t *context,
                                const cJSON *params,
                                rpc_dispatch_result_t *dispatch)
{
    const cJSON *url = cJSON_IsObject(params) ?
        cJSON_GetObjectItemCaseSensitive(params, "url") : NULL;
    if (!cJSON_IsString(url) || url->valuestring == NULL ||
        params->child != url || url->next != NULL) {
        return set_virtual_media_error(
            dispatch, JETKVM_VIRTUAL_MEDIA_CHANGE_INVALID,
            "failed to check mount URL");
    }
    jetkvm_control_virtual_media_url_info_t info = {0};
    if (!context->virtual_media.check_url ||
        !context->virtual_media.check_url(url->valuestring, &info,
                                          context->virtual_media.ctx)) {
        return set_virtual_media_error(
            dispatch, JETKVM_VIRTUAL_MEDIA_CHANGE_UNAVAILABLE,
            "failed to check mount URL");
    }
    cJSON *result = cJSON_CreateObject();
    if (!result ||
        !cJSON_AddBoolToObject(result, "usable", info.usable) ||
        !cJSON_AddNumberToObject(result, "size", (double)info.size) ||
        (info.reason[0] != '\0' &&
         !cJSON_AddStringToObject(result, "reason", info.reason))) {
        cJSON_Delete(result);
        return false;
    }
    dispatch->result = result;
    return true;
}

bool rpc_mount_with_http(const jetkvm_control_context_t *context,
                                const cJSON *params,
                                rpc_dispatch_result_t *dispatch)
{
    const cJSON *url = cJSON_IsObject(params) ?
        cJSON_GetObjectItemCaseSensitive(params, "url") : NULL;
    const cJSON *mode_item = cJSON_IsObject(params) ?
        cJSON_GetObjectItemCaseSensitive(params, "mode") : NULL;
    jetkvm_control_virtual_media_mode_t mode;
    if (!cJSON_IsString(url) || url->valuestring == NULL ||
        !parse_virtual_media_mode(mode_item, &mode) ||
        !object_has_exactly_two_members(params, url, mode_item)) {
        return set_virtual_media_error(
            dispatch, JETKVM_VIRTUAL_MEDIA_CHANGE_INVALID,
            "failed to mount HTTP image");
    }
    return set_virtual_media_error(
        dispatch, context->virtual_media.mount_http ?
            context->virtual_media.mount_http(
                url->valuestring, mode, context->virtual_media.ctx) :
            JETKVM_VIRTUAL_MEDIA_CHANGE_UNAVAILABLE,
        "failed to mount HTTP image");
}

/* The installer disc in the firmware, as a CD-ROM. A mounted image stays:
 * the client unmounts it first. */
bool rpc_mount_builtin_installer(const jetkvm_control_context_t *context,
                                 const cJSON *params,
                                 rpc_dispatch_result_t *dispatch)
{
    if (!require_no_rpc_params(params, dispatch)) {
        return true;
    }
    return set_virtual_media_error(
        dispatch, context->virtual_media.mount_builtin_installer ?
            context->virtual_media.mount_builtin_installer(
                context->virtual_media.ctx) :
            JETKVM_VIRTUAL_MEDIA_CHANGE_UNAVAILABLE,
        "failed to mount the installer");
}

bool rpc_unmount_image(const jetkvm_control_context_t *context,
                              const cJSON *params,
                              rpc_dispatch_result_t *dispatch)
{
    if (!require_no_rpc_params(params, dispatch)) {
        return true;
    }
    return set_virtual_media_error(
        dispatch, context->virtual_media.unmount ?
            context->virtual_media.unmount(context->virtual_media.ctx) :
            JETKVM_VIRTUAL_MEDIA_CHANGE_UNAVAILABLE,
        "failed to unmount image");
}

bool rpc_delete_storage_file(const jetkvm_control_context_t *context,
                                    const cJSON *params,
                                    rpc_dispatch_result_t *dispatch)
{
    const cJSON *filename = cJSON_IsObject(params) ?
        cJSON_GetObjectItemCaseSensitive(params, "filename") : NULL;
    if (!cJSON_IsString(filename) || filename->valuestring == NULL ||
        params->child != filename || filename->next != NULL) {
        return set_virtual_media_error(
            dispatch, JETKVM_VIRTUAL_MEDIA_CHANGE_INVALID,
            "failed to delete storage image");
    }
    const jetkvm_virtual_media_change_result_t result =
        context->virtual_media.delete_file ?
            context->virtual_media.delete_file(filename->valuestring,
                                               context->virtual_media.ctx) :
            JETKVM_VIRTUAL_MEDIA_CHANGE_UNAVAILABLE;
    const bool handled = set_virtual_media_error(
        dispatch, result, "failed to delete storage image");
    if (result == JETKVM_VIRTUAL_MEDIA_CHANGE_NOT_FOUND) {
        /* Upstream's text; its e2e cleanup ignores exactly this message.
         * The buffer holds the prefix plus any filename the API accepts. */
        _Static_assert(sizeof(dispatch->error_storage) >=
                           sizeof("file does not exist: ") +
                               JETKVM_VIRTUAL_MEDIA_FILENAME_CAPACITY - 1U,
                       "error_storage too small for a storage filename");
        snprintf(dispatch->error_storage, sizeof(dispatch->error_storage),
                 "file does not exist: %s", filename->valuestring);
        dispatch->error_data = dispatch->error_storage;
    }
    return handled;
}

bool rpc_start_storage_file_upload(
    const jetkvm_control_context_t *context, const cJSON *params,
    rpc_dispatch_result_t *dispatch)
{
    const cJSON *filename = cJSON_IsObject(params) ?
        cJSON_GetObjectItemCaseSensitive(params, "filename") : NULL;
    const cJSON *size = cJSON_IsObject(params) ?
        cJSON_GetObjectItemCaseSensitive(params, "size") : NULL;
    const cJSON *sha256 = cJSON_IsObject(params) ?
        cJSON_GetObjectItemCaseSensitive(params, "sha256") : NULL;
    bool sha256_present = sha256 != NULL;
    bool sha256_valid = !sha256_present ||
        (cJSON_IsString(sha256) && sha256->valuestring != NULL &&
         strlen(sha256->valuestring) == 64U);
    for (size_t i = 0U; sha256_present && sha256_valid && i < 64U; ++i) {
        sha256_valid = hex_nibble(sha256->valuestring[i]) >= 0;
    }
    bool members_valid = sha256_present ?
        object_has_exactly_three_members(params, filename, size, sha256) :
        object_has_exactly_two_members(params, filename, size);
    if (!cJSON_IsString(filename) || filename->valuestring == NULL ||
        !cJSON_IsNumber(size) || size->valuedouble <= 0.0 ||
        size->valuedouble > (double)UINT64_MAX ||
        size->valuedouble != (double)(uint64_t)size->valuedouble ||
        !sha256_valid || !members_valid) {
        return set_virtual_media_error(
            dispatch, JETKVM_VIRTUAL_MEDIA_CHANGE_INVALID,
            "failed to start storage upload");
    }
    if (refuse_during_update(context, dispatch)) {
        return true;
    }
    jetkvm_control_storage_upload_t upload = {0};
    jetkvm_virtual_media_change_result_t result =
        context->virtual_media.start_upload ?
            context->virtual_media.start_upload(
                filename->valuestring, (uint64_t)size->valuedouble,
                sha256_present ? sha256->valuestring :
                    JETKVM_VIRTUAL_MEDIA_UPLOAD_SHA256_UNAVAILABLE,
                &upload, context->virtual_media.ctx) :
            JETKVM_VIRTUAL_MEDIA_CHANGE_UNAVAILABLE;
    if (result != JETKVM_VIRTUAL_MEDIA_CHANGE_COMPLETED) {
        return set_virtual_media_error(dispatch, result,
                                       "failed to start storage upload");
    }
    cJSON *response = cJSON_CreateObject();
    if (!response ||
        !cJSON_AddNumberToObject(response, "alreadyUploadedBytes",
                                (double)upload.already_uploaded_bytes) ||
        !cJSON_AddStringToObject(response, "dataChannel",
                                upload.data_channel)) {
        cJSON_Delete(response);
        return false;
    }
    dispatch->result = response;
    return true;
}

/* Per callback: the peer task's context replaces only start (with a refusal
 * while the media worker is busy) and still reads the default state. */
static const jetkvm_storage_download_context_t *storage_download_context(
    const jetkvm_control_context_t *context, bool start)
{
    if (context && (start ? context->storage_download.start != NULL :
                            context->storage_download.get_state != NULL)) {
        return &context->storage_download;
    }
    return &s_default_storage_download;
}

static const char *storage_download_phase_name(
    jetkvm_control_storage_download_phase_t phase)
{
    switch (phase) {
    case JETKVM_CONTROL_STORAGE_DOWNLOAD_DOWNLOADING:
        return "downloading";
    case JETKVM_CONTROL_STORAGE_DOWNLOAD_DONE:
        return "done";
    case JETKVM_CONTROL_STORAGE_DOWNLOAD_ERROR:
        return "error";
    case JETKVM_CONTROL_STORAGE_DOWNLOAD_IDLE:
    default:
        return "idle";
    }
}

static cJSON *storage_download_state_json(
    const jetkvm_control_storage_download_state_t *state)
{
    cJSON *object = cJSON_CreateObject();
    if (!object ||
        !cJSON_AddStringToObject(object, "state",
                                 storage_download_phase_name(state->phase)) ||
        !cJSON_AddStringToObject(object, "filename", state->filename) ||
        !cJSON_AddNumberToObject(object, "bytes", (double)state->bytes) ||
        !cJSON_AddNumberToObject(object, "total", (double)state->total) ||
        !cJSON_AddBoolToObject(object, "reused", state->reused) ||
        !cJSON_AddBoolToObject(object, "videoPaused", state->video_paused) ||
        !cJSON_AddStringToObject(object, "error", state->error)) {
        cJSON_Delete(object);
        return NULL;
    }
    return object;
}

bool jetkvm_control_render_storage_download_event(
    const jetkvm_control_storage_download_state_t *state, char *output,
    size_t capacity, size_t *length)
{
    if (!state || !output || capacity == 0U || !length) {
        return false;
    }
    cJSON *event = cJSON_CreateObject();
    cJSON *params = storage_download_state_json(state);
    bool ok = event && params &&
        cJSON_AddStringToObject(event, "jsonrpc", "2.0") &&
        cJSON_AddStringToObject(event, "method", "storageDownloadState");
    if (ok) {
        cJSON_AddItemToObject(event, "params", params);
        params = NULL;
        ok = cJSON_PrintPreallocated(event, output, (int)capacity, false) != 0;
    }
    cJSON_Delete(params);
    cJSON_Delete(event);
    *length = ok ? strlen(output) : 0U;
    return ok;
}

#if JETKVM_TF_CARD_TEST
/* Bench builds only: {"out":true} takes the TF card out, as a hand that
 * pulls it from the slot, and {"out":false} puts it back. */
bool rpc_test_tf_card_out(const jetkvm_control_context_t *context,
                          const cJSON *params,
                          rpc_dispatch_result_t *dispatch)
{
    (void)context;
    const cJSON *out = cJSON_IsObject(params) ?
        cJSON_GetObjectItemCaseSensitive(params, "out") : NULL;
    if (!cJSON_IsBool(out) || params->child != out || out->next != NULL) {
        return set_virtual_media_error(
            dispatch, JETKVM_VIRTUAL_MEDIA_CHANGE_INVALID,
            "out must be true or false");
    }
    jetkvm_virtual_media_test_card_out(cJSON_IsTrue(out));
    dispatch->result = cJSON_CreateNull();
    return dispatch->result != NULL;
}
#endif

bool rpc_download_to_storage(const jetkvm_control_context_t *context,
                             const cJSON *params,
                             rpc_dispatch_result_t *dispatch)
{
    const cJSON *url = cJSON_IsObject(params) ?
        cJSON_GetObjectItemCaseSensitive(params, "url") : NULL;
    const cJSON *filename = cJSON_IsObject(params) ?
        cJSON_GetObjectItemCaseSensitive(params, "filename") : NULL;
    if (!cJSON_IsString(url) || url->valuestring == NULL ||
        !cJSON_IsString(filename) || filename->valuestring == NULL ||
        !object_has_exactly_two_members(params, url, filename)) {
        return set_virtual_media_error(
            dispatch, JETKVM_VIRTUAL_MEDIA_CHANGE_INVALID,
            "failed to start the download");
    }
    if (refuse_during_update(context, dispatch)) {
        return true;
    }
    const jetkvm_storage_download_context_t *download =
        storage_download_context(context, true);
    if (!download->start) {
        return set_virtual_media_error(
            dispatch, JETKVM_VIRTUAL_MEDIA_CHANGE_UNAVAILABLE,
            "failed to start the download");
    }
    dispatch->error_storage[0] = '\0';
    if (!download->start(url->valuestring, filename->valuestring,
                         dispatch->error_storage,
                         sizeof(dispatch->error_storage), download->ctx)) {
        dispatch->error_code = -32603;
        dispatch->error_message = "Internal error";
        dispatch->error_data = dispatch->error_storage[0] != '\0' ?
            dispatch->error_storage : "failed to start the download";
        return true;
    }
    dispatch->result = cJSON_CreateNull();
    return dispatch->result != NULL;
}

bool rpc_get_storage_download_state(const jetkvm_control_context_t *context,
                                    const cJSON *params,
                                    rpc_dispatch_result_t *dispatch)
{
    if (!require_no_rpc_params(params, dispatch)) {
        return true;
    }
    const jetkvm_storage_download_context_t *download =
        storage_download_context(context, false);
    jetkvm_control_storage_download_state_t state = {0};
    if (download->get_state && !download->get_state(&state, download->ctx)) {
        return set_virtual_media_error(
            dispatch, JETKVM_VIRTUAL_MEDIA_CHANGE_UNAVAILABLE,
            "failed to read the download state");
    }
    dispatch->result = storage_download_state_json(&state);
    return dispatch->result != NULL;
}
