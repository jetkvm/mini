/*
 * SPDX-FileCopyrightText: 2026
 * SPDX-License-Identifier: MIT
 */

#include "jetkvm_ota.h"
#include "jetkvm_ota_semver.h"

#include <stdio.h>
#include <string.h>

#include "cJSON.h"

static bool copy_json_string(const cJSON *item, char *output, size_t capacity,
                             bool allow_empty)
{
    if (!cJSON_IsString(item) || !item->valuestring) {
        return false;
    }
    size_t length = strlen(item->valuestring);
    if ((!allow_empty && length == 0) || length >= capacity) {
        return false;
    }
    memcpy(output, item->valuestring, length + 1);
    return true;
}

static void clear_metadata(jetkvm_ota_metadata_t *metadata)
{
    memset(metadata, 0, sizeof(*metadata));
}

bool jetkvm_ota_parse_metadata(const char *json, size_t length,
                               jetkvm_ota_metadata_t *metadata)
{
    if (!json || !metadata) {
        return false;
    }
    clear_metadata(metadata);
    cJSON *root = cJSON_ParseWithLength(json, length);
    if (!cJSON_IsObject(root)) {
        cJSON_Delete(root);
        return false;
    }
    /* Every field is optional: an absent/empty component is simply not
     * offered.  A present field must still be a well-formed string so a
     * truncated URL can never reach the downloader. */
    const struct {
        const char *key;
        char *output;
        size_t capacity;
        bool allow_empty;
    } fields[] = {
        { "appVersion", metadata->app_version,
          JETKVM_OTA_VERSION_CAPACITY, true },
        { "appUrl", metadata->app_url, JETKVM_OTA_URL_CAPACITY, true },
        { "appHash", metadata->app_hash, JETKVM_OTA_HASH_CAPACITY, true },
        { "systemVersion", metadata->system_version,
          JETKVM_OTA_VERSION_CAPACITY, true },
        { "systemUrl", metadata->system_url, JETKVM_OTA_URL_CAPACITY, true },
        { "systemHash", metadata->system_hash,
          JETKVM_OTA_HASH_CAPACITY, true },
        { "systemSigUrl", metadata->system_sig_url,
          JETKVM_OTA_URL_CAPACITY, true },
    };
    bool ok = true;
    for (size_t i = 0; i < sizeof(fields) / sizeof(fields[0]); i++) {
        const cJSON *item = cJSON_GetObjectItemCaseSensitive(root, fields[i].key);
        if (!item) {
            continue;
        }
        if (!copy_json_string(item, fields[i].output, fields[i].capacity,
                              fields[i].allow_empty)) {
            ok = false;
            break;
        }
    }
    /* A component is only actionable when version, URL and hash all agree
     * to be present; a partial offer is rejected as malformed. */
    if (ok) {
        bool app_partial =
            (metadata->app_version[0] != '\0') ||
            (metadata->app_url[0] != '\0') || (metadata->app_hash[0] != '\0');
        bool app_complete =
            (metadata->app_version[0] != '\0') &&
            (metadata->app_url[0] != '\0') && (metadata->app_hash[0] != '\0');
        bool system_partial =
            (metadata->system_version[0] != '\0') ||
            (metadata->system_url[0] != '\0') ||
            (metadata->system_hash[0] != '\0');
        bool system_complete =
            (metadata->system_version[0] != '\0') &&
            (metadata->system_url[0] != '\0') &&
            (metadata->system_hash[0] != '\0');
        ok = (!app_partial || app_complete) &&
             (!system_partial || system_complete);
    }
    cJSON_Delete(root);
    if (!ok) {
        clear_metadata(metadata);
    }
    return ok;
}

bool jetkvm_ota_update_available(const char *local_version,
                                 const char *remote_version,
                                 const char *constraint)
{
    if (!local_version || !remote_version || remote_version[0] == '\0') {
        return false;
    }
    if (constraint != NULL && constraint[0] != '\0') {
        /* Pinned installs mirror Go's custom-version path: any difference
         * counts, downgrades included. */
        return strcmp(local_version, remote_version) != 0;
    }
    jetkvm_ota_semver_t local;
    jetkvm_ota_semver_t remote;
    if (!jetkvm_ota_semver_parse(local_version, &local) ||
        !jetkvm_ota_semver_parse(remote_version, &remote)) {
        /* Go fails the whole check on a parse error; refusing the offer
         * is the fail-safe equivalent here. */
        return false;
    }
    return jetkvm_ota_semver_compare(&remote, &local) > 0;
}

static bool add_string(cJSON *object, const char *key, const char *value)
{
    return cJSON_AddStringToObject(object, key, value ? value : "") != NULL;
}

