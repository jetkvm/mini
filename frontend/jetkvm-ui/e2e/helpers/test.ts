// Every spec imports `test` from here. While a test runs, its page records the
// browser's decoded-frame count, and the test fails if the video stops for
// more than MAX_FREEZE_MS. The check reads the browser's own WebRTC stats,
// which send nothing to the device and add no test time. Only while no new
// frame shows does it also read the Mini's service video counters
// (video-freeze.ts tells an unchanged guest screen from a freeze with them).
import { test as base, expect, type Page, type TestInfo } from "@playwright/test";
import { MAX_FREEZE_MS, longestFreeze, type Sample, type SenderSample } from "./video-freeze";

const SAMPLE_INTERVAL_MS = 250;
// After a reading without service video, or without an answer, the next one
// waits this long: only service video needs the readings.
const SENDER_IDLE_MS = 1000;
const VIDEO_STOPS = "video-stops";

declare global {
  interface Window {
    __videoWatch?: Sample[];
    __videoWatchSender?: () => Promise<void>;
  }
}

/**
 * Opt the running test out, for example `videoStops("reboots the device")` at
 * the start of its body. A whole file or describe block opts out with
 * `test.use({ videoWatch: false })`.
 */
export function videoStops(reason: string): void {
  base.info().annotations.push({ type: VIDEO_STOPS, description: reason });
}

const senderSamples = new WeakMap<Page, SenderSample[]>();

/**
 * Read the Mini's guestVideo counters into `into`. /webrtc/stats?bitrate=1 is
 * the short document; a device without OS Services or without an answer adds
 * nothing.
 */
async function readSender(into: SenderSample[]): Promise<boolean> {
  const from = Date.now();
  try {
    const res = await fetch(`${process.env.JETKVM_URL}/webrtc/stats?bitrate=1`, {
      signal: AbortSignal.timeout(2000),
    });
    if (!res.ok) return false;
    const { guestVideo: video } = (await res.json()) as {
      guestVideo?: {
        source: string;
        frames: number;
        framesDropped: number;
        framesDiscarded: number;
      } | null;
    };
    if (!video) return false;
    into.push({
      from,
      to: Date.now(),
      source: video.source,
      received: video.frames + video.framesDropped + video.framesDiscarded,
    });
    return video.source === "guest";
  } catch {
    return false;
  }
}

/**
 * Record the decoded-frame count in every document the page loads from now
 * on, and the Mini's service video counters while the count does not change.
 */
export async function watchVideo(page: Page): Promise<void> {
  const sender: SenderSample[] = [];
  senderSamples.set(page, sender);
  let next = 0;
  await page.exposeBinding("__videoWatchSender", async () => {
    if (Date.now() < next) return;
    if (!(await readSender(sender))) next = Date.now() + SENDER_IDLE_MS;
  });
  await page.addInitScript(intervalMs => {
    const samples: Sample[] = (window.__videoWatch = []);
    let reading = false;
    setInterval(async () => {
      const stats = await window.__kvmTestHooks?.getInboundVideoStats?.();
      if (!stats) return;
      const still = samples.at(-1)?.[1] === stats.framesDecoded;
      samples.push([Date.now(), stats.framesDecoded]);
      // One reading at a time, and none while frames come.
      if (still && !reading && window.__videoWatchSender) {
        reading = true;
        const done = () => {
          reading = false;
        };
        window.__videoWatchSender().then(done, done);
      }
    }, intervalMs);
    // A navigation keeps this document's samples in the tab's session storage,
    // closed by a marker (count -1) after which the count starts again.
    addEventListener("pagehide", () => {
      const earlier: Sample[] = JSON.parse(sessionStorage.getItem("__videoWatch") ?? "[]");
      const all = [...earlier, ...samples, [Date.now(), -1]];
      sessionStorage.setItem("__videoWatch", JSON.stringify(all));
    });
  }, SAMPLE_INTERVAL_MS);
}

/** Fail if the page's video froze for more than MAX_FREEZE_MS between from and to. */
export async function expectNoVideoFreeze(page: Page, from?: number, to?: number): Promise<void> {
  if (page.isClosed()) return;
  // A navigation in progress has no samples to read yet.
  const samples: Sample[] = await page
    .evaluate(() => [
      ...JSON.parse(sessionStorage.getItem("__videoWatch") ?? "[]"),
      ...(window.__videoWatch ?? []),
    ])
    .catch(() => []);
  const freeze = longestFreeze(samples, from, to, senderSamples.get(page));
  if (freeze.ms > MAX_FREEZE_MS)
    throw new Error(
      `video froze for ${freeze.ms} ms during the test, from ${new Date(freeze.from).toISOString()}` +
        (freeze.serviceVideo
          ? ", while the Mini had service video frames the browser did not show"
          : ""),
    );
}

/** Whether a finished test gets the check: not skipped and not opted out. */
export function videoWatched(enabled: boolean, testInfo: TestInfo): boolean {
  return (
    enabled &&
    testInfo.status !== "skipped" &&
    !testInfo.annotations.some(a => a.type === VIDEO_STOPS)
  );
}

export const test = base.extend<{ videoWatch: boolean }>({
  videoWatch: [true, { option: true }],
  page: async ({ page, videoWatch }, use, testInfo) => {
    if (videoWatch) await watchVideo(page);
    await use(page);
    if (videoWatched(videoWatch, testInfo)) await expectNoVideoFreeze(page);
  },
});

export { expect };
