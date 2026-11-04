// Upload rate and remaining time for the image upload view.
//
// The rate is the bytes received over the last RATE_WINDOW_MS, measured up to
// the current time and not up to the last progress reading. A stall therefore
// lowers the rate as the window moves past the last reading, and the rate is
// 0 once no byte arrived for a whole window. Unit test: tests/upload-eta.mjs.

/** One progress reading: the time in ms and the bytes of the file stored so far. */
export type RateSample = { time: number; bytes: number };

/** The span of readings that the rate is measured over. */
export const RATE_WINDOW_MS = 10_000;
/** No rate is given before the readings span this long. */
export const RATE_MIN_SPAN_MS = 3_000;
/** A remaining time longer than this is not shown. */
export const MAX_REMAINING_S = 24 * 60 * 60;

/**
 * Adds a reading and drops the readings that the window no longer needs. The
 * newest reading at or before the window's start stays as its anchor.
 */
export function addRateSample(samples: RateSample[], time: number, bytes: number): void {
  samples.push({ time, bytes });
  const windowStart = time - RATE_WINDOW_MS;
  while (samples.length > 2 && samples[1].time <= windowStart) samples.shift();
}

/** Bytes per second over the window that ends at `now`, or null before RATE_MIN_SPAN_MS. */
export function uploadRate(samples: RateSample[], now: number): number | null {
  if (samples.length === 0 || now - samples[0].time < RATE_MIN_SPAN_MS) return null;
  const windowStart = now - RATE_WINDOW_MS;
  let anchor = samples[0];
  for (const sample of samples) {
    if (sample.time > windowStart) break;
    anchor = sample;
  }
  const last = samples[samples.length - 1];
  return Math.max(0, last.bytes - anchor.bytes) / ((now - anchor.time) / 1000);
}

/**
 * Seconds until `bytesLeft` are sent at `rate`, or null when there is no rate,
 * the rate is 0, every byte is sent (the device still has to answer), or the
 * result is longer than MAX_REMAINING_S.
 */
export function remainingSeconds(rate: number | null, bytesLeft: number): number | null {
  if (rate === null || rate <= 0 || bytesLeft <= 0) return null;
  const seconds = bytesLeft / rate;
  return seconds > MAX_REMAINING_S ? null : seconds;
}

export type RemainingTime =
  | { unit: "s"; seconds: number }
  | { unit: "min"; minutes: number }
  | { unit: "h"; hours: number; minutes: number };

/**
 * The remaining time as the view shows it: seconds rounded up to 5 s below one
 * minute, then whole minutes, then hours and minutes.
 */
export function remainingTime(seconds: number): RemainingTime {
  const fiveSeconds = Math.max(5, Math.ceil(seconds / 5) * 5);
  if (fiveSeconds < 60) return { unit: "s", seconds: fiveSeconds };
  const minutes = Math.max(1, Math.round(seconds / 60));
  if (minutes < 60) return { unit: "min", minutes };
  return { unit: "h", hours: Math.floor(minutes / 60), minutes: minutes % 60 };
}
