/* The quiet hours of the local display. test_settings.py puts its stand-ins
 * and local_display_settings.c in front of this file. */

/* UTC seconds of a date and time in 2026 or 2027. */
static int64_t utc(int year, int month, int day, int hour, int minute, int second)
{
    static const int before[] = {0, 31, 59, 90, 120, 151, 181, 212, 243, 273, 304, 334};
    assert((year == 2026 || year == 2027) && month >= 1 && month <= 12);
    /* 2026-01-01 is day 20454; neither year has a 29 February. */
    int64_t days = 20454 + (year - 2026) * 365 + before[month - 1] + day - 1;
    return days * 86400 + hour * 3600 + minute * 60 + second;
}

static jetkvm_display_schedule_t schedule(bool enabled, bool dim, int start_hour, int start_minute,
                                          int end_hour, int end_minute, const char *zone)
{
    jetkvm_display_schedule_t value = {.enabled = enabled, .dim = dim,
        .start_minute = (uint16_t)(start_hour * 60 + start_minute),
        .end_minute = (uint16_t)(end_hour * 60 + end_minute)};
    assert(strlen(zone) < sizeof(value.timezone));
    strcpy(value.timezone, zone);
    return value;
}

static bool quiet_at(const jetkvm_display_schedule_t *value, int64_t when)
{
    return local_display_quiet(value, jetkvm_timezone_rule(value->timezone), when);
}

static void test_quiet_hours(void)
{
    assert(utc(2026, 11, 27, 0, 0, 0) == 1795737600);
    /* 22:00 to 06:00 crosses midnight. Stockholm is UTC+1 in January and
     * UTC+2 in July. */
    jetkvm_display_schedule_t night = schedule(true, false, 22, 0, 6, 0, "Europe/Stockholm");
    assert(!quiet_at(&night, utc(2026, 1, 15, 20, 59, 59)));
    assert(quiet_at(&night, utc(2026, 1, 15, 21, 0, 0)));
    assert(quiet_at(&night, utc(2026, 1, 15, 23, 30, 0)));
    assert(quiet_at(&night, utc(2026, 1, 16, 4, 59, 59)));
    assert(!quiet_at(&night, utc(2026, 1, 16, 5, 0, 0)));
    assert(!quiet_at(&night, utc(2026, 1, 16, 12, 0, 0)));
    assert(!quiet_at(&night, utc(2026, 7, 15, 19, 59, 59)));
    assert(quiet_at(&night, utc(2026, 7, 15, 20, 0, 0)));
    assert(quiet_at(&night, utc(2026, 7, 16, 3, 59, 59)));
    assert(!quiet_at(&night, utc(2026, 7, 16, 4, 0, 0)));

    /* A period inside one day. Tokyo has no daylight time. */
    jetkvm_display_schedule_t office = schedule(true, false, 9, 0, 17, 30, "Asia/Tokyo");
    assert(!quiet_at(&office, utc(2026, 7, 14, 23, 59, 59)));
    assert(quiet_at(&office, utc(2026, 7, 15, 0, 0, 0)));
    assert(quiet_at(&office, utc(2026, 7, 15, 8, 29, 59)));
    assert(!quiet_at(&office, utc(2026, 7, 15, 8, 30, 0)));
    assert(quiet_at(&office, utc(2026, 1, 15, 0, 0, 0)));

    /* 29 March 2026: the clock goes from 01:59 to 03:00, so local 02:30 to
     * 03:30 starts at the change. */
    jetkvm_display_schedule_t spring = schedule(true, false, 2, 30, 3, 30, "Europe/Stockholm");
    assert(!quiet_at(&spring, utc(2026, 3, 29, 0, 59, 59)));
    assert(quiet_at(&spring, utc(2026, 3, 29, 1, 0, 0)));
    assert(quiet_at(&spring, utc(2026, 3, 29, 1, 29, 59)));
    assert(!quiet_at(&spring, utc(2026, 3, 29, 1, 30, 0)));
    /* 25 October 2026: the hour from 02:00 to 03:00 comes twice. */
    jetkvm_display_schedule_t autumn = schedule(true, false, 2, 0, 3, 0, "Europe/Stockholm");
    assert(!quiet_at(&autumn, utc(2026, 10, 24, 23, 59, 59)));
    assert(quiet_at(&autumn, utc(2026, 10, 25, 0, 0, 0)));
    assert(quiet_at(&autumn, utc(2026, 10, 25, 0, 59, 59)));
    assert(quiet_at(&autumn, utc(2026, 10, 25, 1, 0, 0)));
    assert(quiet_at(&autumn, utc(2026, 10, 25, 1, 59, 59)));
    assert(!quiet_at(&autumn, utc(2026, 10, 25, 2, 0, 0)));

    /* Off, or without a rule, there are no quiet hours. */
    jetkvm_display_schedule_t off = schedule(false, false, 22, 0, 6, 0, "Europe/Stockholm");
    assert(!quiet_at(&off, utc(2026, 1, 15, 23, 30, 0)));
    assert(!local_display_quiet(&night, NULL, utc(2026, 1, 15, 23, 30, 0)));
    assert(!local_display_quiet(&night, "not a rule", utc(2026, 1, 15, 23, 30, 0)));
}

