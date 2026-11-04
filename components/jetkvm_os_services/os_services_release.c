/*
 * SPDX-FileCopyrightText: 2026
 * SPDX-License-Identifier: MIT
 */

#include "os_services_release.h"

#include <stdio.h>
#include <string.h>

#include "jetkvm_ota_semver.h"

/* --- Index text ---------------------------------------------------------- */

/* The closing quote of the string that starts at p, or NULL. */
static const char *string_end(const char *p, const char *end)
{
    for (++p; p < end; ++p) {
        if (*p == '\\') {
            ++p;
        } else if (*p == '"') {
            return p;
        }
    }
    return NULL;
}

static const char *skip_space(const char *p, const char *end)
{
    while (p < end && (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r')) {
        ++p;
    }
    return p;
}

/* The '[' of the top-level "releases" member, or NULL. */
static const char *find_releases(const char *p, const char *end)
{
    int depth = 0;
    for (; p < end; ++p) {
        if (*p == '"') {
            const char *close = string_end(p, end);
            if (close == NULL) {
                return NULL;
            }
            const char *next = skip_space(close + 1, end);
            if (depth == 1 && close - p == 9 && memcmp(p, "\"releases", 9) == 0 &&
                next < end && *next == ':') {
                next = skip_space(next + 1, end);
                return next < end && *next == '[' ? next : NULL;
            }
            p = close;
        } else if (*p == '{' || *p == '[') {
            depth++;
        } else if (*p == '}' || *p == ']') {
            depth--;
        }
    }
    return NULL;
}

/* --- One release --------------------------------------------------------- */

static bool hex_digest(const char *text, uint8_t out[32])
{
    if (strlen(text) != 64U) {
        return false;
    }
    for (size_t i = 0U; i < 64U; ++i) {
        const char c = text[i];
        const int nibble = c >= '0' && c <= '9' ? c - '0' :
                           c >= 'a' && c <= 'f' ? c - 'a' + 10 :
                           c >= 'A' && c <= 'F' ? c - 'A' + 10 : -1;
        if (nibble < 0) {
            return false;
        }
        out[i / 2U] = (uint8_t)((i % 2U) ? (out[i / 2U] | nibble) : (nibble << 4));
    }
    return true;
}

/* A relative path in the bucket: no scheme, query or parent steps. */
static bool path_valid(const char *path)
{
    if (path[0] == '\0' || path[0] == '/' || strlen(path) > OSVC_RELEASE_PATH_MAX ||
        strstr(path, "..") != NULL) {
        return false;
    }
    return strspn(path, "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ"
                        "0123456789._-+~/") == strlen(path);
}

static void read_package(const cJSON *item, osvc_release_t *release)
{
    const cJSON *path = cJSON_GetObjectItemCaseSensitive(item, "path");
    const cJSON *size = cJSON_GetObjectItemCaseSensitive(item, "size");
    const cJSON *sha = cJSON_GetObjectItemCaseSensitive(item, "sha256");
    if (release->package_count == OSVC_RELEASE_PACKAGES_MAX ||
        strlen(item->string) > OSVC_UPDATE_PACKAGE_MAX ||
        !cJSON_IsString(path) || !path_valid(path->valuestring) ||
        !cJSON_IsNumber(size) || size->valuedouble < 1.0 ||
        size->valuedouble > (double)OSVC_RELEASE_PACKAGE_MAX_BYTES ||
        (double)(uint64_t)size->valuedouble != size->valuedouble ||
        !cJSON_IsString(sha)) {
        return;
    }
    osvc_release_package_t *package = &release->packages[release->package_count];
    if (!hex_digest(sha->valuestring, package->sha256)) {
        return;
    }
    snprintf(package->name, sizeof(package->name), "%s", item->string);
    snprintf(package->path, sizeof(package->path), "%s", path->valuestring);
    package->size = (uint64_t)size->valuedouble;
    release->package_count++;
}

/* The version of a release in range for its index, without a leading "v";
 * NULL otherwise. */
static const char *usable_version(const cJSON *release, bool is_signed)
{
    const cJSON *version = cJSON_GetObjectItemCaseSensitive(release, "version");
    jetkvm_ota_semver_t parsed;
    if (!cJSON_IsString(version) ||
        strlen(version->valuestring) > OSVC_UPDATE_VERSION_MAX ||
        !jetkvm_ota_semver_parse(version->valuestring, &parsed) ||
        parsed.core[0] != OSVC_RELEASE_MAJOR ||
        parsed.core[1] != OSVC_RELEASE_MINOR ||
        (parsed.prerelease_len == 0U) != is_signed) {
        return NULL;
    }
    const char *text = version->valuestring;
    return *text == 'v' || *text == 'V' ? text + 1 : text;
}

/* Replaces *newest with the release in text when it is usable and newer. */
static bool read_release(const char *text, size_t length, bool is_signed,
                         osvc_release_t *newest)
{
    cJSON *release = cJSON_ParseWithLength(text, length);
    if (release == NULL) {
        return false;
    }
    const char *version = usable_version(release, is_signed);
    if (version != NULL && (newest->version[0] == '\0' ||
                            osvc_release_newer(version, newest->version))) {
        memset(newest, 0, sizeof(*newest));
        snprintf(newest->version, sizeof(newest->version), "%s", version);
        newest->is_signed = is_signed;
        const cJSON *item = NULL;
        const cJSON *packages =
            cJSON_GetObjectItemCaseSensitive(release, "packages");
        cJSON_ArrayForEach(item, (cJSON_IsObject(packages) ? packages : NULL)) {
            read_package(item, newest);
        }
    }
    cJSON_Delete(release);
    return true;
}

const char *osvc_release_read_index(const char *text, size_t length,
                                    bool is_signed, osvc_release_t *newest)
{
    memset(newest, 0, sizeof(*newest));
    const char *end = text + length;
    const char *p = find_releases(text, end);
    if (p == NULL) {
        return "the index has no releases list";
    }
    /* Each element of the array, by its span at depth 0. */
    const char *start = ++p;
    for (int depth = 0; p < end; ++p) {
        if (*p == '"') {
            if ((p = string_end(p, end)) == NULL) {
                break;
            }
        } else if (*p == '{' || *p == '[') {
            depth++;
        } else if (depth == 0 && (*p == ',' || *p == ']')) {
            /* "[]" has no element; anything else between commas must be
             * one. */
            if ((*p == ',' || skip_space(start, p) != p) &&
                !read_release(start, (size_t)(p - start), is_signed, newest)) {
                return "the index has a malformed release";
            }
            if (*p == ']') {
                return NULL;
            }
            start = p + 1;
        } else if (*p == '}' || *p == ']') {
            depth--;
        }
    }
    return "the index is truncated";
}

/* --- Selection ----------------------------------------------------------- */

bool osvc_release_newer(const char *version, const char *than)
{
    jetkvm_ota_semver_t a;
    jetkvm_ota_semver_t b;
    return jetkvm_ota_semver_parse(version, &a) &&
           jetkvm_ota_semver_parse(than, &b) &&
           jetkvm_ota_semver_compare(&a, &b) > 0;
}

const osvc_release_t *osvc_release_newest(const osvc_release_t *stable,
                                          const osvc_release_t *dev,
                                          bool include_dev)
{
    const osvc_release_t *newest = stable->version[0] != '\0' ? stable : NULL;
    if (include_dev && dev->version[0] != '\0' &&
        (newest == NULL || osvc_release_newer(dev->version, newest->version))) {
        newest = dev;
    }
    return newest;
}

const osvc_release_package_t *osvc_release_package(
    const osvc_release_t *release, const char *name)
{
    for (size_t i = 0U; release != NULL && i < release->package_count; ++i) {
        if (name[0] != '\0' && strcmp(release->packages[i].name, name) == 0) {
            return &release->packages[i];
        }
    }
    return NULL;
}

const osvc_release_package_t *osvc_release_target(
    const osvc_release_t *newest, const osvc_update_sender_t *sender)
{
    return newest != NULL &&
                   osvc_release_newer(newest->version, sender->version) ?
               osvc_release_package(newest, sender->package) :
               NULL;
}

/* --- Browser state ------------------------------------------------------- */

static const char *const PHASES[] = {
    [OSVC_UPDATE_PHASE_DOWNLOADING] = "downloading",
    [OSVC_UPDATE_PHASE_SENDING] = "sending",
    [OSVC_UPDATE_PHASE_INSTALLING] = "installing",
    [OSVC_UPDATE_PHASE_DONE] = "done",
    [OSVC_UPDATE_PHASE_FAILED] = "failed",
};

bool osvc_update_render(cJSON *object, const osvc_release_t *newest,
                        const osvc_update_sender_t *sender,
                        const osvc_update_progress_t *progress, bool automatic)
{
    const bool finished = progress->phase == OSVC_UPDATE_PHASE_DONE ||
                          progress->phase == OSVC_UPDATE_PHASE_FAILED;
    /* A finished update gives way to a newer release; a running one does
     * not. */
    const bool show_progress =
        progress->phase != OSVC_UPDATE_PHASE_NONE &&
        (!finished || newest == NULL ||
         strcmp(newest->version, progress->version) == 0);
    const char *version = show_progress  ? progress->version :
                          newest != NULL ? newest->version : "";
    if (version[0] == '\0') {
        return true;
    }
    cJSON *state = cJSON_AddObjectToObject(object, "guestUpdate");
    bool ok =
        state != NULL && cJSON_AddStringToObject(state, "version", version) &&
        cJSON_AddBoolToObject(state, "available",
                              newest != NULL &&
                                  osvc_release_newer(newest->version,
                                                     sender->version)) &&
        cJSON_AddBoolToObject(state, "installable",
                              osvc_release_package(newest, sender->package) !=
                                  NULL) &&
        cJSON_AddBoolToObject(state, "automatic", automatic);
    if (ok && show_progress) {
        ok = cJSON_AddStringToObject(state, "phase", PHASES[progress->phase]) &&
             cJSON_AddNumberToObject(state, "sent", (double)progress->sent) &&
             cJSON_AddNumberToObject(state, "size", (double)progress->size) &&
             (progress->error[0] == '\0' ||
              cJSON_AddStringToObject(state, "error", progress->error));
    }
    return ok;
}
