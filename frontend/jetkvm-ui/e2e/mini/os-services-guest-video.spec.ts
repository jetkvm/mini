import type { BrowserContext, Page } from "@playwright/test";
import { test, expect } from "../helpers/test";
import { ensureRpcReady } from "../helpers";
import { skipWithoutRemoteHost } from "../remote-agent/remote-agent";
import {
  SWITCH_TIMEOUT,
  TargetHost,
  clearVideoRetryBudget,
  connectSender,
  framesDecoded,
  prepareOsServices,
  recordOsServicesEvents,
  recordedEvents,
  restoreOsServices,
  setBridgeSettings,
  videoError,
  videoRegistrationState,
  waitForGuestVideo,
  waitForState,
  type OsServicesSnapshot,
} from "./os-services";
import { fetchWebrtcStats } from "./ota";

// Service video (OS Services protocol version 5) from the real sender on the remote host: with
// video on in the OS Services settings, the Mini presents the KVM display,
// the sender streams that display's H.264 over the OS Services interface and
// the browser shows it in place of HDMI capture:
//
//   - video on switches the source to guest; getVideoState is ready at the
//     settings' size, the browser decodes frames at that size and
//     /webrtc/stats guestVideo counts frames and keyframes (the sender sends
//     a frame when the desktop changes, so an idle desktop gives few);
//   - the switch's videoInputState events say switching, never no_signal;
//   - a new resolution re-presents the display and the stream follows it;
//   - video off restores HDMI capture.
//
// The remote host's sender must offer service video (osServices.
// videoRegistered); the spec skips otherwise, and fails when the device
// reports a service video fault (osServices.videoError). It puts back the
// OS Services settings, the approved list and the sender's state.

interface GuestVideo {
  source: string;
  frames: number;
  keyframes: number;
  switches: number;
}

async function guestVideo(): Promise<GuestVideo> {
  const stats = (await fetchWebrtcStats("bitrate=1")) as { guestVideo?: GuestVideo | null };
  expect(stats.guestVideo, "/webrtc/stats?bitrate=1 carries guestVideo").toBeTruthy();
  return stats.guestVideo!;
}

/** A keyframe counted after `before`. The device reports the stream ready on
 * the sender's video.status; the first keyframe can follow 200 ms later. */
async function expectKeyframeSince(before: number, message: string): Promise<void> {
  await expect
    .poll(async () => (await guestVideo()).keyframes, { message, timeout: 10_000 })
    .toBeGreaterThan(before);
}

async function expectDecodedSince(page: Page, before: number): Promise<void> {
  await expect
    .poll(() => framesDecoded(page), { message: "the browser decodes frames", timeout: 30_000 })
    .toBeGreaterThan(before);
}

test.describe("IP-KVM OS Services service video with the sender on the remote host", () => {
  test.describe.configure({ mode: "serial" });
  test.setTimeout(240_000);

  let context: BrowserContext | undefined;
  let page: Page;
  let target: TargetHost;
  let snapshot: OsServicesSnapshot | undefined;
  let testStart = 0;
  let initial: GuestVideo;

  test.beforeAll(async ({ browser }) => {
    test.setTimeout(180_000);
    skipWithoutRemoteHost();
    target = TargetHost.fromEnv();
    context = await browser.newContext({ baseURL: process.env.JETKVM_URL });
    page = await context.newPage();
    await recordOsServicesEvents(page);
    await ensureRpcReady(page, { timeoutMs: 90_000, navigateFirst: true });
    snapshot = await prepareOsServices(page, target);
    await connectSender(page, target, snapshot.fingerprint);
    await clearVideoRetryBudget(page);
    const state = await videoRegistrationState(page);
    // A fault also takes the registration away: it fails the spec, never skips it.
    expect(videoError(state), "the device's service video error").toBeUndefined();
    expect(state.osServices.videoRegistered, "the sender offers service video").toBe(true);
    await setBridgeSettings(page, { video_enabled: false, resolution: "1920x1080" });
    await waitForState(page, "HDMI capture", state => state.source === "physical", SWITCH_TIMEOUT);
    initial = await guestVideo();
  });

  test.afterAll(async () => {
    test.setTimeout(180_000);
    try {
      if (snapshot) await restoreOsServices(page, target, snapshot);
    } finally {
      await context?.close();
    }
  });

  test.beforeEach(async () => {
    testStart = await target.now();
  });

  test.afterEach(async () => {
    const testInfo = test.info();
    if (testInfo.status === testInfo.expectedStatus) return;
    const log = await target.senderLog(testStart).catch(() => []);
    await testInfo.attach("sender.log", {
      body: log.map(entry => `${new Date(entry.at).toISOString()} ${entry.message}`).join("\n"),
      contentType: "text/plain",
    });
  });

  test("video on: the browser shows the sender's stream in place of HDMI capture", async () => {
    const since = Date.now();
    const decoded = await framesDecoded(page);
    await setBridgeSettings(page, { video_enabled: true });
    await waitForGuestVideo(page, 1920, 1080);
    await expectDecodedSince(page, decoded);

    const counters = await guestVideo();
    expect(counters.source).toBe("guest");
    expect(counters.switches, "one switch").toBe(initial.switches + 1);
    expect(counters.frames, "frames from the sender").toBeGreaterThan(initial.frames);
    await expectKeyframeSince(initial.keyframes, "the stream starts with a keyframe");

    const events = await recordedEvents(page, "videoInputState", since);
    const switching = events.filter(event => event.params.source === "switching_to_guest");
    expect.soft(switching.length, "a switching_to_guest event").toBeGreaterThan(0);
    expect(
      switching.filter(event => event.params.error !== undefined).map(event => event.params),
      "switching is not no_signal",
    ).toEqual([]);
  });

  test("a new resolution re-presents the display and the stream follows it", async () => {
    await setBridgeSettings(page, { resolution: "1280x720" });
    await waitForGuestVideo(page, 1280, 720);
    await setBridgeSettings(page, { resolution: "3840x2160" });
    await waitForGuestVideo(page, 3840, 2160);
    await setBridgeSettings(page, { resolution: "1920x1080" });
    await waitForGuestVideo(page, 1920, 1080);
    expect((await guestVideo()).switches, "no new switch").toBe(initial.switches + 1);
  });

  test("video off restores HDMI capture", async () => {
    const since = Date.now();
    await setBridgeSettings(page, { video_enabled: false });
    const state = await waitForState(
      page,
      "HDMI capture",
      state => state.source === "physical",
      SWITCH_TIMEOUT,
    );
    expect(state.osServices.video).toBe(false);
    expect(videoError(state), "the device's service video error").toBeUndefined();
    const switching = (await recordedEvents(page, "videoInputState", since)).filter(
      event => event.params.source === "switching_to_physical",
    );
    expect(
      switching.filter(event => event.params.error !== undefined).map(event => event.params),
      "switching is not no_signal",
    ).toEqual([]);
  });
});