static void test_brightness(void)
{
    const jetkvm_display_schedule_t off = schedule(true, false, 22, 0, 6, 0, "UTC");
    const jetkvm_display_schedule_t dim = schedule(true, true, 22, 0, 6, 0, "UTC");
    const jetkvm_backlight_settings_t always = {64, 0, 0}, timers = {64, 60, 300}, off_only = {64, 0, 300};
    /* Outside the quiet hours only the idle timers act. */
    assert(local_display_scheduled_brightness(&always, &off, false, 1000, UINT64_MAX) == 64);
    assert(local_display_scheduled_brightness(&timers, &off, false, 60, UINT64_MAX) == 32);
    assert(local_display_scheduled_brightness(&timers, &off, false, 300, 300) == 0);
    /* In them the display is off, or at half brightness for a dim schedule. */
    assert(local_display_scheduled_brightness(&always, &off, true, 0, UINT64_MAX) == 0);
    assert(local_display_scheduled_brightness(&always, &dim, true, 0, UINT64_MAX) == 32);
    /* A dim schedule does not light a display that its off timer turned off. */
    assert(local_display_scheduled_brightness(&timers, &dim, true, 300, UINT64_MAX) == 0);
    /* A press lights it until the first idle timer that is set: 30 s with
     * none, the dim timer, or the off timer alone. */
    assert(local_display_scheduled_brightness(&always, &off, true, 29, 29) == 64);
    assert(local_display_scheduled_brightness(&always, &off, true, 30, 30) == 0);
    assert(local_display_scheduled_brightness(&timers, &off, true, 59, 59) == 64);
    assert(local_display_scheduled_brightness(&timers, &off, true, 60, 60) == 0);
    assert(local_display_scheduled_brightness(&timers, &dim, true, 60, 60) == 32);
    assert(local_display_scheduled_brightness(&off_only, &off, true, 299, 299) == 64);
    assert(local_display_scheduled_brightness(&off_only, &off, true, 300, 300) == 0);
    assert(local_display_scheduled_brightness(&off_only, &dim, true, 299, 299) == 64);
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
    /* Without a stored schedule: off, 22:00 to 06:00, UTC. */
    assert(registered.get_schedule(&stored, NULL) && !stored.enabled && !stored.dim &&
           stored.start_minute == 1320 && stored.end_minute == 360 && !strcmp(stored.timezone, "UTC"));
    wall = utc(2026, 11, 27, 23, 0, 0);
    clock_set = true;
    local_display_tick();
    assert(physical_brightness == 35);

    jetkvm_display_schedule_t night = schedule(true, false, 22, 0, 6, 0, "UTC");
    /* Until the clock has the network time, the schedule does nothing. */
    clock_set = false;
    assert(!registered.set_schedule(&night, NULL));
    local_display_tick();
    assert(physical_brightness == 35);
    clock_set = true;
    local_display_tick();
    assert(physical_brightness == 0);
    assert(local_display_state(&snapshot) && snapshot.dark);

    /* A state change does not wake the display in the quiet hours. */
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

    /* The quiet state of a minute is found once, at the first tick in it. */
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

    /* A dim schedule: half brightness, which is not dark. */
    jetkvm_display_schedule_t dim = schedule(true, true, 6, 0, 7, 0, "UTC");
    assert(!registered.set_schedule(&dim, NULL));
    local_display_tick();
    assert(physical_brightness == 32);
    assert(local_display_state(&snapshot) && !snapshot.dark);
    /* The schedule follows local time: 06:30 UTC is 07:30 in Stockholm. */
    wall = utc(2026, 11, 28, 6, 30, 0);
    jetkvm_display_schedule_t local = schedule(true, false, 6, 0, 7, 0, "Europe/Stockholm");
    assert(!registered.set_schedule(&local, NULL));
    local_display_wake();
    local_display_tick();
    assert(physical_brightness == 64);
    now += 3600000000LL;
    local_display_tick();
    assert(physical_brightness == 32);
    local = schedule(true, false, 7, 0, 8, 0, "Europe/Stockholm");
    assert(!registered.set_schedule(&local, NULL));
    local_display_tick();
    assert(physical_brightness == 0);
}

