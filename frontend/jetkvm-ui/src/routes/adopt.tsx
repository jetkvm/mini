import { redirect } from "react-router";
import type { LoaderFunction, LoaderFunctionArgs } from "react-router";
import semver from "semver";

import { DEVICE_API } from "@/ui.config";
import api from "@/api";

export interface CloudState {
  connected: boolean;
  url: string;
  appUrl: string;
}

const loader: LoaderFunction = async ({ request }: LoaderFunctionArgs) => {
  const url = new URL(request.url);
  const searchParams = url.searchParams;

  const tempToken = searchParams.get("tempToken");
  const deviceId = searchParams.get("deviceId");
  const oidcGoogle = searchParams.get("oidcGoogle");
  const clientId = searchParams.get("clientId");

  const [cloudStateResponse, registerResponse] = await Promise.all([
    api.GET(`${DEVICE_API}/cloud/state`),
    api.POST(`${DEVICE_API}/cloud/register`, {
      token: tempToken,
      oidcGoogle,
      clientId,
    }),
  ]);

  if (!cloudStateResponse.ok) throw new Error("Failed to get cloud state");
  const cloudState = (await cloudStateResponse.json()) as CloudState;

  if (!registerResponse.ok) throw new Error("Failed to register device");

  // Older devices return only a success message. The Mini also reports the
  // bundle it advertises to cloud, so naming starts in that versioned UI.
  const registration = await registerResponse.json().catch(() => null);
  const version = registration?.cloudUiVersion;
  if (version !== undefined && (typeof version !== "string" || !semver.valid(version))) {
    throw new Error("Invalid cloud UI version in registration response");
  }
  const prefix = version ? `/v/${encodeURIComponent(version)}` : "";
  return redirect(cloudState.appUrl + `${prefix}/devices/${deviceId}/setup`);
};

export default function AdoptRoute() {
  return <></>;
}

AdoptRoute.loader = loader;