bool jetkvm_ota_render_status_json(const jetkvm_ota_update_status_t *status,
                                   char *output, size_t capacity,
                                   size_t *out_size)
{
    if (!status || !output || !out_size) {
        return false;
    }
    bool ok = false;
    cJSON *root = cJSON_CreateObject();
    cJSON *local = NULL;
    cJSON *remote = NULL;
    if (!root) {
        return false;
    }
    local = cJSON_CreateObject();
    remote = cJSON_CreateObject();
    if (!local || !remote ||
        !cJSON_AddItemToObject(root, "local", local) ||
        !cJSON_AddItemToObject(root, "remote", remote) ||
        !add_string(local, "appVersion", status->local_app_version) ||
        !add_string(local, "systemVersion", status->local_system_version) ||
        !add_string(remote, "appVersion", status->remote.app_version) ||
        !add_string(remote, "appUrl", status->remote.app_url) ||
        !add_string(remote, "appHash", status->remote.app_hash) ||
        !add_string(remote, "systemVersion", status->remote.system_version) ||
        !add_string(remote, "systemUrl", status->remote.system_url) ||
        !add_string(remote, "systemHash", status->remote.system_hash) ||
        !cJSON_AddBoolToObject(root, "systemUpdateAvailable",
                               status->system_update_available) ||
        !cJSON_AddBoolToObject(root, "appUpdateAvailable",
                               status->app_update_available) ||
        !cJSON_AddBoolToObject(root, "willDisableAutoUpdate",
                               status->will_disable_auto_update)) {
        goto done;
    }
    if (status->error[0] != '\0' &&
        !add_string(root, "error", status->error)) {
        goto done;
    }
    {
        char *rendered = cJSON_PrintUnformatted(root);
        if (!rendered) {
            goto done;
        }
        size_t length = strlen(rendered);
        if (length + 1 <= capacity) {
            memcpy(output, rendered, length + 1);
            *out_size = length;
            ok = true;
        }
        cJSON_free(rendered);
    }
done:
    cJSON_Delete(root);
    return ok;
}

/* Go sends RFC3339 timestamps and the web UI derives the install stage from
 * null-vs-non-null alone, so a stage flag maps to a timestamp string while
 * its absence maps to JSON null. */
static bool add_stage_fields(cJSON *object, const char *prefix,
                             float download_progress, bool download_finished,
                             float verification_progress, bool verified,
                             float update_progress, bool updated,
                             const char *timestamp)
{
    char key[48];
    (void)snprintf(key, sizeof(key), "%sDownloadProgress", prefix);
    if (!cJSON_AddNumberToObject(object, key, download_progress)) {
        return false;
    }
    (void)snprintf(key, sizeof(key), "%sDownloadFinishedAt", prefix);
    if (download_finished) {
        if (!add_string(object, key, timestamp)) {
            return false;
        }
    } else if (!cJSON_AddNullToObject(object, key)) {
        return false;
    }
    (void)snprintf(key, sizeof(key), "%sVerificationProgress", prefix);
    if (!cJSON_AddNumberToObject(object, key, verification_progress)) {
        return false;
    }
    (void)snprintf(key, sizeof(key), "%sVerifiedAt", prefix);
    if (verified) {
        if (!add_string(object, key, timestamp)) {
            return false;
        }
    } else if (!cJSON_AddNullToObject(object, key)) {
        return false;
    }
    (void)snprintf(key, sizeof(key), "%sUpdateProgress", prefix);
    if (!cJSON_AddNumberToObject(object, key, update_progress)) {
        return false;
    }
    (void)snprintf(key, sizeof(key), "%sUpdatedAt", prefix);
    if (updated) {
        return add_string(object, key, timestamp);
    }
    return cJSON_AddNullToObject(object, key) != NULL;
}

