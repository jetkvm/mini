/*
 * SPDX-FileCopyrightText: 2026
 * SPDX-License-Identifier: MIT
 */

/* A scripted platform for the session tests: a settable clock, a seeded
 * random source, a real Ed25519 key (Monocypher), and an IN endpoint that
 * records every message the KVM writes. */

#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "os_services_session.h"

#define FAKE_OUT_CAPACITY (1024U * 1024U)
#define FAKE_CLIPBOARD_EVENTS 16U
#define FAKE_TERMINAL_EVENTS 32U
#define FAKE_TERMINAL_OUTPUT (256U * 1024U)
#define FAKE_FILE_EVENTS 1024U

typedef struct {
    uint32_t terminal_id;
    char reason[128];
    bool exited;
} fake_terminal_closed_t;

/* A file_credit (done false) or a file_done. */
typedef struct {
    bool done;
    uint32_t drop_id;
    uint32_t transfer_id;
    uint64_t bytes;
    osvc_file_status_t status;
    char path[128];
    size_t path_length;
    bool last;
} fake_file_event_t;

typedef struct {
    uint32_t request_id;
    osvc_clipboard_status_t status;
    bool answered;
    char text[OSVC_CLIPBOARD_MAX_TEXT + 1U];
    size_t length;
    bool truncated;
} fake_clipboard_event_t;

typedef struct {
    int64_t now;
    uint64_t random_state;
    uint8_t secret_key[64];
    uint8_t public_key[32];
    uint8_t out[FAKE_OUT_CAPACITY];
    size_t out_length;
    size_t out_read;
    bool fail_writes;
    unsigned writes;
    unsigned ended;
    char end_reason[128];
    unsigned resets;
    bool drain_on_reset;
    unsigned state_changes;
    fake_clipboard_event_t clipboard[FAKE_CLIPBOARD_EVENTS];
    size_t clipboard_count;
    /* Shell output as the browser would see it, and the terminal of the
     * last piece; the browser refuses output while backed_up is set. */
    uint8_t terminal_output[FAKE_TERMINAL_OUTPUT];
    size_t terminal_output_length;
    uint32_t terminal_output_id;
    unsigned terminal_output_calls;
    bool backed_up;
    fake_terminal_closed_t terminal_closed[FAKE_TERMINAL_EVENTS];
    size_t terminal_closed_count;
    fake_file_event_t file[FAKE_FILE_EVENTS];
    size_t file_count;
} fake_platform_t;

typedef struct {
    uint16_t service;
    uint16_t kind;
    uint32_t length;
    const uint8_t *payload;
} fake_message_t;

void fake_platform_init(fake_platform_t *fake, osvc_platform_t *platform);
/* The next message the KVM wrote, or false when none is left. */
bool fake_next_message(fake_platform_t *fake, fake_message_t *message);
/* Forgets every message the KVM wrote so far. */
void fake_clear_output(fake_platform_t *fake);
