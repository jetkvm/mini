/*
 * SPDX-FileCopyrightText: 2026
 * SPDX-License-Identifier: MIT
 */

/* OS Services (IP-KVM OS Services) RPCs. The OS Services component owns the
 * settings and the sender connection; these handlers only carry JSON to and
 * from its callbacks. */
#include "control_protocol_internal.h"

#include <stdio.h>
#include <string.h>

#define OS_SERVICES_UNAVAILABLE "OS Services unavailable"
#define NO_GUEST_CLIPBOARD "no OS Services clipboard is connected"
#define NO_GUEST_FILE "no OS Services file transfer is connected"
#define INVALID_DROP \
    "drop.files must be an array of {name: string, size: whole number}"
/* A file's size travels as a JSON number: whole and exact up to 2^53. */
#define GUEST_FILE_SIZE_MAX 9007199254740992.0

/* The RPC worker builds a clipboard reply in a 128 KiB buffer
 * (JETKVM_MEMORY_RPC_SCRATCH_BYTES). The text may take 120 KiB of it once
 * escaped; the envelope and the other fields need far less than the rest. */
#define GUEST_CLIPBOARD_REPLY_TEXT_BUDGET (120U * 1024U)

jetkvm_os_services_context_t s_default_os_services;
static jetkvm_guest_clipboard_context_t s_default_guest_clipboard;
static jetkvm_guest_file_context_t s_default_guest_file;

void jetkvm_control_set_default_os_services_context(
    const jetkvm_os_services_context_t *context)
{
    s_default_os_services = context ? *context :
        (jetkvm_os_services_context_t){ 0 };
}

void jetkvm_control_set_default_guest_clipboard_context(
    const jetkvm_guest_clipboard_context_t *context)
{
    s_default_guest_clipboard = context ? *context :
        (jetkvm_guest_clipboard_context_t){ 0 };
}

jetkvm_guest_clipboard_context_t jetkvm_control_default_guest_clipboard_context(void)
{
    return s_default_guest_clipboard;
}

void jetkvm_control_set_default_guest_file_context(
    const jetkvm_guest_file_context_t *context)
{
    s_default_guest_file = context ? *context :
        (jetkvm_guest_file_context_t){ 0 };
}

jetkvm_guest_file_context_t jetkvm_control_default_guest_file_context(void)
{
    return s_default_guest_file;
}

/* The data is copied, and cut to error_storage's size: a callback's reason
 * need only outlive the call. */
static bool internal_error(rpc_dispatch_result_t *dispatch, const char *data)
{
    if (data != dispatch->error_storage) {
        (void)snprintf(dispatch->error_storage,
                       sizeof(dispatch->error_storage), "%s", data);
    }
    dispatch->error_code = -32603;
    dispatch->error_message = "Internal error";
    dispatch->error_data = dispatch->error_storage;
    return true;
}

static bool invalid_params(rpc_dispatch_result_t *dispatch, const char *data)
{
    dispatch->error_code = -32602;
    dispatch->error_message = "Invalid params";
    dispatch->error_data = data;
    return true;
}

bool rpc_get_os_services_settings(const jetkvm_control_context_t *context,
                                   const cJSON *params,
                                   rpc_dispatch_result_t *dispatch)
{
    (void)context;
    (void)params;
    const jetkvm_os_services_context_t *os = &s_default_os_services;
    if (!os->get_settings) {
        return internal_error(dispatch, OS_SERVICES_UNAVAILABLE);
    }
    cJSON *settings = cJSON_CreateObject();
    if (!settings) {
        return false;
    }
    if (!os->get_settings(settings, os->ctx)) {
        cJSON_Delete(settings);
        return internal_error(dispatch, OS_SERVICES_UNAVAILABLE);
    }
    dispatch->result = settings;
    return true;
}

/* Every settings field is optional; one the request leaves out keeps its
 * value. The OS Services component validates the fields it knows. */
bool rpc_set_os_services_settings(const jetkvm_control_context_t *context,
                                   const cJSON *params,
                                   rpc_dispatch_result_t *dispatch)
{
    (void)context;
    const cJSON *settings = rpc_param(params, "settings");
    if (!cJSON_IsObject(settings)) {
        return invalid_params(dispatch, "settings must be an object");
    }
    const jetkvm_os_services_context_t *os = &s_default_os_services;
    if (!os->set_settings) {
        return internal_error(dispatch, OS_SERVICES_UNAVAILABLE);
    }
    cJSON *applied = cJSON_CreateObject();
    if (!applied) {
        return false;
    }
    const char *reason = os->set_settings(settings, applied, os->ctx);
    if (reason) {
        cJSON_Delete(applied);
        return internal_error(dispatch, reason);
    }
    dispatch->result = applied;
    return true;
}

