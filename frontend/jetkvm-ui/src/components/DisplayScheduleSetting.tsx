import { useCallback, useEffect, useMemo, useState } from "react";

import { JsonRpcResponse, useJsonRpc } from "@hooks/useJsonRpc";
import InputField from "@components/InputField";
import { NestedSettingsGroup } from "@components/NestedSettingsGroup";
import { SelectMenuBasic } from "@components/SelectMenuBasic";
import { SettingsItem } from "@components/SettingsItem";
import notifications from "@/notifications";
import {
  DisplaySchedule,
  isTimeOfDay,
  menuOfSchedule,
  scheduleForMenu,
} from "@/utils/displaySchedule";
import { m } from "@localizations/messages.js";

// The quiet hours of the local display. The page shows this only for a
// device that reports the display_schedule capability.
export function DisplayScheduleSetting() {
  const { send } = useJsonRpc();
  const [schedule, setSchedule] = useState<DisplaySchedule | null>(null);
  const [timezones, setTimezones] = useState<string[]>([]);
  // The time fields as typed; a field is stored when it loses the focus.
  const [start, setStart] = useState("");
  const [end, setEnd] = useState("");

  const show = useCallback((value: DisplaySchedule) => {
    setSchedule(value);
    setStart(value.start);
    setEnd(value.end);
  }, []);

  useEffect(() => {
    send("getDisplaySchedule", {}, (resp: JsonRpcResponse) => {
      if ("error" in resp) {
        console.error("Failed to get display schedule:", resp.error);
        return;
      }
      show(resp.result as DisplaySchedule);
    });
    send("getDisplayScheduleTimezones", {}, (resp: JsonRpcResponse) => {
      if ("error" in resp) {
        console.error("Failed to get display schedule time zones:", resp.error);
        return;
      }
      setTimezones(resp.result as string[]);
    });
  }, [send, show]);

  const timezoneOptions = useMemo(
    () => timezones.map(timezone => ({ value: timezone, label: timezone })),
    [timezones],
  );

  // Until the device has answered, there is no schedule to show.
  if (!schedule) return null;

  // The page shows the stored schedule again when the device refuses one.
  const store = (next: DisplaySchedule) => {
    show(next);
    send("setDisplaySchedule", { params: next }, (resp: JsonRpcResponse) => {
      if ("error" in resp) {
        notifications.error(
          m.hardware_display_schedule_error({ error: resp.error.data || m.unknown_error() }),
        );
        show(schedule);
        return;
      }
      notifications.success(m.hardware_display_schedule_success());
    });
  };

  const storeTime = (field: "start" | "end", value: string) => {
    if (value === schedule[field]) return;
    if (!isTimeOfDay(value)) {
      show(schedule);
      return;
    }
    store({ ...schedule, [field]: value });
  };

  return (
    <>
      <SettingsItem
        title={m.hardware_display_schedule_title()}
        description={m.hardware_display_schedule_description()}
      >
        <SelectMenuBasic
          size="SM"
          label=""
          value={menuOfSchedule(schedule)}
          options={[
            { value: "never", label: m.hardware_time_never() },
            { value: "dim", label: m.hardware_display_schedule_dim() },
            { value: "off", label: m.hardware_display_schedule_off() },
          ]}
          onChange={e =>
            store(
              scheduleForMenu(
                schedule,
                e.target.value,
                timezones,
                Intl.DateTimeFormat().resolvedOptions().timeZone,
              ),
            )
          }
        />
      </SettingsItem>
      {schedule.enabled && (
        <NestedSettingsGroup>
          <SettingsItem title={m.hardware_display_schedule_start_title()} description="">
            <InputField
              size="SM"
              type="time"
              aria-label={m.hardware_display_schedule_start_title()}
              value={start}
              onChange={e => setStart(e.target.value)}
              onBlur={e => storeTime("start", e.target.value)}
            />
          </SettingsItem>
          <SettingsItem title={m.hardware_display_schedule_end_title()} description="">
            <InputField
              size="SM"
              type="time"
              aria-label={m.hardware_display_schedule_end_title()}
              value={end}
              onChange={e => setEnd(e.target.value)}
              onBlur={e => storeTime("end", e.target.value)}
            />
          </SettingsItem>
          <SettingsItem title={m.jiggler_timezone_label()} description="">
            <SelectMenuBasic
              size="SM"
              label=""
              value={schedule.timezone}
              options={
                timezoneOptions.length
                  ? timezoneOptions
                  : [{ value: schedule.timezone, label: schedule.timezone }]
              }
              onChange={e => store({ ...schedule, timezone: e.target.value })}
            />
          </SettingsItem>
          <p className="text-xs text-slate-600 dark:text-slate-400">
            {m.hardware_display_schedule_note()}
          </p>
        </NestedSettingsGroup>
      )}
    </>
  );
}
