/*
 * SPDX-FileCopyrightText: 2026
 * SPDX-License-Identifier: MIT
 */

#include <assert.h>
#include <stdint.h>
#include <stdio.h>

#include "http_upload_lifecycle.h"

int main(void)
{
    const int64_t started = INT64_C(987654321);

    assert(!http_upload_receive_idle_timed_out(started, 0));
    assert(!http_upload_receive_idle_timed_out(started - 1, started));
    assert(!http_upload_receive_idle_timed_out(
        started + HTTP_UPLOAD_RECEIVE_IDLE_TIMEOUT_US - 1, started));
    assert(http_upload_receive_idle_timed_out(
        started + HTTP_UPLOAD_RECEIVE_IDLE_TIMEOUT_US, started));

    const int64_t refreshed =
        started + HTTP_UPLOAD_RECEIVE_IDLE_TIMEOUT_US - 1;
    assert(!http_upload_receive_idle_timed_out(
        refreshed + HTTP_UPLOAD_RECEIVE_IDLE_TIMEOUT_US - 1, refreshed));
    assert(http_upload_receive_idle_timed_out(
        refreshed + HTTP_UPLOAD_RECEIVE_IDLE_TIMEOUT_US, refreshed));

    assert(http_upload_receive_idle_timed_out(
        INT64_MAX, INT64_MAX - HTTP_UPLOAD_RECEIVE_IDLE_TIMEOUT_US));

    assert(http_upload_resume_prefix(0U, 512U) == 0U);
    assert(http_upload_resume_prefix(1U, 512U) == 511U);
    assert(http_upload_resume_prefix(127U, 512U) == 385U);
    assert(http_upload_resume_prefix(128U, 512U) == 384U);
    assert(http_upload_resume_prefix(511U, 512U) == 1U);
    assert(http_upload_resume_prefix(512U, 512U) == 0U);
    assert(http_upload_resume_prefix(513U, 512U) == 511U);
    assert(http_upload_resume_prefix(UINT64_MAX, 512U) == 1U);
    assert(http_upload_resume_prefix(1U, 0U) == 0U);

    puts("HTTP upload lifecycle: PASS");
    return 0;
}
