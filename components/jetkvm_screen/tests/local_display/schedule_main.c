/* The dim window and the off window of the local display. test_settings.py
 * puts its stand-ins and local_display_settings.c in front of this file. */

/* UTC seconds of a date and time in 2026 or 2027. */
static int64_t utc(int year, int month, int day, int hour, int minute, int second)
{
    static const int before[] = {0, 31, 59, 90, 120, 151, 181, 212, 243, 273, 304, 334};
    assert((year == 2026 || year == 2027) && month >= 1 && month <= 12);
    /* 2026-01-01 is day 20454; neither year has a 29 February. */
    int64_t days = 20454 + (year - 2026) * 365 + before[month - 1] + day - 1;
    return days * 86400 + hour * 3600 + minute * 60 + second;
}

static jetkvm_display_window_t window(bool enabled, int start_hour, int start_minute, int end_hour, int end_minute)
{
    return (jetkvm_display_window_t){.enabled = enabled,
        .start_minute = (uint16_t)(start_hour * 60 + start_minute),
        .end_minute = (uint16_t)(end_hour * 60 + end_minute)};
}

static jetkvm_display_schedule_t schedule(jetkvm_display_window_t dim, jetkvm_display_window_t off, const char *zone)
{
    jetkvm_display_schedule_t value = {.dim = dim, .off = off};
    assert(strlen(zone) < sizeof(value.timezone));
    strcpy(value.timezone, zone);
    return value;
}

/* Windows that are off, with times that a schedule may have. */
#define NO_DIM window(false, 20, 0, 23, 0)
#define NO_OFF window(false, 22, 0, 6, 0)

static bool in_window(const jetkvm_display_window_t *value, const char *zone, int64_t when)
{
    return local_display_in_window(value, jetkvm_timezone_rule(zone), when);
}

static void test_windows(void)
{
    assert(utc(2026, 11, 27, 0, 0, 0) == 1795737600);
    /* 22:00 to 06:00 crosses midnight. Stockholm is UTC+1 in January and
     * UTC+2 in July. */
    jetkvm_display_window_t night = window(true, 22, 0, 6, 0);
    assert(!in_window(&night, "Europe/Stockholm", utc(2026, 1, 15, 20, 59, 59)));
    assert(in_window(&night, "Europe/Stockholm", utc(2026, 1, 15, 21, 0, 0)));
    assert(in_window(&night, "Europe/Stockholm", utc(2026, 1, 15, 23, 30, 0)));
    assert(in_window(&night, "Europe/Stockholm", utc(2026, 1, 16, 4, 59, 59)));
    assert(!in_window(&night, "Europe/Stockholm", utc(2026, 1, 16, 5, 0, 0)));
    assert(!in_window(&night, "Europe/Stockholm", utc(2026, 1, 16, 12, 0, 0)));
    assert(!in_window(&night, "Europe/Stockholm", utc(2026, 7, 15, 19, 59, 59)));
    assert(in_window(&night, "Europe/Stockholm", utc(2026, 7, 15, 20, 0, 0)));
    assert(in_window(&night, "Europe/Stockholm", utc(2026, 7, 16, 3, 59, 59)));
    assert(!in_window(&night, "Europe/Stockholm", utc(2026, 7, 16, 4, 0, 0)));

    /* A window inside one day. Tokyo has no daylight time. */
    jetkvm_display_window_t office = window(true, 9, 0, 17, 30);
    assert(!in_window(&office, "Asia/Tokyo", utc(2026, 7, 14, 23, 59, 59)));
    assert(in_window(&office, "Asia/Tokyo", utc(2026, 7, 15, 0, 0, 0)));
    assert(in_window(&office, "Asia/Tokyo", utc(2026, 7, 15, 8, 29, 59)));
    assert(!in_window(&office, "Asia/Tokyo", utc(2026, 7, 15, 8, 30, 0)));
    assert(in_window(&office, "Asia/Tokyo", utc(2026, 1, 15, 0, 0, 0)));

    /* 29 March 2026: the clock goes from 01:59 to 03:00, so local 02:30 to
     * 03:30 starts at the change. */
    jetkvm_display_window_t spring = window(true, 2, 30, 3, 30);
    assert(!in_window(&spring, "Europe/Stockholm", utc(2026, 3, 29, 0, 59, 59)));
    assert(in_window(&spring, "Europe/Stockholm", utc(2026, 3, 29, 1, 0, 0)));
    assert(in_window(&spring, "Europe/Stockholm", utc(2026, 3, 29, 1, 29, 59)));
    assert(!in_window(&spring, "Europe/Stockholm", utc(2026, 3, 29, 1, 30, 0)));
    /* 25 October 2026: the hour from 02:00 to 03:00 comes twice. */
    jetkvm_display_window_t autumn = window(true, 2, 0, 3, 0);
    assert(!in_window(&autumn, "Europe/Stockholm", utc(2026, 10, 24, 23, 59, 59)));
    assert(in_window(&autumn, "Europe/Stockholm", utc(2026, 10, 25, 0, 0, 0)));
    assert(in_window(&autumn, "Europe/Stockholm", utc(2026, 10, 25, 0, 59, 59)));
    assert(in_window(&autumn, "Europe/Stockholm", utc(2026, 10, 25, 1, 0, 0)));
    assert(in_window(&autumn, "Europe/Stockholm", utc(2026, 10, 25, 1, 59, 59)));
    assert(!in_window(&autumn, "Europe/Stockholm", utc(2026, 10, 25, 2, 0, 0)));

    /* Off, or without a rule, a window has no time. */
    jetkvm_display_window_t off = window(false, 22, 0, 6, 0);
    assert(!in_window(&off, "Europe/Stockholm", utc(2026, 1, 15, 23, 30, 0)));
    assert(!local_display_in_window(&night, NULL, utc(2026, 1, 15, 23, 30, 0)));
    assert(!local_display_in_window(&night, "not a rule", utc(2026, 1, 15, 23, 30, 0)));
}

