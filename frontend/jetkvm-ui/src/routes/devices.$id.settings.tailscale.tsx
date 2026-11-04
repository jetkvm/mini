import { Navigate } from "react-router";

import { LuTriangleAlert } from "react-icons/lu";

import Card from "@components/Card";
import TailscaleCard from "@components/TailscaleCard";
import { SettingsPageHeader } from "@components/SettingsPageheader";
import { useCapability, useDeviceStore } from "@hooks/stores";
import { m } from "@localizations/messages.js";

// Only a device with Tailscale has this page; on any other the page sends its
// visitor to the general settings.
export default function SettingsTailscaleRoute() {
  const tailscale = useCapability("tailscale");
  const reported = useDeviceStore(state => state.capabilities !== null);
  if (reported && !tailscale) return <Navigate to="../general" replace />;
  return (
    <div className="space-y-4">
      <SettingsPageHeader
        title={m.tailscale_title()}
        description={m.tailscale_page_description()}
      />
      <Card>
        <div className="flex items-center gap-3 rounded p-4">
          <LuTriangleAlert className="h-5 w-5 flex-shrink-0 text-amber-600 dark:text-amber-400" />
          <p className="text-sm font-medium text-amber-800 dark:text-amber-200">
            {m.tailscale_experimental()}
          </p>
        </div>
      </Card>
      {tailscale && <TailscaleCard />}
    </div>
  );
}
