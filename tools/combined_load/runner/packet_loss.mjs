// packetLoss: the tool's browser loses every packet to and from the Mini for
// DROP_MS, via Chrome's network emulation.
//
// Chrome applies the emulation only to WebRTC sockets opened while one is
// set, and drops it from all sockets when cleared. So prepare() sets NO_LOSS
// before the page loads and it stays set; the loss is ALL_LOST for DROP_MS.
import { sleep } from './driver.mjs';

export const DROP_MS = 100;
// The rate, in bytes per second, that makes Chrome count the emulation as
// set. No session reaches it.
const RATE_OF_NO_LIMIT = 1e9;
export const NO_LOSS = { offline: false, latency: 0, downloadThroughput: RATE_OF_NO_LIMIT,
  uploadThroughput: RATE_OF_NO_LIMIT, packetLoss: 0 };
export const ALL_LOST = { ...NO_LOSS, packetLoss: 100 };

// The browser notices a lost packet when the next one comes, and its report
// is up to 50 ms old.
const NOTICED_WITHIN_MS = 500;

/**
 * The switch of packetLoss.
 *
 *   emulate(conditions)  sets Chrome's network emulation for the tool's page
 *   video()              the browser's counts for the Mini's video so far:
 *                        { framesDecoded, packetsLost, nackCount }, or null
 *                        without a video stream
 *
 * The change to on is the loss. Its switch is done when the loss has ended
 * and the browser has noticed it; the recovery is the state's settling,
 * which the rules judge as for every other change.
 */
export function packetLossSwitch({ emulate, video, now = Date.now, wait = sleep }) {
  let attached = false;
  let lost = false;
  const count = (report, name, before) => Math.max(0, (report?.[name] ?? 0) - (before?.[name] ?? 0));
  return {
    // Before the list's page loads: the page's sockets get the emulation.
    prepare: async () => {
      await emulate(NO_LOSS);
      attached = true;
      return {};
    },
    on: async () => {
      const before = await video();
      if (!before) throw new Error('packetLoss needs the Mini\'s video in the page, and the page has none');
      const from = now();
      await emulate(ALL_LOST);
      lost = true;
      await wait(Math.max(0, DROP_MS - (now() - from)));
      await emulate(NO_LOSS);
      const endedAt = now();
      // A loss that did not reach the session would pass every rule. The
      // browser misses the lost video packets when the next packet comes:
      // it counts them as lost and asks the Mini for them again.
      for (;;) {
        const report = await video();
        if (count(report, 'packetsLost', before) + count(report, 'nackCount', before) > 0) return;
        if (now() - endedAt >= NOTICED_WITHIN_MS) {
          throw new Error(`the page's connection lost no packet: the browser missed no video packet ${NOTICED_WITHIN_MS} ms after ` +
            `Chrome's network emulation was set to lose every packet for ${DROP_MS} ms` +
            (attached ? '' : '; the emulation was not set before the page loaded'));
        }
        await wait(25);
      }
    },
    // Off is the end of the state: the loss itself ended in on(). After a
    // run that was stopped inside the loss, it ends the loss.
    off: async () => {
      lost = false;
      if (attached) await emulate(NO_LOSS);
    },
    isOn: () => lost,
  };
}
