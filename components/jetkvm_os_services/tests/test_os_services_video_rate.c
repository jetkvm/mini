/*
 * SPDX-FileCopyrightText: 2026
 * SPDX-License-Identifier: MIT
 */

/*
 * The service video rate policy at 1080p (ceiling 8000 kbps): cuts follow
 * the estimate at once by at most half, raises wait and step, the floor and
 * the frame-rate halving, and a link with room keeps the full target.
 */

#include <assert.h>
#include <stdio.h>

#include "os_services_video_rate.h"

static osvc_video_rate_t s_rate;
static int64_t s_now;

static void start(void)
{
    s_now = 0;
    osvc_video_rate_reset(&s_rate, 8000U, s_now);
}

/* One evaluation per second for seconds, each with estimate; the number of
 * changes. */
static unsigned run(uint32_t estimate, unsigned seconds)
{
    unsigned changes = 0U;
    for (unsigned i = 0U; i < seconds; ++i) {
        s_now += 1000;
        changes += osvc_video_rate_step(&s_rate, estimate, s_now) ? 1U : 0U;
    }
    return changes;
}

/* Cuts every 2 s, as the settle time allows, until the target is kbps. */
static void cut_to(uint32_t kbps)
{
    while (s_rate.kbps > kbps) {
        assert(run(kbps, 2U) == 1U);
    }
    assert(s_rate.kbps == kbps);
}

static void test_room_keeps_the_ceiling(void)
{
    start();
    assert(run(8000U, 60U) == 0U && run(30000U, 60U) == 0U);
    /* Dips under 10% are noise. */
    assert(run(7300U, 60U) == 0U);
    assert(s_rate.kbps == 8000U && osvc_video_rate_fps(&s_rate, 30U) == 30U);
}

static void test_cut_at_once_after_the_settle_time(void)
{
    start();
    /* The stream's first IDR is still crossing. */
    assert(run(5456U, 1U) == 0U);
    assert(run(5456U, 1U) == 1U && s_rate.kbps == 5400U);
    assert(s_rate.cuts == 1U);
    /* The next cut waits for the sender to apply this one, and halves the
     * target at most. */
    assert(run(2000U, 1U) == 0U && run(2000U, 1U) == 1U);
    assert(s_rate.kbps == 2700U);
    assert(run(2000U, 2U) == 1U && s_rate.kbps == 2000U && s_rate.cuts == 3U);
    /* Half of 5100 is kept in the 100 kbps steps by rounding up. */
    start();
    assert(run(5100U, 2U) == 1U && run(100U, 2U) == 1U && s_rate.kbps == 2600U);
}

static void test_raise_waits_and_steps(void)
{
    start();
    cut_to(2000U);
    /* No raise for 10 s after a change, and none on a small rise. */
    assert(run(8000U, 9U) == 0U);
    assert(run(8000U, 1U) == 1U && s_rate.kbps == 3000U);
    assert(run(2800U, 10U) == 0U);
    assert(run(3500U, 10U) == 0U);
    /* At most half again, at most the estimate, at most the ceiling. */
    assert(run(4321U, 1U) == 1U && s_rate.kbps == 4300U);
    assert(run(8000U, 10U) == 1U && s_rate.kbps == 6400U);
    assert(run(30000U, 10U) == 1U && s_rate.kbps == 8000U);
    assert(run(30000U, 60U) == 0U && s_rate.raises == 4U && s_rate.cuts == 2U);
}

/* An estimate at the ceiling raises from just under it too. */
static void test_raise_reaches_the_ceiling(void)
{
    start();
    assert(run(7000U, 2U) == 1U && s_rate.kbps == 7000U);
    assert(run(7900U, 30U) == 0U);
    assert(run(8000U, 1U) == 1U && s_rate.kbps == 8000U);
}

static void test_failed_raise_doubles_the_wait(void)
{
    start();
    cut_to(2000U);
    assert(run(8000U, 10U) == 1U && s_rate.kbps == 3000U);
    /* The raise was too much: cut back, and wait 20 s for the next. */
    assert(run(2000U, 2U) == 1U && s_rate.kbps == 2000U);
    assert(run(8000U, 19U) == 0U && run(8000U, 1U) == 1U);
    assert(run(2000U, 2U) == 1U);
    assert(run(8000U, 39U) == 0U && run(8000U, 1U) == 1U);
    assert(run(2000U, 2U) == 1U);
    assert(run(8000U, 59U) == 0U && run(8000U, 1U) == 1U);
    assert(run(2000U, 2U) == 1U);
    assert(run(8000U, 59U) == 0U && run(8000U, 1U) == 1U);
    /* A raise that holds for 10 s brings back the short wait. */
    assert(run(8000U, 10U) == 1U && run(8000U, 10U) == 1U);
}

static void test_floor_and_frame_rate(void)
{
    start();
    assert(run(100U, 2U) == 1U && s_rate.kbps == 4000U);
    assert(run(100U, 2U) == 1U && s_rate.kbps == 2000U);
    assert(osvc_video_rate_fps(&s_rate, 30U) == 30U);
    assert(run(100U, 2U) == 1U && s_rate.kbps == 1000U);
    assert(osvc_video_rate_fps(&s_rate, 30U) == 15U);
    assert(osvc_video_rate_fps(&s_rate, 60U) == 30U);
    assert(run(100U, 10U) == 0U);
    /* Back to the full rate only well above the floor where it halved. */
    assert(run(1700U, 10U) == 1U && s_rate.kbps == 1500U);
    assert(osvc_video_rate_fps(&s_rate, 30U) == 15U);
    assert(run(3000U, 10U) == 1U && s_rate.kbps == 2200U);
    assert(osvc_video_rate_fps(&s_rate, 30U) == 15U);
    assert(run(3000U, 10U) == 1U && s_rate.kbps == 3000U);
    assert(osvc_video_rate_fps(&s_rate, 30U) == 30U);
    assert(run(1200U, 2U) == 1U && run(1200U, 2U) == 1U);
    assert(s_rate.kbps == 1200U && osvc_video_rate_fps(&s_rate, 30U) == 15U);
}

int main(void)
{
    test_room_keeps_the_ceiling();
    test_cut_at_once_after_the_settle_time();
    test_raise_waits_and_steps();
    test_raise_reaches_the_ceiling();
    test_failed_raise_doubles_the_wait();
    test_floor_and_frame_rate();
    printf("os_services video rate tests passed\n");
    return 0;
}
