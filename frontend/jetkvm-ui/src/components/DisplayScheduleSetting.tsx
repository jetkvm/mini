import { useEffect, useMemo, useRef, useState } from "react";
import { LuX } from "react-icons/lu";

import { JsonRpcResponse, useJsonRpc } from "@hooks/useJsonRpc";
import { Button } from "@components/Button";
import InputField from "@components/InputField";
import { SelectMenuBasic } from "@components/SelectMenuBasic";
import { SettingsItem } from "@components/SettingsItem";
import notifications from "@/notifications";
import {
  DisplaySchedule,
  DisplayWindow,
  DisplayWindowName,
  isTimeOfDay,
  scheduleWithWindow,
  settingWriter,
} from "@/utils/displaySchedule";
import { m } from "@localizations/messages.js";

// The display's schedule on the device, for the rows below.
export interface DisplayScheduleState {
  // Null until the device has answered.
  schedule: DisplaySchedule | null;
  timezones: string[];
  // Shows a schedule at once and stores it on the device.
  store: (next: DisplaySchedule) => void;
}

// Reads the schedule of a device that reports the display_schedule
// capability, and writes the user's edits.
export function useDisplaySchedule(supported: boolean): DisplayScheduleState {
  const { send } = useJsonRpc();
  const [schedule, setSchedule] = useState<DisplaySchedule | null>(null);
  const [timezones, setTimezones] = useState<string[]>([]);

  // The writer lives as long as the page and sends through the RPC channel
  // of the moment.
  const sendNow = useRef(send);
  useEffect(() => {
    sendNow.current = send;
  }, [send]);
  const [writer] = useState(() =>
    settingWriter<DisplaySchedule>({
      send: value =>
        new Promise(resolve => {
          void sendNow
            .current("setDisplaySchedule", { params: value }, (resp: JsonRpcResponse) =>
              resolve("error" in resp ? resp.error.data || m.unknown_error() : null),
            )
            .then(sent => {
              if (!sent) resolve(m.unknown_error());
            });
        }),
      show: setSchedule,
      succeeded: () => notifications.success(m.hardware_display_schedule_success()),
      failed: error => notifications.error(m.hardware_display_schedule_error({ error })),
    }),
  );

  // Runs again when the RPC channel changes. The schedule is read after the
  // answer with the zones: the rows appear with the schedule, and a window
  // that is turned on for the first time needs the zones. Without the zones,
  // such a schedule is in UTC.
  useEffect(() => {
    if (!supported) return;
    writer.reset();
    send("getDisplayScheduleTimezones", {}, (zones: JsonRpcResponse) => {
      if ("error" in zones)
        console.error("Failed to get display schedule time zones:", zones.error);
      else setTimezones(zones.result as string[]);
      send("getDisplaySchedule", {}, (resp: JsonRpcResponse) => {
        if ("error" in resp) {
          console.error("Failed to get display schedule:", resp.error);
          return;
        }
        writer.read(resp.result as DisplaySchedule);
      });
    });
  }, [send, supported, writer]);

  return { schedule, timezones, store: writer.write };
}

// The rules of one action on the display, "Dim Display" or "Turn off
// Display", under its title.
export function DisplayRuleGroup({
  title,
  children,
}: {
  title: string;
  children: React.ReactNode;
}) {
  return (
    <div className="space-y-3">
      <div className="text-base font-semibold text-black dark:text-white">{title}</div>
      <div className="space-y-3 pl-4">{children}</div>
    </div>
  );
}

// One time of a window. It is stored when the field loses the focus.
function TimeField({
  label,
  value,
  onStore,
}: {
  label: string;
  value: string;
  onStore: (value: string) => void;
}) {
  // The field as typed, and the stored time that it started from.
  const [typed, setTyped] = useState(value);
  const [shown, setShown] = useState(value);
  if (shown !== value) {
    setShown(value);
    setTyped(value);
  }
  return (
    <InputField
      size="SM"
      type="time"
      // The browser draws the clock symbol in the color of its color scheme.
      className="dark:[color-scheme:dark]"
      aria-label={label}
      value={typed}
      onChange={e => setTyped(e.target.value)}
      onBlur={e => {
        if (e.target.value === value) return;
        if (isTimeOfDay(e.target.value)) onStore(e.target.value);
        else setTyped(value);
      }}
    />
  );
}

// "Between": the dim window or the off window, as Never or as two times.
export function DisplayWindowSetting({
  name,
  description,
  state,
}: {
  name: DisplayWindowName;
  description: string;
  state: DisplayScheduleState;
}) {
  const { schedule, timezones, store } = state;
  if (!schedule) return null;
  const window = schedule[name];
  const change = (next: Partial<DisplayWindow>) =>
    store(
      scheduleWithWindow(
        schedule,
        name,
        next,
        timezones,
        Intl.DateTimeFormat().resolvedOptions().timeZone,
      ),
    );

  return (
    <SettingsItem
      size="SM"
      title={m.hardware_display_schedule_between_title()}
      description={description}
    >
      {window.enabled ? (
        <div className="flex items-center gap-x-2">
          <TimeField
            label={m.hardware_display_schedule_start_title()}
            value={window.start}
            onStore={start => change({ start })}
          />
          <span className="text-sm text-slate-700 dark:text-slate-300">–</span>
          <TimeField
            label={m.hardware_display_schedule_end_title()}
            value={window.end}
            onStore={end => change({ end })}
          />
          <Button
            size="SM"
            theme="light"
            type="button"
            aria-label={m.hardware_time_never()}
            LeadingIcon={LuX}
            onClick={() => change({ enabled: false })}
          />
        </div>
      ) : (
        <SelectMenuBasic
          size="SM"
          label=""
          value="never"
          options={[
            { value: "never", label: m.hardware_time_never() },
            { value: "set", label: m.hardware_display_schedule_set_times() },
          ]}
          onChange={e => {
            if (e.target.value === "set") change({ enabled: true });
          }}
        />
      )}
    </SettingsItem>
  );
}

// The one time zone of both windows: a small line whose zone is a menu.
export function DisplayScheduleTimezone({ state }: { state: DisplayScheduleState }) {
  const { schedule, timezones, store } = state;
  const options = useMemo(
    () => timezones.map(timezone => ({ value: timezone, label: timezone })),
    [timezones],
  );
  if (!schedule || !(schedule.dim.enabled || schedule.off.enabled)) return null;
  // The words of the message around the place of the zone.
  const [before, after] = m.hardware_display_schedule_timezone({ timezone: "\n" }).split("\n");

  return (
    <div className="flex items-center gap-x-2 text-xs text-slate-600 dark:text-slate-400">
      {before && <span>{before.trim()}</span>}
      <SelectMenuBasic
        size="XS"
        label=""
        value={schedule.timezone}
        options={
          options.length ? options : [{ value: schedule.timezone, label: schedule.timezone }]
        }
        onChange={e => store({ ...schedule, timezone: e.target.value })}
      />
      {after && <span>{after.trim()}</span>}
    </div>
  );
}