bool rpc_guest_update_start(const jetkvm_control_context_t *context,
                            const cJSON *params,
                            rpc_dispatch_result_t *dispatch)
{
    (void)context;
    (void)params;
    const jetkvm_os_services_context_t *os = &s_default_os_services;
    const char *reason =
        os->update_start ? os->update_start(os->ctx) : OS_SERVICES_UNAVAILABLE;
    if (reason) {
        return internal_error(dispatch, reason);
    }
    dispatch->result = cJSON_CreateNull();
    return dispatch->result != NULL;
}

/* Strict UTF-8, as Go's utf8.Valid: no overlong forms, no surrogates,
 * nothing above U+10FFFF. */
static bool valid_utf8(const unsigned char *text, size_t length)
{
    size_t i = 0;
    while (i < length) {
        unsigned char lead = text[i];
        if (lead < 0x80U) {
            ++i;
            continue;
        }
        size_t continuation;
        uint32_t code_point;
        uint32_t minimum;
        if ((lead & 0xE0U) == 0xC0U) {
            continuation = 1U;
            code_point = lead & 0x1FU;
            minimum = 0x80U;
        } else if ((lead & 0xF0U) == 0xE0U) {
            continuation = 2U;
            code_point = lead & 0x0FU;
            minimum = 0x800U;
        } else if ((lead & 0xF8U) == 0xF0U) {
            continuation = 3U;
            code_point = lead & 0x07U;
            minimum = 0x10000U;
        } else {
            return false;
        }
        if (length - i <= continuation) {
            return false;
        }
        for (size_t k = 1U; k <= continuation; ++k) {
            if ((text[i + k] & 0xC0U) != 0x80U) {
                return false;
            }
            code_point = (code_point << 6) | (text[i + k] & 0x3FU);
        }
        if (code_point < minimum || code_point > 0x10FFFFU ||
            (code_point >= 0xD800U && code_point <= 0xDFFFU)) {
            return false;
        }
        i += continuation + 1U;
    }
    return true;
}

/* The bytes cJSON prints for one byte of a string (print_string_ptr). */
static size_t json_escaped_size(unsigned char byte)
{
    switch (byte) {
    case '"':
    case '\\':
    case '\b':
    case '\f':
    case '\n':
    case '\r':
    case '\t':
        return 2U;
    default:
        return byte < 0x20U ? 6U : 1U;
    }
}

/* The length of the longest prefix of text that prints within the reply
 * budget. A cut backs up to the start of the character it would split.
 * cJSON ends a string at NUL, so the text does too. */
static size_t guest_clipboard_text_fit(const char *text, size_t length,
                                       bool *cut)
{
    const unsigned char *bytes = (const unsigned char *)text;
    size_t printed = 0;
    size_t end = 0;
    while (end < length && bytes[end] != '\0') {
        size_t next = printed + json_escaped_size(bytes[end]);
        if (next > GUEST_CLIPBOARD_REPLY_TEXT_BUDGET) {
            break;
        }
        printed = next;
        ++end;
    }
    if (end < length) {
        *cut = true;
        while (end > 0U && (bytes[end] & 0xC0U) == 0x80U) {
            --end;
        }
    }
    return end;
}

/* The worker's per-request context carries the clipboard: the peer task
 * must not wait for the sender. */
bool rpc_guest_clipboard_get(const jetkvm_control_context_t *context,
                             const cJSON *params,
                             rpc_dispatch_result_t *dispatch)
{
    (void)params;
    const jetkvm_guest_clipboard_context_t *clipboard = &context->guest_clipboard;
    if (!clipboard->get) {
        return internal_error(dispatch, NO_GUEST_CLIPBOARD);
    }
    const size_t capacity = JETKVM_GUEST_CLIPBOARD_MAX_TEXT + 1U;
    char *text = cJSON_malloc(capacity);
    if (!text) {
        return internal_error(dispatch, "out of memory");
    }
    size_t length = 0;
    bool truncated = false;
    const char *status = NULL;
    const char *reason = clipboard->get(text, capacity, &length, &truncated,
                                        &status, clipboard->ctx);
    if (reason) {
        cJSON_free(text);
        return internal_error(dispatch, reason);
    }
    if (!status) {
        status = "failed";
    }
    if (strcmp(status, "ok") != 0) {
        length = 0;
    } else if (length > JETKVM_GUEST_CLIPBOARD_MAX_TEXT) {
        length = JETKVM_GUEST_CLIPBOARD_MAX_TEXT;
        truncated = true;
    }
    length = guest_clipboard_text_fit(text, length, &truncated);
    text[length] = '\0';
    cJSON *result = cJSON_CreateObject();
    bool built = result && cJSON_AddStringToObject(result, "status", status) &&
        cJSON_AddStringToObject(result, "text", text) &&
        cJSON_AddBoolToObject(result, "truncated", truncated);
    cJSON_free(text);
    if (!built) {
        cJSON_Delete(result);
        return false;
    }
    dispatch->result = result;
    return true;
}

