// The local display by time of day (getDisplaySchedule): in the dim window it
// is at half brightness, and in the off window it is off. A window is from
// start to end, "HH:MM", each day. Both windows are in one time zone; a
// schedule whose windows were never turned on has none ("").
export interface DisplayWindow {
  enabled: boolean;
  start: string;
  end: string;
}

export interface DisplaySchedule {
  dim: DisplayWindow;
  off: DisplayWindow;
  timezone: string;
}

export type DisplayWindowName = "dim" | "off";

const TIME = /^([01]\d|2[0-3]):[0-5]\d$/;

// A time field holds "" while a part of it is empty.
export function isTimeOfDay(value: string): boolean {
  return TIME.test(value);
}

// The schedule to store when one window changes. A schedule without a time
// zone gets one when a window is turned on: the browser's zone when the
// device has that zone, and UTC otherwise. A zone that is set stays.
export function scheduleWithWindow(
  schedule: DisplaySchedule,
  name: DisplayWindowName,
  change: Partial<DisplayWindow>,
  timezones: readonly string[],
  browserTimezone: string,
): DisplaySchedule {
  const next = { ...schedule, [name]: { ...schedule[name], ...change } };
  if (next.timezone || !(next.dim.enabled || next.off.enabled)) return next;
  return { ...next, timezone: timezones.includes(browserTimezone) ? browserTimezone : "UTC" };
}

// What a setting's writes need from the page.
export interface SettingWriterIo<T> {
  // Sends a value to the device. Resolves to null when the device stored
  // it, and to the error when it refused it or the request was not sent.
  send: (value: T) => Promise<string | null>;
  // Puts a value into the controls.
  show: (value: T) => void;
  succeeded: () => void;
  failed: (error: string) => void;
}

export interface SettingWriter<T> {
  // The device's answer to a read.
  read: (value: T) => void;
  // An edit by the user.
  write: (value: T) => void;
  // The RPC channel changed: answers to the writes on their way do not come.
  reset: () => void;
}

// Writes a setting so that the controls end with the value that the device
// has. An edit shows at once. One write is on its way at a time; an edit
// during it waits for its answer, and of several such edits the newest is
// sent. When no edit waits any more, the controls show the value that the
// device stored last, or the one that it reported, after a failed write.
export function settingWriter<T>(io: SettingWriterIo<T>): SettingWriter<T> {
  let stored: T | undefined;
  let wanted: T | undefined;
  let writing = false;
  // Counts the resets, so that an answer from before a reset changes nothing.
  let channel = 0;

  const next = () => {
    if (wanted === undefined) return;
    const value = wanted;
    const sentOn = channel;
    writing = true;
    void io.send(value).then(error => {
      if (sentOn !== channel) return;
      writing = false;
      if (error === null) stored = value;
      else io.failed(error);
      if (wanted !== value) {
        next();
        return;
      }
      wanted = undefined;
      if (error === null) io.succeeded();
      else if (stored !== undefined) io.show(stored);
    });
  };

  return {
    read: value => {
      stored = value;
      if (wanted === undefined) io.show(value);
    },
    write: value => {
      wanted = value;
      io.show(value);
      if (!writing) next();
    },
    reset: () => {
      channel++;
      writing = false;
      wanted = undefined;
    },
  };
}