static void test_storage(void)
{
    restart(true);
    jetkvm_display_schedule_t stored, night = schedule(true, true, 22, 15, 6, 45, "America/New_York");
    uint32_t settings_before[5];
    bool had_settings = has_saved;
    memcpy(settings_before, saved, sizeof(saved));
    assert(!registered.set_schedule(&night, NULL) && has_schedule);
    /* The schedule has its own key; the record that a firmware without it
     * reads stays as it was. */
    assert(has_saved == had_settings && !memcmp(settings_before, saved, sizeof(saved)));
    restart(true);
    assert(registered.get_schedule(&stored, NULL) && stored.enabled && stored.dim &&
           stored.start_minute == 22 * 60 + 15 && stored.end_minute == 6 * 60 + 45 &&
           !strcmp(stored.timezone, "America/New_York"));

    /* What cannot be used is refused and stores nothing. */
    uint8_t schedule_before[sizeof(saved_schedule)];
    memcpy(schedule_before, saved_schedule, sizeof(saved_schedule));
    jetkvm_display_schedule_t bad = schedule(true, false, 22, 0, 6, 0, "Europe/Atlantis");
    assert(registered.set_schedule(&bad, NULL));
    bad = schedule(true, false, 22, 0, 22, 0, "UTC");
    assert(registered.set_schedule(&bad, NULL));
    bad = schedule(true, false, 22, 0, 6, 0, "UTC");
    bad.end_minute = 1440;
    assert(registered.set_schedule(&bad, NULL));
    memset(bad.timezone, 'A', sizeof(bad.timezone));
    assert(registered.set_schedule(&bad, NULL));
    /* A failed write keeps the schedule in use. */
    save_fails = true;
    jetkvm_display_schedule_t other = schedule(false, false, 1, 0, 2, 0, "UTC");
    assert(registered.set_schedule(&other, NULL));
    save_fails = false;
    assert(!memcmp(schedule_before, saved_schedule, sizeof(saved_schedule)));
    assert(registered.get_schedule(&stored, NULL) && stored.enabled && !strcmp(stored.timezone, "America/New_York"));

    /* A stored record that this firmware cannot use leaves the schedule off. */
    for (int which = 0; which < 5; ++which) {
        memcpy(saved_schedule, schedule_before, sizeof(saved_schedule));
        saved_schedule_size = sizeof(saved_schedule);
        if (which == 0) saved_schedule[0] = 2;                      /* another version */
        if (which == 1) memcpy(saved_schedule + 8, "Europe/Atlantis", 16); /* a zone that left the table */
        if (which == 2) saved_schedule_size = sizeof(saved_schedule) - 1U;
        if (which == 3) saved_schedule[1] = 2;                      /* enabled is 0 or 1 */
        if (which == 4) memcpy(saved_schedule + 4, saved_schedule + 6, 2); /* start equals end */
        restart(true);
        assert(registered.get_schedule(&stored, NULL) && !stored.enabled &&
               stored.start_minute == 1320 && stored.end_minute == 360 && !strcmp(stored.timezone, "UTC"));
    }
    memcpy(saved_schedule, schedule_before, sizeof(saved_schedule));
    saved_schedule_size = sizeof(saved_schedule);
    /* A reset boot does not read the schedule, so it cannot hide the progress. */
    restart(false);
    assert(registered.get_schedule(&stored, NULL) && !stored.enabled);
    restart(true);
    assert(registered.get_schedule(&stored, NULL) && stored.enabled);

    /* The names that set_schedule takes, in order, with an end. */
    const char *name = NULL, *previous = NULL;
    size_t count = 0;
    for (;; ++count) {
        assert(registered.schedule_timezone(count, &name, NULL));
        if (!name) break;
        if (previous) assert(strcmp(previous, name) < 0);
        previous = name;
        jetkvm_display_schedule_t each = schedule(false, false, 22, 0, 6, 0, name);
        assert(!registered.set_schedule(&each, NULL));
    }
    assert(count > 300);
    assert(!registered.schedule_timezone(0, NULL, NULL));
}

int main(void)
{
    test_quiet_hours();
    test_brightness();
    test_display();
    test_storage();
    puts("Display schedule: quiet hours by local time, press and state change, no network time and storage: PASS");
}