bool rpc_guest_clipboard_set(const jetkvm_control_context_t *context,
                             const cJSON *params,
                             rpc_dispatch_result_t *dispatch)
{
    const cJSON *text = rpc_param(params, "text");
    if (!cJSON_IsString(text) || !text->valuestring) {
        return invalid_params(dispatch, "text must be a string");
    }
    size_t length = strlen(text->valuestring);
    if (length > JETKVM_GUEST_CLIPBOARD_MAX_TEXT) {
        (void)snprintf(dispatch->error_storage,
                       sizeof(dispatch->error_storage),
                       "clipboard text exceeds %u bytes",
                       (unsigned)JETKVM_GUEST_CLIPBOARD_MAX_TEXT);
        return internal_error(dispatch, dispatch->error_storage);
    }
    if (!valid_utf8((const unsigned char *)text->valuestring, length)) {
        return internal_error(dispatch, "clipboard text is not valid UTF-8");
    }
    const jetkvm_guest_clipboard_context_t *clipboard = &context->guest_clipboard;
    if (!clipboard->set) {
        return internal_error(dispatch, NO_GUEST_CLIPBOARD);
    }
    const char *status = NULL;
    const char *reason = clipboard->set(text->valuestring, length, &status,
                                        clipboard->ctx);
    if (reason) {
        return internal_error(dispatch, reason);
    }
    cJSON *result = cJSON_CreateObject();
    if (!result || !cJSON_AddStringToObject(result, "status",
                                            status ? status : "failed")) {
        cJSON_Delete(result);
        return false;
    }
    dispatch->result = result;
    return true;
}

static bool guest_file_valid(const cJSON *file)
{
    const cJSON *name = cJSON_GetObjectItemCaseSensitive(file, "name");
    const cJSON *size = cJSON_GetObjectItemCaseSensitive(file, "size");
    if (!cJSON_IsObject(file) || !cJSON_IsString(name) || !name->valuestring ||
        !cJSON_IsNumber(size)) {
        return false;
    }
    const double value = size->valuedouble;
    return value >= 0.0 && value <= GUEST_FILE_SIZE_MAX &&
        (double)(uint64_t)value == value;
}

/* {"drop":{"files":[{"name","size"}, ...]}}: the drop travels as one object,
 * not one file at a time. The OS Services component checks the names and the
 * count, as the sender's protocol bounds them. */
bool rpc_guest_file_drop(const jetkvm_control_context_t *context,
                         const cJSON *params, rpc_dispatch_result_t *dispatch)
{
    const cJSON *drop = rpc_param(params, "drop");
    if (!cJSON_IsObject(drop)) {
        return invalid_params(dispatch, "drop must be an object");
    }
    const cJSON *files = cJSON_GetObjectItemCaseSensitive(drop, "files");
    if (!cJSON_IsArray(files)) {
        return invalid_params(dispatch, INVALID_DROP);
    }
    const cJSON *file = NULL;
    cJSON_ArrayForEach(file, files) {
        if (!guest_file_valid(file)) {
            return invalid_params(dispatch, INVALID_DROP);
        }
    }
    const jetkvm_guest_file_context_t *guest_file = &context->guest_file;
    if (!guest_file->drop) {
        return internal_error(dispatch, NO_GUEST_FILE);
    }
    cJSON *result = cJSON_CreateObject();
    if (!result) {
        return false;
    }
    const char *reason = guest_file->drop(files, result, guest_file->ctx);
    if (reason) {
        cJSON_Delete(result);
        return internal_error(dispatch, reason);
    }
    dispatch->result = result;
    return true;
}

bool jetkvm_control_render_guest_video_fields(char *output, size_t capacity,
                                             size_t *length)
{
    if (!output || !length || capacity == 0U) {
        return false;
    }
    *length = 0;
    output[0] = '\0';
    const jetkvm_os_services_context_t *os = &s_default_os_services;
    if (!os->add_video_state) {
        return true;
    }
    cJSON *fields = cJSON_CreateObject();
    char *rendered = fields && os->add_video_state(fields, os->ctx) ?
        cJSON_PrintUnformatted(fields) : NULL;
    cJSON_Delete(fields);
    if (!rendered) {
        return false;
    }
    /* "{members}" becomes ",members": one byte shorter, plus the NUL. */
    size_t size = strlen(rendered);
    bool fits = size <= 2U || size <= capacity;
    if (size > 2U && fits) {
        output[0] = ',';
        memcpy(output + 1, rendered + 1, size - 2U);
        output[size - 1U] = '\0';
        *length = size - 1U;
    }
    cJSON_free(rendered);
    return fits;
}
