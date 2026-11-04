import { writeFileSync } from "node:fs";
import type { Page } from "@playwright/test";
import { test, expect, videoStops } from "../helpers/test";

import {
  callJsonRpc,
  ensureRpcReady,
  getKeysDownState,
  openAccessSettings,
  sendKeypress,
  waitForDecodedFrames,
  waitForWebRTCReady,
  withRpcPage,
} from "../helpers";
import { skipWithoutRemoteHost } from "../remote-agent/remote-agent";
import {
  expectUpdateChecksDuringHttpsImageRead,
  skipWithoutHostReads,
  stopHttpsImageRead,
} from "./https-image-read";
import { brokerHost } from "./mqtt-broker";
import { isMockCloudBuild, startMockCloud, type MockCloud } from "./mock-cloud";
import { UPDATE_CHECK_STOPS_VIDEO, fetchWebrtcStats } from "./ota";
import { useHdmiCapture } from "./os-services";

// The Mini's cloud path against a local mock of JetKVM Cloud (mock-cloud.ts),
// where it needs the page: adoption through the settings page and the cloud's
// pages, a session whose signaling goes through the cloud relay, such a
// session after the cloud dropped the Mini's socket, and update checks beside
// an HTTPS image that the host reads while the cloud socket is open over wss,
// as it is with the real cloud. The Mini must run a build with
// -DJETKVM_MOCK_CLOUD_JWKS_URL; on other firmware the tests are skipped. What
// needs no page is in tools/integration/tests/cloud.test.mjs: the Mini's own
// calls of an adoption, its new socket after a drop, a local session while
// the cloud never answers, an update check beside the cloud socket, and the
// internal RAM that the socket takes.

test.describe.configure({ mode: "serial" });

// Chrome names its host candidates with mDNS names, and the Mini resolves such
// a name only to the address of a local signaling socket. Through JetKVM Cloud
// the browser also has server-reflexive candidates from the cloud's STUN
// servers, which carry an address; this LAN has no STUN server, so the browser
// shows its host addresses instead.
test.use({ launchOptions: { args: ["--disable-features=WebRtcHideLocalIpsWithMdns"] } });

interface CloudState {
  connected: boolean;
  url: string;
  appUrl: string;
}

// Modifiers only, so nothing is typed on the host. keysDownState, the Mini's
// report of the keys it holds on USB, has them in its modifier byte.
const LEFT_SHIFT = { key: 0xe1, bit: 0x02 };

let cloud: MockCloud | undefined;
let before: CloudState | undefined;
let deviceId = "";
let cloudUiVersion = "";

async function setCloudUrl(page: Page, apiUrl: string, appUrl: string): Promise<void> {
  await callJsonRpc(page, "setCloudUrl", { apiUrl, appUrl });
}

// The Mini reports the key held and then released, over this page's session.
async function keyRoundTrip(page: Page): Promise<void> {
  for (const down of [true, false]) {
    await sendKeypress(page, LEFT_SHIFT.key, down);
    await expect
      .poll(async () => (((await getKeysDownState(page))?.modifier ?? 0) & LEFT_SHIFT.bit) !== 0, {
        message: `the Mini reports Left Shift ${down ? "held" : "released"}`,
        timeout: 3000,
      })
      .toBe(down);
  }
}

// A page from the mock cloud runs the Mini's UI, so its signaling socket goes
// to the relay; the offer reaches the Mini over its cloud socket.
async function cloudSession(page: Page): Promise<void> {
  const offers = cloud!.offers;
  await page.goto(cloud!.url + "/");
  await waitForWebRTCReady(page);
  expect(cloud!.offers, "offers relayed to the Mini").toBeGreaterThan(offers);
  expect(cloud!.answers, "answers relayed from the Mini").toBe(cloud!.offers);
  await waitForDecodedFrames(page);
  await keyRoundTrip(page);
}

// Runs `fn` once `page` has an RPC-ready local session.
async function onLocalSession(page: Page, fn: () => Promise<void>): Promise<void> {
  await ensureRpcReady(page, { navigateFirst: true });
  await fn();
}

test.beforeAll(async ({ browser }) => {
  await withRpcPage(browser, async page => {
    const { systemVersion } = (await callJsonRpc(page, "getLocalVersion")) as {
      systemVersion: string;
    };
    test.skip(
      !isMockCloudBuild(systemVersion),
      `firmware ${systemVersion} is not a -DJETKVM_MOCK_CLOUD_JWKS_URL build`,
    );
    cloudUiVersion = `${systemVersion}-mini`;
    before = (await callJsonRpc(page, "getCloudState")) as CloudState;
    test.skip(
      before.connected,
      `the Mini is adopted to ${before.url}; adoption to the mock would replace that`,
    );
    deviceId = (await callJsonRpc(page, "getDeviceID")) as string;
  });
  cloud = await startMockCloud(await brokerHost(process.env.JETKVM_URL!), process.env.JETKVM_URL!);
});

