import { remoteHostExec } from "../remote-agent/shared";

/**
 * Whether the shell condition `check` holds on the remote host
 * (JETKVM_REMOTE_HOST). A failed SSH login throws, so an unreachable host
 * fails the test rather than skipping it.
 */
export function remoteHostCan(check: string): boolean {
  return remoteHostExec(`if ${check}; then echo yes; else echo no; fi`).trim() === "yes";
}
