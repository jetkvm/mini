// The quiet hours of the local display (getDisplaySchedule): between start
// and end, "HH:MM" in the time zone, the display is dimmed or off. A schedule
// that was never turned on has no time zone ("").
export interface DisplaySchedule {
  enabled: boolean;
  action: "off" | "dim";
  start: string;
  end: string;
  timezone: string;
}

// The values of the schedule menu.
export type DisplayScheduleMenu = "never" | "dim" | "off";

const TIME = /^([01]\d|2[0-3]):[0-5]\d$/;

// A time field holds "" while a part of it is empty.
export function isTimeOfDay(value: string): boolean {
  return TIME.test(value);
}

export function menuOfSchedule(schedule: DisplaySchedule): DisplayScheduleMenu {
  return schedule.enabled ? schedule.action : "never";
}

// The schedule to store when the menu changes to a value. A schedule
// without a time zone is turned on for the first time: it takes the browser's
// zone when the device has that zone, and UTC otherwise.
export function scheduleForMenu(
  schedule: DisplaySchedule,
  value: string,
  timezones: readonly string[],
  browserTimezone: string,
): DisplaySchedule {
  if (value !== "dim" && value !== "off") return { ...schedule, enabled: false };
  const first = timezones.includes(browserTimezone) ? browserTimezone : "UTC";
  return { ...schedule, enabled: true, action: value, timezone: schedule.timezone || first };
}
