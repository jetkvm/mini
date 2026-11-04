import { LuCopy, LuExternalLink } from "react-icons/lu";
import { useCallback, useEffect, useRef, useState } from "react";

import { Button, LinkButton } from "@components/Button";
import { GridCard } from "@components/Card";
import LoadingSpinner from "@components/LoadingSpinner";
import { Checkbox } from "@components/Checkbox";
import { ConfirmDialog } from "@components/ConfirmDialog";
import { InputFieldWithLabel } from "@components/InputField";
import { NestedSettingsGroup } from "@components/NestedSettingsGroup";
import { SelectMenuBasic } from "@components/SelectMenuBasic";
import { SettingsItem } from "@components/SettingsItem";
import { useCopyToClipboard } from "@components/useCopyToClipBoard";
import { TailscaleStatus } from "@hooks/stores";
import { JsonRpcResponse, useJsonRpc } from "@hooks/useJsonRpc";
import { m } from "@localizations/messages.js";
import notifications from "@/notifications";

const defaultControlURL = "https://controlplane.tailscale.com";
const controlServerModeDefault = "default";
const controlServerModeCustom = "custom";
type ControlServerMode = typeof controlServerModeDefault | typeof controlServerModeCustom;

const errorText = (resp: JsonRpcResponse) =>
  "error" in resp
    ? typeof resp.error.data === "string"
      ? resp.error.data
      : resp.error.message
    : "";