test.afterAll(async ({ browser }) => {
  if (!cloud) return;
  try {
    await withRpcPage(browser, async page => {
      const state = (await callJsonRpc(page, "getCloudState")) as CloudState;
      if (state.connected) {
        // Deregistration reaches the cloud at the Mini's cloud URL.
        await setCloudUrl(page, cloud!.url, cloud!.url);
        await callJsonRpc(page, "deregisterDevice");
      }
      await setCloudUrl(page, before!.url, before!.appUrl);
      expect(await callJsonRpc(page, "getCloudState")).toEqual(before);
    });
  } finally {
    await cloud.close();
  }
});

test.afterEach(async ({ page: _page }, testInfo) => {
  if (cloud && testInfo.status !== testInfo.expectedStatus) {
    const path = testInfo.outputPath("mock-cloud-events.txt");
    writeFileSync(path, cloud.events.join("\n") + "\n");
    await testInfo.attach("mock-cloud-events", { path });
  }
});

test("adoption through the access settings", async ({ page }) => {
  // The page shows the Mini's cloud URLs once its cloud state arrives, and
  // replaces what was typed before then; so the URLs are set first.
  await onLocalSession(page, () => setCloudUrl(page, cloud!.url, cloud!.url));
  await openAccessSettings(page);
  await expect(page.getByPlaceholder("https://api.example.com")).toHaveValue(cloud!.url);
  await expect(page.getByPlaceholder("https://app.example.com")).toHaveValue(cloud!.url);
  await page.getByRole("button", { name: "Adopt KVM to Cloud" }).click();

  // /signup sends the browser to the Mini's /adopt, which registers with the
  // mock and then opens the cloud app's setup page.
  await page.waitForURL(
    `${cloud!.url}/v/${encodeURIComponent(cloudUiVersion)}/devices/${deviceId}/setup`,
    { timeout: 30_000 },
  );
  expect(cloud!.tokenExchanges, "temporary tokens exchanged").toBe(1);
  expect(cloud!.jwksFetches, "key set fetched to check the ID token").toBeGreaterThan(0);

  await page.goto("/");
  await ensureRpcReady(page);
  expect(await callJsonRpc(page, "getCloudState")).toEqual({
    connected: true,
    url: cloud!.url,
    appUrl: cloud!.url,
  });
  // The Mini opens its cloud socket with the token it was given.
  await expect
    .poll(() => cloud!.deviceConnected(), { message: "the Mini's cloud socket", timeout: 30_000 })
    .toBe(true);
  expect(cloud!.deviceConnections[0].appVersion).toBe(cloudUiVersion);
});

test("session through the cloud relay: video and keyboard", async ({ page }) => {
  await cloudSession(page);
});

test("the Mini reconnects after the cloud drops its socket", async ({ page }) => {
  expect(cloud!.deviceConnected(), "the Mini's cloud socket before the drop").toBe(true);
  const connections = cloud!.deviceConnections.length;
  const dropped = Date.now();
  cloud!.dropDevice();
  await expect
    .poll(() => cloud!.deviceConnections.length, {
      message: "a new cloud socket from the Mini",
      timeout: 30_000,
      intervals: [100],
    })
    .toBe(connections + 1);
  console.log(`reconnected ${cloud!.deviceConnections.at(-1)!.at - dropped} ms after the drop`);
  await cloudSession(page);
});

// The real cloud's socket is wss, and its TLS session stays open while the
// Mini runs. The update check takes the same TLS admission as the firmware
// download, so it must still find room with that session open while the host
// reads an HTTPS image. The test moves the Mini to the mock's HTTPS URL, whose
// certificate comes from the test CA that a mock cloud build trusts.
test.describe("with the cloud socket over wss", () => {
  test.afterEach(({ page }) => stopHttpsImageRead(page));

  test.describe("while the host reads an HTTPS image", () => {
    // With a sender connected and service video on, the mount re-enumerates
    // the Mini's USB device, which ends the OS Services session, and the Mini
    // switches to HDMI capture and back to service video. That stops the
    // browser's video for seconds, which the video watch in helpers/test.ts
    // fails, so this test runs with HDMI capture.
    useHdmiCapture();

    test("update checks are admitted while the host reads an HTTPS image", async ({ page }) => {
      videoStops(UPDATE_CHECK_STOPS_VIDEO);
      test.setTimeout(240_000);
      skipWithoutRemoteHost();
      skipWithoutHostReads();
      const connections = cloud!.deviceConnections.length;
      await onLocalSession(page, () => setCloudUrl(page, cloud!.secureUrl, cloud!.url));
      await expect
        .poll(() => cloud!.deviceConnections.length > connections && cloud!.deviceConnected(), {
          message: "a new cloud socket from the Mini",
          timeout: 30_000,
        })
        .toBe(true);
      expect(cloud!.deviceConnections.at(-1)!.secure, "the cloud socket uses TLS").toBe(true);
      await expect
        .poll(async () => (await fetchWebrtcStats()).cloudConnected, {
          message: "/webrtc/stats reports the cloud socket open",
          timeout: 10_000,
        })
        .toBe(true);
      await expectUpdateChecksDuringHttpsImageRead(page);
    });
  });
});