/* The darkest of the idle timers and the two windows wins. */
static void test_brightness(void)
{
    const jetkvm_backlight_settings_t always = {64, 0, 0}, timers = {64, 60, 300}, off_only = {64, 0, 300};
    /* In no window only the idle timers act. */
    assert(local_display_scheduled_brightness(&always, false, false, 1000, UINT64_MAX) == 64);
    assert(local_display_scheduled_brightness(&timers, false, false, 60, UINT64_MAX) == 32);
    assert(local_display_scheduled_brightness(&timers, false, false, 300, 300) == 0);
    /* In the dim window the display is at half brightness, in the off window
     * it is off, and in both it is off. */
    assert(local_display_scheduled_brightness(&always, true, false, 0, UINT64_MAX) == 32);
    assert(local_display_scheduled_brightness(&always, false, true, 0, UINT64_MAX) == 0);
    assert(local_display_scheduled_brightness(&always, true, true, 0, UINT64_MAX) == 0);
    /* The dim window does not light a display that the off timer turned off,
     * and the off window turns off one that the dim timer only dimmed. */
    assert(local_display_scheduled_brightness(&timers, true, false, 300, UINT64_MAX) == 0);
    assert(local_display_scheduled_brightness(&timers, false, true, 60, UINT64_MAX) == 0);
    /* The dim timer and the dim window together are still half brightness. */
    assert(local_display_scheduled_brightness(&timers, true, false, 60, UINT64_MAX) == 32);
    /* A press lights it until the first idle timer that is set: 30 s with
     * none, the dim timer, or the off timer alone. */
    assert(local_display_scheduled_brightness(&always, false, true, 29, 29) == 64);
    assert(local_display_scheduled_brightness(&always, false, true, 30, 30) == 0);
    assert(local_display_scheduled_brightness(&always, true, true, 29, 29) == 64);
    assert(local_display_scheduled_brightness(&always, true, true, 30, 30) == 0);
    assert(local_display_scheduled_brightness(&timers, false, true, 59, 59) == 64);
    assert(local_display_scheduled_brightness(&timers, false, true, 60, 60) == 0);
    assert(local_display_scheduled_brightness(&timers, true, false, 60, 60) == 32);
    assert(local_display_scheduled_brightness(&off_only, false, true, 299, 299) == 64);
    assert(local_display_scheduled_brightness(&off_only, false, true, 300, 300) == 0);
    assert(local_display_scheduled_brightness(&off_only, true, false, 299, 299) == 64);
}