export default function TailscaleCard() {
  const { send } = useJsonRpc();

  const [status, setStatus] = useState<TailscaleStatus | null>(null);
  const [controlURLInput, setControlURLInput] = useState("");
  const [controlServerMode, setControlServerMode] =
    useState<ControlServerMode>(controlServerModeDefault);
  const [isSavingControlURL, setIsSavingControlURL] = useState(false);
  const [isSaving, setIsSaving] = useState(false);
  const [showLogout, setShowLogout] = useState(false);
  // The user has opened the login page; the page waits for the login.
  const [loginOpened, setLoginOpened] = useState(false);
  const [error, setError] = useState<string | null>(null);
  const controlURLDirty = useRef(false);
  const configPending = useRef(false);
  // Counts the settings calls; a status read that started before the last
  // one returns the state before it and is not shown.
  const configCalls = useRef(0);

  const refreshStatus = useCallback(() => {
    const calls = configCalls.current;
    send("getTailscaleStatus", {}, resp => {
      if ("error" in resp) {
        setError(resp.error.message);
        return;
      }
      setError(null);
      if (configPending.current || calls !== configCalls.current) return;
      const nextStatus = resp.result as TailscaleStatus;
      setStatus(nextStatus);
      if (controlURLDirty.current) return;
      const activeControlURL = nextStatus.controlURL ?? defaultControlURL;
      if (activeControlURL === defaultControlURL) {
        setControlServerMode(controlServerModeDefault);
        setControlURLInput("");
      } else {
        setControlServerMode(controlServerModeCustom);
        setControlURLInput(activeControlURL);
      }
    });
  }, [send]);

  useEffect(() => {
    refreshStatus();
    const timer = window.setInterval(refreshStatus, 3000);
    return () => window.clearInterval(timer);
  }, [refreshStatus]);

  const saveControlURL = useCallback(
    (controlURL: string) => {
      setIsSavingControlURL(true);
      configCalls.current++;
      send("setTailscaleControlURL", { controlURL }, resp => {
        setIsSavingControlURL(false);
        if ("error" in resp) {
          notifications.error(m.tailscale_control_server_update_failed({ error: errorText(resp) }));
          return;
        }
        notifications.success(m.tailscale_control_server_update_success());
        controlURLDirty.current = false;
        refreshStatus();
      });
    },
    [refreshStatus, send],
  );

  // Runs one settings call with the switch and the buttons locked, and reads
  // the status again when it ends.
  const run = useCallback(
    (method: string, params: Record<string, unknown>, onDone?: () => void) => {
      configPending.current = true;
      configCalls.current++;
      setIsSaving(true);
      send(method, params, resp => {
        configPending.current = false;
        setIsSaving(false);
        if ("error" in resp) notifications.error(errorText(resp));
        else onDone?.();
        refreshStatus();
      });
    },
    [refreshStatus, send],
  );

  const connectWithAuthKey = (authKey: string, onDone: () => void) =>
    run("setTailscaleConfig", { enabled: true, authKey }, onDone);

  const setEnabled = (enabled: boolean) => {
    setStatus(previous => (previous ? { ...previous, enabled } : previous));
    run("setTailscaleConfig", { enabled });
  };

  const connected = !!status?.running && !!status.self;
  const needsLogin =
    !!status?.enabled && !connected && status.backendState === "NeedsLogin" && !!status.authURL;
  // An expired node key or a refused auth key gets no login URL: a new auth
  // key enrolls the node again.
  const needsAuthKey =
    !!status?.enabled && !connected && status.backendState === "NeedsLogin" && !status.authURL;
  // The Mini asks for a new login URL every few seconds until one is
  // approved, so a new URL does not end the wait; leaving NeedsLogin does.
  useEffect(() => {
    if (!needsLogin) setLoginOpened(false);
  }, [needsLogin]);

  if (status === null || !status.installed) {
    return null;
  }

  // Firmware without loggedIn reports an identity only while it is connected.
  const loggedIn = status.loggedIn ?? connected;

  return (
    <div className="space-y-4">
      <GridCard>
        <div className="space-y-4 p-4 text-black dark:text-white">
          <div className="flex items-center gap-x-2">
            <h3 className="text-base font-bold text-slate-900 dark:text-white">
              {m.tailscale_status()}
            </h3>
            <StatusBadge status={status} />
          </div>

          {connected && status.self ? (
            <ConnectedDetails self={status.self} />
          ) : needsLogin && status.authURL ? (
            <div className="space-y-3">
              {loginOpened ? (
                <Waiting text={m.tailscale_login_waiting()} />
              ) : (
                <p className="text-sm text-slate-600 dark:text-slate-400">
                  {m.tailscale_login_prompt()}
                </p>
              )}
              <div className="flex flex-wrap gap-2">
                <Button
                  size="SM"
                  theme={loginOpened ? "light" : "primary"}
                  type="button"
                  text={loginOpened ? m.tailscale_open_login_again() : m.tailscale_open_login()}
                  TrailingIcon={LuExternalLink}
                  onClick={() => {
                    window.open(status.authURL, "_blank", "noopener,noreferrer");
                    setLoginOpened(true);
                  }}
                />
                <Button
                  size="SM"
                  theme="light"
                  type="button"
                  text={m.tailscale_cancel()}
                  disabled={isSaving}
                  onClick={() => setEnabled(false)}
                />
              </div>
            </div>
          ) : needsAuthKey ? (
            <div className="space-y-3">
              <p className="text-sm text-slate-600 dark:text-slate-400">
                {m.tailscale_needs_auth_key()}
              </p>
              <AuthKeyForm disabled={isSaving} onConnect={connectWithAuthKey} />
              {!loggedIn && (
                <Button
                  size="SM"
                  theme="light"
                  type="button"
                  text={m.tailscale_cancel()}
                  disabled={isSaving}
                  onClick={() => setEnabled(false)}
                />
              )}
            </div>
          ) : status.enabled ? (
            <div className="flex flex-wrap items-center gap-2">
              <Waiting text={`${m.tailscale_connecting()}…`} />
              {!loggedIn && (
                <Button
                  size="SM"
                  theme="light"
                  type="button"
                  text={m.tailscale_cancel()}
                  disabled={isSaving}
                  onClick={() => setEnabled(false)}
                />
              )}
            </div>
          ) : loggedIn ? (
            <p className="text-sm text-slate-600 dark:text-slate-400">{m.tailscale_off()}</p>
          ) : (
            <div className="space-y-3">
              <p className="text-sm text-slate-600 dark:text-slate-400">
                {m.tailscale_login_description()}
              </p>
              <Button
                size="SM"
                theme="primary"
                type="button"
                text={m.tailscale_login()}
                disabled={isSaving}
                onClick={() => run("setTailscaleConfig", { enabled: true, authKey: "" })}
              />
              <Fold title={m.tailscale_use_auth_key()}>
                <AuthKeyForm disabled={isSaving} onConnect={connectWithAuthKey} />
              </Fold>
            </div>
          )}

          {(error || status.health?.length) && (
            <div role="status" className="space-y-1 text-sm text-amber-700 dark:text-amber-400">
              {error && <p>{error}</p>}
              {status.health?.map(message => (
                <p key={message}>{message}</p>
              ))}
            </div>
          )}

          {loggedIn && (
            <div className="space-y-4 border-t border-slate-800/10 pt-4 dark:border-slate-300/20">
              <SettingsItem
                size="SM"
                title={m.tailscale_enable()}
                description={m.tailscale_enabled_description()}
              >
                <Checkbox
                  aria-label={m.tailscale_enable()}
                  checked={status.enabled ?? false}
                  disabled={isSaving}
                  onChange={e => setEnabled(e.target.checked)}
                />
              </SettingsItem>
              <SettingsItem
                size="SM"
                title={m.tailscale_logout()}
                description={m.tailscale_logout_description()}
              >
                <Button
                  size="SM"
                  theme="light"
                  type="button"
                  text={m.tailscale_logout()}
                  disabled={isSaving}
                  onClick={() => setShowLogout(true)}
                />
              </SettingsItem>
            </div>
          )}
        </div>
      </GridCard>

      <Fold title={m.tailscale_advanced()}>
        <div className="space-y-4">
          <SettingsItem
            size="SM"
            title={m.tailscale_control_server_title()}
            description={m.tailscale_control_server_description()}
          >
            <SelectMenuBasic
              size="XS"
              label=""
              value={controlServerMode}
              disabled={isSavingControlURL}
              onChange={(e: React.ChangeEvent<HTMLSelectElement>) => {
                const mode = e.target.value as ControlServerMode;
                setControlServerMode(mode);
                controlURLDirty.current = mode === controlServerModeCustom;
                // Default has no field to edit, so it is saved when picked.
                if (
                  mode === controlServerModeDefault &&
                  (status.controlURL ?? defaultControlURL) !== defaultControlURL
                ) {
                  saveControlURL("");
                }
              }}
              options={[
                { value: controlServerModeDefault, label: m.tailscale_control_server_default() },
                { value: controlServerModeCustom, label: m.tailscale_control_server_custom() },
              ]}
            />
          </SettingsItem>
          {controlServerMode === controlServerModeCustom && (
            <NestedSettingsGroup>
              <InputFieldWithLabel
                id="tailscale-control-url"
                size="SM"
                label={m.tailscale_control_server_custom_url_label()}
                placeholder={m.tailscale_control_server_custom_url_placeholder()}
                value={controlURLInput}
                onChange={e => {
                  controlURLDirty.current = true;
                  setControlURLInput(e.target.value);
                }}
              />
              <Button
                size="SM"
                theme="primary"
                type="button"
                text={isSavingControlURL ? m.tailscale_saving() : m.tailscale_save()}
                disabled={isSavingControlURL || !controlURLInput.trim()}
                onClick={() => saveControlURL(controlURLInput.trim())}
              />
            </NestedSettingsGroup>
          )}
        </div>
      </Fold>

      <ConfirmDialog
        open={showLogout}
        onClose={() => setShowLogout(false)}
        title={m.tailscale_logout_dialog_title()}
        description={m.tailscale_logout_dialog_description()}
        variant="danger"
        confirmText={m.tailscale_logout()}
        isConfirming={isSaving}
        onConfirm={() =>
          run("logoutTailscale", {}, () => {
            setShowLogout(false);
            notifications.success(m.tailscale_logged_out());
          })
        }
      />
    </div>
  );
}

