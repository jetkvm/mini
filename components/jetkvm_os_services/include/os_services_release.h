/*
 * SPDX-FileCopyrightText: 2026
 * SPDX-License-Identifier: MIT
 */

/*
 * OS Services releases as the update relay sees them. The release bucket
 * holds releases.json, signed with the JetKVM release key (releases.json.sig),
 * and releases-dev.json, unsigned, for the dev channel. Each lists releases
 * with a package per sender platform:
 *
 *   {"releases":[{"version":"0.4.2","packages":{"linux-x86_64-appimage":
 *     {"path":"0.4.2/...AppImage","size":123,"sha256":"<64 hex>"}}}]}
 *
 * This firmware installs 0.4.x: releases from the signed index and
 * prereleases from the dev index. Only the newest of each index is kept.
 * Also renders the browser's guestUpdate object. Pure C and cJSON.
 */

#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "cJSON.h"
#include "os_services_session.h"

#ifdef __cplusplus
extern "C" {
#endif

/* The range of releases this firmware installs: 0.4.x. A new minor version
 * can change what the sender needs from the KVM. */
#define OSVC_RELEASE_MAJOR 0U
#define OSVC_RELEASE_MINOR 4U
#define OSVC_RELEASE_PACKAGES_MAX 8U
#define OSVC_RELEASE_PATH_MAX 128U
/* The largest package the KVM relays. */
#define OSVC_RELEASE_PACKAGE_MAX_BYTES (512U * 1024U * 1024U)
/* The longest guestUpdate member the browser's videoInputState event makes
 * room for (jetkvm_os_services.c), with a 64-byte version and error. */
#define OSVC_UPDATE_STATE_JSON_MAX 280U

typedef struct {
    char name[OSVC_UPDATE_PACKAGE_MAX + 1U];
    /* Relative to the index. */
    char path[OSVC_RELEASE_PATH_MAX + 1U];
    uint64_t size;
    uint8_t sha256[32];
} osvc_release_package_t;

typedef struct {
    /* Empty when the index lists no release in range. */
    char version[OSVC_UPDATE_VERSION_MAX + 1U];
    /* From the signed index: its packages carry signatures. */
    bool is_signed;
    size_t package_count;
    osvc_release_package_t packages[OSVC_RELEASE_PACKAGES_MAX];
} osvc_release_t;

/*
 * Reads an index and keeps its newest release in range in *newest (version
 * empty when there is none). A release or package that is incomplete or
 * malformed is skipped. Each release is parsed on its own, so a large index
 * never becomes one large cJSON tree. Returns NULL, or why the index is
 * unreadable (*newest is then unspecified).
 */
const char *osvc_release_read_index(const char *text, size_t length,
                                    bool is_signed, osvc_release_t *newest);

/* The newer of the two, the dev release only when include_dev; NULL when
 * neither has one. */
const osvc_release_t *osvc_release_newest(const osvc_release_t *stable,
                                          const osvc_release_t *dev,
                                          bool include_dev);

/* The release's package for a sender platform, or NULL. */
const osvc_release_package_t *osvc_release_package(
    const osvc_release_t *release, const char *name);

/* version is newer than than; false when either does not parse. */
bool osvc_release_newer(const char *version, const char *than);

typedef enum {
    OSVC_UPDATE_PHASE_NONE = 0,
    OSVC_UPDATE_PHASE_DOWNLOADING,
    OSVC_UPDATE_PHASE_SENDING,
    OSVC_UPDATE_PHASE_INSTALLING,
    OSVC_UPDATE_PHASE_DONE,
    OSVC_UPDATE_PHASE_FAILED,
} osvc_update_phase_t;

/* The update in progress, or the last one. */
typedef struct {
    osvc_update_phase_t phase;
    char version[OSVC_UPDATE_VERSION_MAX + 1U];
    uint64_t sent;
    uint64_t size;
    char error[OSVC_UPDATE_MESSAGE_CAPACITY];
} osvc_update_progress_t;

/* The connected sender; version empty when none has reported one. */
typedef struct {
    char version[OSVC_INFO_VERSION_MAX + 1U];
    /* Empty when it cannot install packages. */
    char package[OSVC_UPDATE_PACKAGE_MAX + 1U];
} osvc_update_sender_t;

/* The package to install on the sender: newest is newer than the sender
 * and lists its package. NULL otherwise. */
const osvc_release_package_t *osvc_release_target(
    const osvc_release_t *newest, const osvc_update_sender_t *sender);

/*
 * Adds "guestUpdate" to object: the newest release and whether the sender
 * can take it, or the update in progress (or the one that just ended, while
 * no newer release replaced it). Adds nothing while neither exists.
 */
bool osvc_update_render(cJSON *object, const osvc_release_t *newest,
                        const osvc_update_sender_t *sender,
                        const osvc_update_progress_t *progress, bool automatic);

#ifdef __cplusplus
}
#endif