static const local_display_driver_t driver = {rotate, light, active};

static void restart(bool restore_settings)
{
    s_mutex = NULL;
    memset(&s_state, 0, sizeof(s_state));
    assert(!local_display_settings_init(270, &driver, restore_settings));
    assert(!local_display_prepare() && !local_display_ready() && !local_display_active(true));
}

/* now is the monotonic clock in microseconds, wall the UTC clock in seconds. */
static void test_display(void)
{
    restart(true);
    local_display_state_t snapshot;
    jetkvm_display_schedule_t stored;
    /* Without a stored schedule: no window, dim 20:00 to 23:00, off 22:00 to
     * 06:00, no time zone. */
    assert(registered.get_schedule(&stored, NULL) && !stored.dim.enabled && !stored.off.enabled &&
           stored.dim.start_minute == 1200 && stored.dim.end_minute == 1380 &&
           stored.off.start_minute == 1320 && stored.off.end_minute == 360 && !stored.timezone[0]);
    wall = utc(2026, 11, 27, 20, 30, 0);
    clock_set = true;
    local_display_tick();
    assert(physical_brightness == 35);

    /* Dim from 20:00 to 23:00 and off from 22:00 to 06:00: two windows that
     * share an hour. */
    jetkvm_display_schedule_t evening = schedule(window(true, 20, 0, 23, 0), window(true, 22, 0, 6, 0), "UTC");
    /* Until the clock has the network time, the windows do nothing. */
    clock_set = false;
    assert(!registered.set_schedule(&evening, NULL));
    local_display_tick();
    assert(physical_brightness == 35);
    clock_set = true;
    local_display_tick();
    assert(physical_brightness == 17);
    assert(local_display_state(&snapshot) && !snapshot.dark);
    /* In the shared hour the off window wins. */
    wall = utc(2026, 11, 27, 22, 0, 0);
    local_display_tick();
    assert(physical_brightness == 0);
    assert(local_display_state(&snapshot) && snapshot.dark);
    /* After the dim window, the off window alone. */
    wall = utc(2026, 11, 27, 23, 30, 0);
    local_display_tick();
    assert(physical_brightness == 0);

    /* A state change does not wake the display in a window. */
    now += 5000000;
    local_display_notice();
    local_display_tick();
    assert(physical_brightness == 0);
    /* A press does, for 30 s without an idle timer. */
    local_display_wake();
    local_display_tick();
    assert(physical_brightness == 35);
    now += 29999999;
    local_display_tick();
    assert(physical_brightness == 35);
    now += 1;
    local_display_tick();
    assert(physical_brightness == 0);
    /* A changed backlight setting shows for as long as a press. */
    jetkvm_backlight_settings_t backlight = {64, 10, 0};
    assert(!registered.set_backlight(&backlight, NULL) && physical_brightness == 64);
    now += 9999999;
    local_display_tick();
    assert(physical_brightness == 64);
    now += 1;
    local_display_tick();
    assert(physical_brightness == 0);

    /* The windows of a minute are found once, at the first tick in it. */
    wall = utc(2026, 11, 28, 5, 59, 59);
    local_display_tick();
    assert(physical_brightness == 0);
    /* After the end the idle timers decide again: dimmed, 10 s after the
     * last wake. A state change then wakes the display. */
    wall = utc(2026, 11, 28, 6, 0, 0);
    local_display_tick();
    assert(physical_brightness == 32);
    local_display_notice();
    local_display_tick();
    assert(physical_brightness == 64);
    /* In the dim window a state change does not wake a display that the dim
     * timer and the window dimmed, and the off timer can still turn it off. */
    backlight = (jetkvm_backlight_settings_t){64, 10, 20};
    assert(!registered.set_backlight(&backlight, NULL));
    wall = utc(2026, 11, 28, 20, 0, 0);
    now += 10000000;
    local_display_tick();
    assert(physical_brightness == 32);
    local_display_notice();
    local_display_tick();
    assert(physical_brightness == 32);
    now += 10000000;
    local_display_tick();
    assert(physical_brightness == 0);

    /* The windows follow local time: 06:30 UTC is 07:30 in Stockholm. */
    backlight = (jetkvm_backlight_settings_t){64, 0, 0};
    assert(!registered.set_backlight(&backlight, NULL));
    now += 3600000000LL;
    wall = utc(2026, 11, 29, 6, 30, 0);
    jetkvm_display_schedule_t local = schedule(NO_DIM, window(true, 6, 0, 7, 0), "Europe/Stockholm");
    assert(!registered.set_schedule(&local, NULL));
    local_display_tick();
    assert(physical_brightness == 64);
    local = schedule(window(true, 7, 0, 8, 0), NO_OFF, "Europe/Stockholm");
    assert(!registered.set_schedule(&local, NULL));
    local_display_tick();
    assert(physical_brightness == 32);
    local = schedule(NO_DIM, window(true, 7, 0, 8, 0), "Europe/Stockholm");
    assert(!registered.set_schedule(&local, NULL));
    local_display_tick();
    assert(physical_brightness == 0);
}