function AuthKeyForm({
  disabled,
  onConnect,
}: {
  disabled: boolean;
  onConnect: (authKey: string, onDone: () => void) => void;
}) {
  const [authKey, setAuthKey] = useState("");
  return (
    <NestedSettingsGroup>
      <InputFieldWithLabel
        id="tailscale-auth-key"
        aria-label={m.tailscale_auth_key()}
        size="SM"
        type="password"
        autoComplete="off"
        label={m.tailscale_auth_key()}
        description={m.tailscale_auth_key_hint()}
        placeholder="tskey-auth-..."
        value={authKey}
        onChange={e => setAuthKey(e.target.value)}
      />
      <Button
        size="SM"
        theme="primary"
        type="button"
        text={m.tailscale_connect()}
        disabled={disabled || !authKey.trim()}
        onClick={() => onConnect(authKey.trim(), () => setAuthKey(""))}
      />
    </NestedSettingsGroup>
  );
}

function Waiting({ text }: { text: string }) {
  return (
    <div role="status" className="flex items-center gap-x-2">
      <LoadingSpinner className="h-4 w-4 shrink-0 text-blue-700 dark:text-blue-400" />
      <p className="text-sm text-slate-700 dark:text-slate-300">{text}</p>
    </div>
  );
}

function Fold({ title, children }: { title: string; children: React.ReactNode }) {
  return (
    <details>
      <summary className="cursor-pointer text-sm font-medium text-slate-700 select-none dark:text-slate-300">
        {title}
      </summary>
      <div className="pt-3">{children}</div>
    </details>
  );
}

