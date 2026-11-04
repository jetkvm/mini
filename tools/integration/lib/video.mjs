// Video freeze helpers, using the e2e suite's freeze rule.
import { MAX_FREEZE_MS, longestFreeze } from '../../../frontend/jetkvm-ui/e2e/helpers/video-freeze.ts';
import { until } from './mini.mjs';

export { MAX_FREEZE_MS };

// Samples decoded frames every 250 ms; while stalled, also checks the OS
// Services sender's frame count (it sends nothing for a static screen).
// stop() returns the longest freeze { ms, from, serviceVideo }; stop.between()
// queries a window; call stop.sessionEnded() before reopening a session you
// ended on purpose, or the gap counts as a freeze.
export function watchVideo(mini) {
  const samples = [];
  const sender = [];
  let reading = false;
  let nextReading = 0;
  const readSender = async () => {
    const from = Date.now();
    const response = await fetch(new URL('/webrtc/stats?bitrate=1', mini.url), { signal: AbortSignal.timeout(2000) });
    const video = response.ok ? (await response.json()).guestVideo : null;
    if (!video) return false;
    sender.push({ from, to: Date.now(), source: video.source, received: video.frames + video.framesDropped + video.framesDiscarded });
    return video.source === 'guest';
  };
  const timer = setInterval(async () => {
    const count = await mini.frames().catch(() => null);
    if (count === null) return;
    const still = samples.at(-1)?.[1] === count;
    samples.push([Date.now(), count]);
    if (!still || reading || Date.now() < nextReading) return;
    reading = true;
    if (!(await readSender().catch(() => false))) nextReading = Date.now() + 1000;
    reading = false;
  }, 250);
  // The watch does not keep the test file's process alive: node --test runs
  // no further t.after hook once one throws, so stop() can stay uncalled.
  timer.unref();
  const stop = () => {
    clearInterval(timer);
    return longestFreeze(samples, 0, Infinity, sender);
  };
  stop.between = (from, to) => longestFreeze(samples, from, to, sender);
  stop.sessionEnded = () => samples.push([Date.now(), -1]);
  return stop;
}

// /webrtc/stats with the page's cookies. 'details=1' adds psramLargest.
export async function webrtcStats(mini, query = '') {
  const response = await mini.page.request.get(new URL(`/webrtc/stats${query && `?${query}`}`, mini.url).href, { timeout: 5000 });
  if (!response.ok()) throw new Error(`/webrtc/stats: HTTP ${response.status()}`);
  return response.json();
}

/**
 * Turns service video off (re-enumerating USB would otherwise flip the source
 * and freeze video) and waits for HDMI capture with a mode stable for 5 s.
 * mini.close() restores the setting.
 */
export async function hdmiCapture(mini) {
  const captures = () => until(() => webrtcStats(mini), stats => stats.modeValid, 45);
  if (!(await mini.rpc('getOsServicesSettings')).video_enabled) return captures();
  const before = await mini.rpc('getVideoState');
  await mini.rpc('setOsServicesSettings', { settings: { video_enabled: false } });
  await until(() => mini.rpc('getVideoState'), state => state.source === 'physical', 45);
  if (before.source === 'physical' && !before.osServices?.connected) return captures();
  let held = null;
  await until(() => webrtcStats(mini), stats => {
    if (!stats.modeValid) held = null;
    else if (held?.epoch !== stats.videoEpoch) held = { epoch: stats.videoEpoch, since: Date.now() };
    return held !== null && Date.now() - held.since >= 5000;
  }, 45);
}

// Resolves once the page decodes a new frame.
export async function decodes(mini, seconds = 15) {
  const before = (await mini.frames()) ?? 0;
  await until(mini.frames, frames => frames > before, seconds);
}