static void test_storage(void)
{
    restart(true);
    jetkvm_display_schedule_t stored;
    jetkvm_display_schedule_t night = schedule(window(true, 19, 5, 21, 55), window(false, 22, 15, 6, 45), "America/New_York");
    uint32_t settings_before[5];
    bool had_settings = has_saved;
    memcpy(settings_before, saved, sizeof(saved));
    assert(!registered.set_schedule(&night, NULL) && has_schedule);
    /* The schedule has its own key; the record that a firmware without it
     * reads stays as it was. */
    assert(has_saved == had_settings && !memcmp(settings_before, saved, sizeof(saved)));
    restart(true);
    /* Each window comes back with its own switch and times. */
    assert(registered.get_schedule(&stored, NULL) && stored.dim.enabled && !stored.off.enabled &&
           stored.dim.start_minute == 19 * 60 + 5 && stored.dim.end_minute == 21 * 60 + 55 &&
           stored.off.start_minute == 22 * 60 + 15 && stored.off.end_minute == 6 * 60 + 45 &&
           !strcmp(stored.timezone, "America/New_York"));

    /* What cannot be used is refused and stores nothing. */
    uint8_t schedule_before[sizeof(saved_schedule)];
    memcpy(schedule_before, saved_schedule, sizeof(saved_schedule));
    jetkvm_display_schedule_t bad = schedule(window(true, 22, 0, 6, 0), NO_OFF, "Europe/Atlantis");
    assert(registered.set_schedule(&bad, NULL));
    /* Equal times in either window, also in one that is off. */
    bad = schedule(window(true, 22, 0, 22, 0), NO_OFF, "UTC");
    assert(registered.set_schedule(&bad, NULL));
    bad = schedule(NO_DIM, window(false, 6, 0, 6, 0), "UTC");
    assert(registered.set_schedule(&bad, NULL));
    bad = schedule(NO_DIM, window(true, 22, 0, 6, 0), "UTC");
    bad.off.end_minute = 1440;
    assert(registered.set_schedule(&bad, NULL));
    memset(bad.timezone, 'A', sizeof(bad.timezone));
    assert(registered.set_schedule(&bad, NULL));
    /* A window that is on needs a time zone; either window counts. */
    bad = schedule(window(true, 20, 0, 23, 0), NO_OFF, "");
    assert(registered.set_schedule(&bad, NULL));
    bad = schedule(NO_DIM, window(true, 22, 0, 6, 0), "");
    assert(registered.set_schedule(&bad, NULL));
    /* A failed write keeps the schedule in use. */
    save_fails = true;
    jetkvm_display_schedule_t other = schedule(NO_DIM, NO_OFF, "UTC");
    assert(registered.set_schedule(&other, NULL));
    save_fails = false;
    assert(!memcmp(schedule_before, saved_schedule, sizeof(saved_schedule)));
    assert(registered.get_schedule(&stored, NULL) && stored.dim.enabled && !strcmp(stored.timezone, "America/New_York"));

    /* A stored record that this firmware cannot use leaves both windows off. */
    for (int which = 0; which < 6; ++which) {
        memcpy(saved_schedule, schedule_before, sizeof(saved_schedule));
        saved_schedule_size = sizeof(saved_schedule);
        if (which == 0) saved_schedule[0] = 3;                       /* another version */
        if (which == 1) memcpy(saved_schedule + 12, "Europe/Atlantis", 16); /* a zone that left the table */
        if (which == 2) saved_schedule_size = sizeof(saved_schedule) - 1U;
        if (which == 3) saved_schedule[1] = 2;                       /* a switch is 0 or 1 */
        if (which == 4) memcpy(saved_schedule + 4, saved_schedule + 6, 2); /* dim start equals dim end */
        if (which == 5) memcpy(saved_schedule + 8, saved_schedule + 10, 2); /* off start equals off end */
        restart(true);
        assert(registered.get_schedule(&stored, NULL) && !stored.dim.enabled && !stored.off.enabled &&
               stored.dim.start_minute == 1200 && stored.off.start_minute == 1320 && !stored.timezone[0]);
    }
    /* The record of the builds before the two windows: version 1, 72 bytes,
     * here "on, off action, 22:00 to 06:00, UTC". The Mini starts with no
     * window, and the next write stores the new record. */
    memset(saved_schedule, 0, sizeof(saved_schedule));
    const uint8_t old_record[] = {1, 1, 0, 0, 0x28, 0x05, 0x68, 0x01, 'U', 'T', 'C'};
    memcpy(saved_schedule, old_record, sizeof(old_record));
    saved_schedule_size = 72;
    restart(true);
    assert(registered.get_schedule(&stored, NULL) && !stored.dim.enabled && !stored.off.enabled && !stored.timezone[0]);
    wall = utc(2026, 11, 27, 23, 0, 0);
    clock_set = true;
    local_display_tick();
    jetkvm_backlight_settings_t backlight;
    assert(registered.get_backlight(&backlight, NULL) && backlight.max_brightness == 64 &&
           physical_brightness == 64);
    assert(!registered.set_schedule(&night, NULL) && saved_schedule_size == sizeof(saved_schedule) &&
           saved_schedule[0] == 2);

    /* Both windows off are stored and read without a time zone. */
    jetkvm_display_schedule_t unset = schedule(window(false, 1, 0, 2, 0), window(false, 3, 0, 4, 0), "");
    assert(!registered.set_schedule(&unset, NULL));
    restart(true);
    assert(registered.get_schedule(&stored, NULL) && !stored.dim.enabled && !stored.off.enabled &&
           stored.dim.start_minute == 60 && stored.off.end_minute == 240 && !stored.timezone[0]);
    /* The same record with a window on cannot be used. */
    saved_schedule[2] = 1;
    restart(true);
    assert(registered.get_schedule(&stored, NULL) && !stored.off.enabled && stored.dim.start_minute == 1200);
    saved_schedule[2] = 0;
    /* A reset boot does not read the schedule, so it cannot hide the progress. */
    assert(!registered.set_schedule(&night, NULL));
    restart(false);
    assert(registered.get_schedule(&stored, NULL) && !stored.dim.enabled);
    restart(true);
    assert(registered.get_schedule(&stored, NULL) && stored.dim.enabled);

    /* The names that set_schedule takes, in order, with an end. */
    const char *name = NULL, *previous = NULL;
    size_t count = 0;
    for (;; ++count) {
        assert(registered.schedule_timezone(count, &name, NULL));
        if (!name) break;
        if (previous) assert(strcmp(previous, name) < 0);
        previous = name;
        jetkvm_display_schedule_t each = schedule(NO_DIM, NO_OFF, name);
        assert(!registered.set_schedule(&each, NULL));
    }
    assert(count > 300);
    assert(!registered.schedule_timezone(0, NULL, NULL));
}

int main(void)
{
    test_windows();
    test_brightness();
    test_display();
    test_storage();
    puts("Display schedule: dim and off windows by local time, the darkest rule, press and state change, no network time and storage: PASS");
}
