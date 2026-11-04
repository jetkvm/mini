// The freeze rule of the video watch (helpers/test.ts), apart from Playwright
// so that video-freeze.unit.ts can run it with `node --test`.

/** A test fails when its video shows no new frame for longer than this. */
export const MAX_FREEZE_MS = 1000;

/** [Date.now(), framesDecoded]. A count of -1 marks the end of a document. */
export type Sample = [number, number];

/**
 * One reading of the Mini's /webrtc/stats guestVideo, requested at `from` and
 * answered at `to` (Date.now()). `received` counts every frame the Mini read
 * from the OS Services sender: passed to the viewer, dropped or discarded.
 */
export interface SenderSample {
  from: number;
  to: number;
  source: string;
  received: number;
}

export interface Freeze {
  ms: number;
  from: number;
  /** Measured from a frame the Mini received from the sender, not from the browser's last frame. */
  serviceVideo: boolean;
}

const NONE: Freeze = { ms: 0, from: 0, serviceVideo: false };

/**
 * How long the browser showed no new frame in the gap from its frame at
 * `start` to the sample at `end`.
 *
 * With HDMI capture the Mini encodes at a fixed rate, so the whole gap is a
 * freeze. The OS Services sender sends a frame only when the guest's screen
 * changes, so in service video a reading with an unchanged count shows an
 * unchanged screen, up to the time the Mini answered that reading. The gap is
 * service video when it holds a reading and every reading in it shows the
 * source "guest". Its freeze is the longer of these two times:
 * - the longest time without a reading, from the gap's start, so that a
 *   Mini that stops answering is not taken for an unchanged screen;
 * - from the first reading whose count differs from the reading before it,
 *   when the Mini had a frame from the sender that the browser did not show,
 *   to the gap's end.
 * A frame that came before the gap's first reading counts only when another
 * one follows it.
 */
export function gapFreeze(start: number, end: number, sender: SenderSample[]): Freeze {
  const inside = sender.filter(s => s.from >= start && s.to <= end);
  if (inside.length === 0 || inside.some(s => s.source !== "guest"))
    return { ms: end - start, from: start, serviceVideo: false };
  let worst = NONE;
  let shown = start;
  for (const [i, reading] of inside.entries()) {
    if (reading.to - shown > worst.ms)
      worst = { ms: reading.to - shown, from: shown, serviceVideo: false };
    if (i > 0 && reading.received !== inside[i - 1].received) {
      const frozen = { ms: end - reading.to, from: reading.to, serviceVideo: true };
      return frozen.ms > worst.ms ? frozen : worst;
    }
    shown = reading.to;
  }
  return end - shown > worst.ms ? { ms: end - shown, from: shown, serviceVideo: false } : worst;
}

/**
 * The longest freeze (gapFreeze) between [from, to], up to the last sample.
 * A sample shows a new frame when its count is above the previous sample's,
 * or above zero in the first sample of a document (after the -1 marker) or
 * of a new peer connection (framesDecoded going down). The count starts at
 * the first sample in the window that shows a new frame, so a stall left
 * over from before the window does not count, and a reload or reconnect is
 * not a freeze unless frames stop after it.
 */
export function longestFreeze(
  samples: Sample[],
  from = 0,
  to = Infinity,
  sender: SenderSample[] = [],
): Freeze {
  let worst = NONE;
  // The open gap runs from the last new frame to the latest sample.
  let gapStart: number | undefined;
  let latest = 0;
  let lastCount: number | undefined;
  const close = (end: number) => {
    if (gapStart === undefined) return;
    const freeze = gapFreeze(gapStart, end, sender);
    if (freeze.ms > worst.ms) worst = freeze;
    gapStart = undefined;
  };
  for (const [time, count] of samples) {
    if (time > to) break;
    if (count < 0) {
      close(latest);
      lastCount = undefined;
      continue;
    }
    // The first sample of a document or of a new peer connection.
    const fresh = lastCount === undefined || count < lastCount;
    if (time >= from) {
      if (fresh) close(latest);
      if (fresh ? count > 0 : count > lastCount!) {
        close(time);
        gapStart = time;
      }
      latest = time;
    }
    lastCount = count;
  }
  close(latest);
  return worst;
}