function ConnectedDetails({ self }: { self: NonNullable<TailscaleStatus["self"]> }) {
  const { copy } = useCopyToClipboard();
  const ipv4 = self.tailscaleIPs?.find(ip => !ip.includes(":"));
  const ipv6 = self.tailscaleIPs?.find(ip => ip.includes(":"));
  const dnsName = self.dnsName?.replace(/\.$/, "");
  const rows = [
    { label: m.tailscale_hostname(), value: self.hostName },
    { label: m.tailscale_ipv4(), value: ipv4 },
    { label: m.tailscale_ipv6(), value: ipv6 },
    { label: m.tailscale_dns_name(), value: dnsName },
  ].filter((row): row is typeof row & { value: string } => !!row.value);

  return (
    <div className="space-y-3">
      <div className="divide-y divide-slate-800/10 dark:divide-slate-300/20">
        {rows.map(row => (
          <div key={row.label} className="flex items-center justify-between gap-x-2 py-1.5">
            <span className="text-sm text-slate-600 dark:text-slate-400">{row.label}</span>
            <span className="flex min-w-0 items-center gap-x-1">
              <span className="truncate font-mono text-[13px] font-medium select-all">
                {row.value}
              </span>
              <Button
                size="XS"
                theme="blank"
                type="button"
                aria-label={m.tailscale_copy({ value: row.label })}
                LeadingIcon={LuCopy}
                onClick={async () => {
                  if (await copy(row.value)) {
                    notifications.success(m.tailscale_copied({ value: row.value }));
                  } else {
                    notifications.error(m.tailscale_copy_failed());
                  }
                }}
              />
            </span>
          </div>
        ))}
      </div>
      {dnsName && (
        <LinkButton
          size="SM"
          theme="light"
          to={`http://${dnsName}`}
          target="_blank"
          text={m.tailscale_open_kvm()}
          TrailingIcon={LuExternalLink}
        />
      )}
    </div>
  );
}

function StatusBadge({ status }: { status: TailscaleStatus }) {
  if (status.running) {
    return (
      <span className="rounded-full bg-green-100 px-2 py-0.5 text-xs font-medium text-green-700 dark:bg-green-900/30 dark:text-green-400">
        {m.tailscale_connected()}
      </span>
    );
  }
  if (status.enabled && status.backendState === "NeedsLogin") {
    return (
      <span className="rounded-full bg-amber-100 px-2 py-0.5 text-xs font-medium text-amber-700 dark:bg-amber-900/30 dark:text-amber-400">
        {m.tailscale_needs_login()}
      </span>
    );
  }
  return (
    <span className="rounded-full bg-slate-100 px-2 py-0.5 text-xs font-medium text-slate-600 dark:bg-slate-700 dark:text-slate-400">
      {status.enabled ? m.tailscale_connecting() : m.tailscale_stopped()}
    </span>
  );
}
