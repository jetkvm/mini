import { useEffect, useState } from "react";
import { useRTCStore } from "@hooks/stores";
import { useJsonRpc } from "@hooks/useJsonRpc";
import { SelectMenuBasic } from "@components/SelectMenuBasic";
import { SettingsItem } from "@components/SettingsItem";
import notifications from "@/notifications";
import { m } from "@localizations/messages";

type MacAddressSource = "default" | "espressif";
interface MacAddressSettings {
  available: boolean;
  source?: MacAddressSource;
  default_mac?: string;
  espressif_mac?: string;
}

// A device with a factory MAC address chooses between that address and the
// Espressif MAC of its chip; the choice takes effect at the next restart.
export default function MacAddressSourceSetting() {
  const { send } = useJsonRpc();
  const channel = useRTCStore(state => state.rpcDataChannel);
  const [settings, setSettings] = useState<MacAddressSettings | null>(null);
  const [saving, setSaving] = useState(false);

  useEffect(() => {
    if (channel?.readyState !== "open") return;
    send("getMacAddressSettings", {}, response => {
      if ("result" in response) setSettings(response.result as MacAddressSettings);
    });
  }, [channel, send]);

  if (!settings?.available) return null;
  const save = (source: MacAddressSource) => {
    setSaving(true);
    send("setMacAddressSettings", { source }, response => {
      setSaving(false);
      if ("error" in response) {
        notifications.error(response.error.message);
        return;
      }
      setSettings(response.result as MacAddressSettings);
      notifications.success(m.network_mac_source_saved());
    });
  };

  return (
    <div className="flex items-center justify-between gap-x-8">
      <SettingsItem
        title={m.network_mac_source_title()}
        description={m.network_mac_source_description({
          default_mac: settings.default_mac ?? "",
          espressif_mac: settings.espressif_mac ?? "",
        })}
      />
      <div className="shrink-0">
        <SelectMenuBasic
          id="mac-address-source"
          size="SM"
          aria-label={m.network_mac_source_title()}
          value={settings.source}
          disabled={saving || channel?.readyState !== "open"}
          options={[
            { value: "default", label: m.network_mac_source_default() },
            { value: "espressif", label: m.network_mac_source_espressif() },
          ]}
          onChange={event => save(event.target.value as MacAddressSource)}
        />
      </div>
    </div>
  );
}
