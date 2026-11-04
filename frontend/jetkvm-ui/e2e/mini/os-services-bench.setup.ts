import { readFileSync, writeFileSync } from "node:fs";
import { test, expect, type Page } from "@playwright/test";
import { ensureRpcReady } from "../helpers";
import { skipWithoutRemoteHost } from "../remote-agent/remote-agent";
import {
  BENCH_BRIDGE_SETTINGS,
  FINGERPRINT,
  SENDER_UNIT,
  TargetHost,
  carryApproval,
  connectSender,
  osServicesState,
  restoreOsServices,
  rpcAnswer,
  setBridgeSettings,
  settingsRequest,
  waitForState,
  type OsServicesSettings,
  type OsServicesSnapshot,
} from "./os-services";

// The bench steps between tools/e2e/run.sh's two phases. Phase 1 runs with
// OS Services off: the sender on the remote host is stopped and the Mini has
// no OS Services session. Phase 2 runs with it on: the sender runs, the
// Mini's current key is approved and the OS Services settings are the
// firmware's defaults. run.sh records the bench before the phases and puts it
// back after them, in E2E_OSVC_BENCH_FILE. Each step runs on its own (-g).

const benchFile = (): string => {
  const file = process.env.E2E_OSVC_BENCH_FILE;
  if (!file) throw new Error("E2E_OSVC_BENCH_FILE not set (tools/e2e/run.sh sets it)");
  return file;
};

let target: TargetHost;

test.beforeEach(async ({ page }) => {
  skipWithoutRemoteHost();
  target = TargetHost.fromEnv();
  await ensureRpcReady(page, { timeoutMs: 90_000, navigateFirst: true });
});

/** The Mini's OS Services settings and key. */
async function miniSide(
  page: Page,
): Promise<{ settings: OsServicesSettings; fingerprint: string }> {
  const answer = await rpcAnswer(page, "getOsServicesSettings");
  expect(answer.error, "getOsServicesSettings").toBeUndefined();
  const { fingerprint } = (await osServicesState(page)).osServices;
  expect(fingerprint, "the Mini's fingerprint").toMatch(FINGERPRINT);
  return { settings: answer.result as OsServicesSettings, fingerprint };
}

/** Also skips on a remote host without the sender's unit. */
async function bench(page: Page): Promise<OsServicesSnapshot> {
  const mini = await miniSide(page);
  const unit = await target.unitProperties();
  test.skip(
    unit.LoadState !== "loaded",
    `${target.login} has no ${SENDER_UNIT} user unit or launchd agent (LoadState ${unit.LoadState})`,
  );
  return {
    ...mini,
    approvals: await target.readApprovalList(),
    senderActive: unit.ActiveState === "active",
  };
}

test("bench: record OS Services", async ({ page }) => {
  writeFileSync(benchFile(), JSON.stringify(await bench(page)));
});

test("bench: OS Services off", async ({ page }) => {
  await bench(page);
  await target.sender("stop");
  await waitForState(
    page,
    "no OS Services session, HDMI capture",
    state => !state.osServices.connected && state.source === "physical",
  );
});

test("bench: OS Services on", async ({ page }) => {
  const { fingerprint } = await bench(page);
  await setBridgeSettings(page, settingsRequest(BENCH_BRIDGE_SETTINGS));
  await connectSender(page, target, fingerprint);
});

// run.sh runs it only after a record.
test("bench: restore OS Services", async ({ page }) => {
  const recorded = JSON.parse(readFileSync(benchFile(), "utf8")) as OsServicesSnapshot;
  const { fingerprint } = await miniSide(page);
  // A factory reset in between gave the Mini a new key; it keeps the approval
  // the old one had.
  await restoreOsServices(page, target, {
    ...recorded,
    approvals: carryApproval(recorded.approvals, recorded.fingerprint, fingerprint),
    fingerprint,
  });
});
