// Approves the Mini on the host's OS Services sender via its own
// --approve-kvm / --forget-kvm options.
import { execFile } from 'node:child_process';
import { promisify } from 'node:util';
import { quote } from '../../combined_load/runner/hosts.mjs';
import { SSH_OPTIONS } from '../../combined_load/runner/switches.mjs';
import { until } from './mini.mjs';

const run = promisify(execFile);

// Runs the sender registered by --install-service. On Linux, use the user
// manager's XDG_CONFIG_HOME (where the approval list lives), which can differ
// from the SSH session's. On Windows, stop Git's sh rewriting the arguments.
const SENDER = `
case "$(uname -s)" in
Linux)
  export XDG_RUNTIME_DIR="\${XDG_RUNTIME_DIR:-/run/user/$(id -u)}"
  config=$(systemctl --user show-environment | sed -n 's/^XDG_CONFIG_HOME=//p')
  [ -z "$config" ] || export XDG_CONFIG_HOME="$config"
  exe=$(systemctl --user show -p ExecStart --value ipkvm-os-services.service | sed -n 's/^{ path=\\(.*\\) ; argv\\[\\]=.*/\\1/p') ;;
Darwin)
  exe=$(/usr/libexec/PlistBuddy -c 'Print :ProgramArguments:0' "$HOME/Library/LaunchAgents/com.jetkvm.os-services.plist") ;;
*)
  export MSYS_NO_PATHCONV=1 MSYS2_ARG_CONV_EXCL='*'
  exe="$LOCALAPPDATA/IP-KVM OS Services/ipkvm-os-services.exe" ;;
esac
[ -x "$exe" ] || { echo "no installed OS Services sender (\${exe:-not registered as a service})" >&2; exit 1; }
"$exe" "$@"
`;

// option: '--approve-kvm' or '--forget-kvm'. Rejects with the sender's message
// ('--forget-kvm' of an unknown key ends with "is not approved").
export async function sender(mini, option, fingerprint) {
  const command = `sh -c ${quote(SENDER)} sh ${option} ${quote(fingerprint)}`;
  try {
    await run('ssh', [...SSH_OPTIONS, mini.remoteHost, command], { timeout: 30_000 });
  } catch (e) {
    throw new Error(`${mini.remoteHost}: ${option}: ${e.stderr?.trim() || e.message}`);
  }
}

/**
 * Approves the Mini on the running sender if needed and waits for an approved
 * session. Returns { fingerprint, approvedBefore, forget }; forget() undoes
 * only an approval made here and does not close the open session.
 */
export async function approveMini(mini, seconds = 45) {
  const session = async () => (await mini.rpc('getVideoState')).osServices;
  const { fingerprint } = await session();
  // The Mini has the sender's details only once a session stays open.
  const approved = now => now.connected && !now.approvalPending && Boolean(now.sender);
  const found = await until(session, now => now.approvalPending || approved(now), seconds);
  if (approved(found)) return { fingerprint, approvedBefore: true, forget: async () => undefined };
  await sender(mini, '--approve-kvm', fingerprint);
  const forget = () => sender(mini, '--forget-kvm', fingerprint);
  try {
    await until(session, approved, seconds);
  } catch (error) {
    await forget().catch(() => undefined);
    throw new Error(`the sender on ${mini.remoteHost} did not open a session after --approve-kvm ${fingerprint}: ${error.message}`);
  }
  return { fingerprint, approvedBefore: false, forget };
}
