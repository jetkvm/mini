// OS Services session helpers for the sender on the bench's host.
import { approveMini } from './approval.mjs';
import { until } from './mini.mjs';

export const videoState = mini => mini.rpc('getVideoState');

export const connected = state => state.osServices?.connected === true && !state.osServices.approvalPending;

// The sender's details arrive in a separate message from the clipboard offer.
export const connectedWithClipboard = state =>
  connected(state) && state.guestClipboard !== undefined && Boolean(state.osServices.sender?.version);

// The raw answer, { result } or { error: { code, message, data } }.
export const rpcAnswer = (mini, method, params = {}, timeoutMs = 10_000) =>
  mini.page.evaluate(([m, p, timeout]) => new Promise(resolve => {
    window.__kvmTestHooks.sendJsonRpc(m, p, resolve, timeout);
  }), [method, params, timeoutMs]);

// Calls window.__kvmTestHooks[name] in the page.
export const hook = (mini, name, ...parameters) =>
  mini.page.evaluate(([n, p]) => window.__kvmTestHooks[n](...p), [name, parameters]);

/**
 * Starts the sender, approves the Mini and enables clipboard sharing. Returns
 * { fingerprint, state, restore }; restore() puts the sender's approval list
 * and running state back.
 */
export async function openSession(mini, host) {
  const service = await host.sender();
  if (!service.installed) throw new Error(`${host.login} has no OS Services sender installed`);
  let approval;
  const restore = async () => {
    await approval?.forget();
    // --forget-kvm does not close an open session; restart the sender.
    const forgotten = approval?.approvedBefore === false;
    if (!service.running || forgotten) await host.exec(host.commands.stopSender, { timeoutMs: 60_000 });
    if (service.running && forgotten) await host.exec(host.commands.startSender, { timeoutMs: 60_000 });
  };
  try {
    if (!service.running) await host.exec(host.commands.startSender, { timeoutMs: 60_000 });
    approval = await approveMini(mini);
    await mini.rpc('setOsServicesSettings', { settings: { clipboard_enabled: true } });
    const state = await until(() => videoState(mini), connectedWithClipboard, 45);
    return { fingerprint: approval.fingerprint, state, restore };
  } catch (error) {
    await restore().catch(() => undefined);
    throw error;
  }
}