bool jetkvm_ota_render_state_event(const jetkvm_ota_state_t *state,
                                   const char *timestamp,
                                   char *output, size_t capacity,
                                   size_t *out_size)
{
    if (!state || !timestamp || !output || !out_size) {
        return false;
    }
    bool ok = false;
    cJSON *root = cJSON_CreateObject();
    cJSON *params = NULL;
    if (!root) {
        return false;
    }
    params = cJSON_CreateObject();
    if (!params || !add_string(root, "jsonrpc", "2.0") ||
        !add_string(root, "method", "otaState") ||
        !cJSON_AddItemToObject(root, "params", params) ||
        !cJSON_AddBoolToObject(params, "updating", state->updating)) {
        goto done;
    }
    if (state->metadata_fetched) {
        if (!add_string(params, "metadataFetchedAt", timestamp)) {
            goto done;
        }
    } else if (!cJSON_AddNullToObject(params, "metadataFetchedAt")) {
        goto done;
    }
    if (!cJSON_AddBoolToObject(params, "appUpdatePending",
                               state->app_update_pending) ||
        !add_stage_fields(params, "app",
                          state->app_download_progress,
                          state->app_download_finished,
                          state->app_verification_progress,
                          state->app_verified,
                          state->app_update_progress,
                          state->app_updated, timestamp) ||
        !cJSON_AddBoolToObject(params, "systemUpdatePending",
                               state->system_update_pending) ||
        !add_stage_fields(params, "system",
                          state->system_download_progress,
                          state->system_download_finished,
                          state->system_verification_progress,
                          state->system_verified,
                          state->system_update_progress,
                          state->system_updated, timestamp)) {
        goto done;
    }
    if (state->error[0] != '\0') {
        if (!add_string(params, "error", state->error)) {
            goto done;
        }
    } else if (!cJSON_AddNullToObject(params, "error")) {
        goto done;
    }
    {
        char *rendered = cJSON_PrintUnformatted(root);
        if (!rendered) {
            goto done;
        }
        size_t length = strlen(rendered);
        if (length + 1 <= capacity) {
            memcpy(output, rendered, length + 1);
            *out_size = length;
            ok = true;
        }
        cJSON_free(rendered);
    }
done:
    cJSON_Delete(root);
    return ok;
}

bool jetkvm_ota_validate_api_url(const char *url)
{
    if (!url) {
        return false;
    }
    size_t length = strlen(url);
    if (length == 0 || length >= JETKVM_OTA_API_URL_CAPACITY) {
        return false;
    }
    const char *host = NULL;
    if (strncmp(url, "http://", 7) == 0) {
        host = url + 7;
    } else if (strncmp(url, "https://", 8) == 0) {
        host = url + 8;
    } else {
        return false;
    }
    if (*host == '\0' || *host == '/' || *host == '?') {
        return false;
    }
    /* No query/fragment/userinfo in the base URL: the releases path and
     * query string are appended by jetkvm_ota_build_releases_url(). */
    for (const char *p = host; *p; p++) {
        if (*p == '?' || *p == '#' || *p == '@' || *p == ' ') {
            return false;
        }
    }
    return true;
}

bool jetkvm_ota_image_desc_matches(const jetkvm_ota_image_desc_t *desc,
                                   const char *sku)
{
    if (!desc || !sku || desc->magic != JETKVM_OTA_IMAGE_DESC_MAGIC ||
        memchr(desc->sku, '\0', sizeof(desc->sku)) == NULL) {
        return false;
    }
    return strcmp(desc->sku, sku) == 0;
}

static bool append_query_param(char *output, size_t capacity, size_t *length,
                               const char *key, const char *value, bool first)
{
    int written = snprintf(output + *length, capacity - *length, "%s%s=%s",
                           first ? "?" : "&", key, value);
    if (written < 0 || (size_t)written >= capacity - *length) {
        return false;
    }
    *length += (size_t)written;
    return true;
}

bool jetkvm_ota_build_releases_url(char *output, size_t capacity,
                                   const char *api_url, const char *device_id,
                                   const char *sku, bool include_prerelease,
                                   const char *app_constraint,
                                   const char *system_constraint)
{
    if (!output || !jetkvm_ota_validate_api_url(api_url) || !device_id ||
        device_id[0] == '\0' || !sku || sku[0] == '\0') {
        return false;
    }
    size_t base_length = strlen(api_url);
    if (base_length > 0 && api_url[base_length - 1] == '/') {
        base_length--; /* collapse one trailing slash before appending */
    }
    size_t length = 0;
    int written = snprintf(output, capacity, "%.*s/releases", (int)base_length,
                           api_url);
    if (written < 0 || (size_t)written >= capacity) {
        return false;
    }
    length = (size_t)written;
    if (!append_query_param(output, capacity, &length, "deviceId", device_id,
                            true)) {
        return false;
    }
    if (!append_query_param(output, capacity, &length, "prerelease",
                            include_prerelease ? "true" : "false", false)) {
        return false;
    }
    if (!append_query_param(output, capacity, &length, "sku", sku, false)) {
        return false;
    }
    if (app_constraint && app_constraint[0] != '\0' &&
        !append_query_param(output, capacity, &length, "appVersion",
                            app_constraint, false)) {
        return false;
    }
    if (system_constraint && system_constraint[0] != '\0' &&
        !append_query_param(output, capacity, &length, "systemVersion",
                            system_constraint, false)) {
        return false;
    }
    return true;
}
