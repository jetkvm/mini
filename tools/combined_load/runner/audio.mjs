// What the tool's browser hears of the Mini's sound, for the variable `audio`.
//
// The target page on the host plays a tone without end and a beep of
// 1000 Hz, 100 ms long, on each whole second of its clock, into the Mini's
// USB audio output (../target_page.html). The Mini sends that sound to the
// browser as Opus. In the tool's page, hear() plays the Mini's audio track
// as the UI does, through an audio element, reads the browser's report of
// the track, and finds the start of each beep in the sound. The page's
// clock, the host's clock and this machine's clock then give the time that
// a beep took from the host's output to the browser.

// The time from one beep of the target page to the next.
const BEEP_PERIOD_MS = 1000;

/**
 * Finds the starts of the beeps in a sound. Returns a function that takes
 * the next samples (-1 to 1) and returns the index of the first sample of
 * each block in which a beep started, counted from the first sample it ever
 * got.
 *
 * It judges blocks of 5 ms, which is five periods of the beep: the block is
 * a beep when it is not silent and most of what is in it is at 1000 Hz (the
 * amplitude at 1000 Hz, by the Goertzel algorithm, is at least the block's
 * RMS; for a sine alone it is 1.41 times the RMS, and for the page's tone
 * alone under 0.3 times). That compares the block with itself, so the
 * volume that the host has set for the Mini's output does not matter. A
 * beep starts at a beep block that follows 300 ms without one. The start is
 * therefore known to 5 ms.
 *
 * The function uses nothing outside itself: the tool gives its source to
 * the page.
 */
export function beepDetector(sampleRate) {
  const block = Math.round(sampleRate * 0.005);
  const quietBlocks = 60;
  const coefficient = 2 * Math.cos(2 * Math.PI * 1000 / sampleRate);
  let s1 = 0;
  let s2 = 0;
  let energy = 0;
  let filled = 0;
  let index = 0;
  let sinceBeep = quietBlocks;
  return samples => {
    const starts = [];
    for (let i = 0; i < samples.length; i++) {
      const x = samples[i];
      const s0 = x + coefficient * s1 - s2;
      s2 = s1;
      s1 = s0;
      energy += x * x;
      index++;
      if (++filled < block) continue;
      const amplitude = 2 * Math.sqrt(Math.max(0, s1 * s1 + s2 * s2 - coefficient * s1 * s2)) / block;
      const rms = Math.sqrt(energy / block);
      if (rms > 1e-4 && amplitude >= rms) {
        if (sinceBeep >= quietBlocks) starts.push(index - block);
        sinceBeep = 0;
      } else {
        sinceBeep++;
      }
      s1 = s2 = energy = filled = 0;
    }
    return starts;
  };
}

// The windows in which toneFinder looks for the strongest frequency: 4096
// samples, which is 85 ms and bins of 11.7 Hz at 48 kHz, one window in
// every four, so three per second. The page's beep is 100 ms in a second
// and so in one of them at most.
const TONE_WINDOW = 4096;
const TONE_EVERY = 4;

/**
 * Finds the strongest frequency of a sound. Returns a function that takes
 * the next samples (-1 to 1) and returns the strongest frequency, in Hz, of
 * each window that these samples completed: the bin of a 4096-point
 * transform between 50 and 2000 Hz with the largest amplitude (by the
 * Goertzel algorithm, one bin at a time), or null for a silent window.
 *
 * The function uses nothing outside itself but its two numbers, which
 * hearScript gives it: the tool gives its source to the page.
 */
export function toneFinder(sampleRate, size = TONE_WINDOW, every = TONE_EVERY) {
  const window = new Float32Array(size);
  let filled = 0;
  let windows = 0;
  const first = Math.ceil(50 * size / sampleRate);
  const last = Math.floor(2000 * size / sampleRate);
  const strongest = () => {
    let energy = 0;
    for (let i = 0; i < size; i++) energy += window[i] * window[i];
    if (Math.sqrt(energy / size) <= 1e-4) return null;
    let best = first;
    let power = -1;
    for (let bin = first; bin <= last; bin++) {
      const coefficient = 2 * Math.cos(2 * Math.PI * bin / size);
      let s1 = 0;
      let s2 = 0;
      for (let i = 0; i < size; i++) {
        const s0 = window[i] + coefficient * s1 - s2;
        s2 = s1;
        s1 = s0;
      }
      const now = s1 * s1 + s2 * s2 - coefficient * s1 * s2;
      if (now > power) {
        power = now;
        best = bin;
      }
    }
    return best * sampleRate / size;
  };
  return samples => {
    const found = [];
    for (let i = 0; i < samples.length; i++) {
      window[filled++] = samples[i];
      if (filled < size) continue;
      filled = 0;
      if (windows++ % every === 0) found.push(strongest());
    }
    return found;
  };
}

/**
 * Whether one of these media elements plays the track with sound: it is not
 * paused, not muted, and the track is in its stream.
 *
 * A browser plays a remote track at the sum of the volumes of the elements
 * that play it, so two elements give twice the level. The UI has an audio
 * element of its own for the track when it found audio enabled as its
 * session connected, which is also when the session comes back during a
 * list. hear() mutes the tool's element for as long as the UI's plays.
 *
 * The function uses nothing outside itself: the tool gives its source to
 * the page.
 */
export const playsTrack = (elements, track) => elements.some(element =>
  !element.paused && !element.muted && Boolean(element.srcObject?.getAudioTracks?.().includes(track)));

/**
 * In the page: what the browser has of the Mini's audio track, or null
 * without a track. The first call for a track plays it through an audio
 * element, as the UI does for a page that was loaded with audio enabled,
 * and passes it to makeDetector's detector and to makeTone's finder of the
 * strongest frequency. Each call mutes that element
 * when `played`, which is playsTrack, finds an element of the document that
 * plays the track. Returns the fields of the
 * browser's inbound audio report that the tool records, and as `beeps` the
 * times, on this machine's clock in ms, at which the beeps started that
 * were heard since the last call with take; without take the beeps stay for
 * the next call. tones has the strongest frequency of each window with
 * sound since the last call with take (toneFinder); without take it is
 * empty, and the windows stay for the next call. A second has two or three
 * windows, of which the beep is in one at most.
 *
 * The time of a sample: the browser hands the sound over in blocks, each
 * with its place on the audio clock, and a call for a block comes after the
 * block's last sample, late by what the page's main thread is busy with and
 * never early. So of the calls of the last 10 s, the one that puts the
 * first sample earliest is the least late, and it gives the samples their
 * times.
 */
export const hear = async (makeDetector, played, take, makeTone) => {
  const pc = window.__kvmTestHooks?._getPeerConnection?.();
  const track = pc?.getReceivers().map(receiver => receiver.track).find(candidate => candidate?.kind === 'audio');
  if (!track) return null;
  let heard = window.__combinedLoadAudio;
  if (heard?.track !== track) {
    heard?.close();
    const stream = new MediaStream([track]);
    const element = new Audio();
    element.srcObject = stream;
    element.play().catch(() => undefined);
    const context = new AudioContext();
    const source = context.createMediaStreamSource(stream);
    // The output of the tap stays silent; a tap runs only when it leads to the output.
    const tap = context.createScriptProcessor(1024, 1, 1);
    const detect = makeDetector(context.sampleRate);
    const tone = makeTone(context.sampleRate);
    const state = heard = window.__combinedLoadAudio = {
      track, element, beeps: [], tones: [], zero: null, given: 0, calls: [],
      close: () => {
        element.srcObject = null;
        context.close().catch(() => undefined);
      },
    };
    const ms = samples => samples * 1000 / context.sampleRate;
    tap.onaudioprocess = event => {
      const input = event.inputBuffer.getChannelData(0);
      const now = performance.timeOrigin + performance.now();
      // The end of this block in the sound, in ms from the first block's
      // start, on the audio clock: a block that the browser left out
      // because the main thread was busy is still counted.
      state.zero ??= event.playbackTime;
      const end = (event.playbackTime - state.zero) * 1000 + ms(input.length);
      // When the first sample was, by this call.
      state.calls.push({ at: now, first: now - end });
      while (now - state.calls[0].at > 10_000) state.calls.shift();
      state.tones.push(...tone(input).filter(hz => hz !== null));
      const starts = detect(input);
      state.given += input.length;
      if (starts.length === 0) return;
      const first = Math.min(...state.calls.map(call => call.first));
      for (const start of starts) state.beeps.push(first + end - ms(state.given - start));
    };
    source.connect(tap);
    tap.connect(context.destination);
  }
  // The track plays once: the tool's element is not in the document, and it
  // is muted while an element of the UI plays the track.
  heard.element.muted = played([...document.querySelectorAll('audio, video')], track);
  let report = null;
  (await pc.getStats()).forEach(entry => {
    if (entry.type === 'inbound-rtp' && entry.kind === 'audio') report = entry;
  });
  if (!report) return null;
  const { packetsReceived, packetsLost, packetsDiscarded, jitter, totalSamplesReceived, concealedSamples,
    silentConcealedSamples, concealmentEvents, insertedSamplesForDeceleration, removedSamplesForAcceleration,
    totalAudioEnergy, totalSamplesDuration, jitterBufferDelay, jitterBufferEmittedCount, timestamp } = report;
  return {
    packetsReceived, packetsLost, packetsDiscarded, jitter, totalSamplesReceived, concealedSamples,
    silentConcealedSamples, concealmentEvents, insertedSamplesForDeceleration, removedSamplesForAcceleration,
    totalAudioEnergy, totalSamplesDuration, jitterBufferDelay, jitterBufferEmittedCount, timestamp,
    beeps: take ? heard.beeps.splice(0) : [],
    tones: take ? heard.tones.splice(0) : [],
  };
};

/**
 * The strongest frequency of a second of sound, in Hz, from the strongest
 * frequency of each of its windows (toneFinder): the middle one, the lower
 * one of two. The beep's frequency is the higher one, and it is in one
 * window at most. undefined without a window.
 */
export const middleTone = tones => (tones?.length ? [...tones].sort((a, b) => a - b)[Math.floor((tones.length - 1) / 2)] : undefined);

/** hear() as a script for the page. */
export const hearScript = take => `(${hear})(${beepDetector}, ${playsTrack}, ${Boolean(take)}, rate => (${toneFinder})(rate, ${TONE_WINDOW}, ${TONE_EVERY}))`;

/**
 * The target page's sound for the list. A browser moves the sound of an
 * output that has left to the output that the host has selected, so the
 * page plays only while nothing takes the Mini off USB.
 *
 * want(on) tells the page whether to play. playing() is what the page
 * reports: true, false, or null without a report of the last seconds.
 *
 *   turn(on)       the list's wish: audio on or off
 *   quiet()        before a change that takes the Mini off USB: returns
 *                  when the page has reported its sound off, and throws
 *                  when it has not after waitMs, so that the change does
 *                  not take place
 *   resume()       after that change: the page gets the list's wish again
 *   offUsb(change) quiet(), the change, resume(); returns what the change
 *                  returns
 */
export function pageSound({ want, playing, waitMs = 5000, pollMs = 20 }) {
  let listening = false;
  const resume = () => want(listening);
  const quiet = async () => {
    if (!listening) return;
    want(false);
    const end = Date.now() + waitMs;
    while (playing() !== false) {
      if (Date.now() > end) {
        resume();
        throw new Error(`the target page has not reported its sound off after ${waitMs / 1000} s, so the Mini stays on USB`);
      }
      await new Promise(resolve => setTimeout(resolve, pollMs));
    }
  };
  return {
    turn: on => {
      listening = on;
      want(on);
    },
    quiet,
    resume,
    offUsb: async change => {
      await quiet();
      try {
        return await change();
      } finally {
        resume();
      }
    },
  };
}

/**
 * The level of the sound that the browser played between two of its
 * reports, 0 (silence) to 1: the root of the mean square of the browser's
 * audio level, which is the largest sample of each 10 ms, over that time
 * (the report's sums totalAudioEnergy and totalSamplesDuration). 0 when the
 * browser played nothing between them; undefined without the sums.
 */
export function levelBetween(now, before) {
  const energy = now?.totalAudioEnergy - before?.totalAudioEnergy;
  const seconds = now?.totalSamplesDuration - before?.totalSamplesDuration;
  if (Number.isNaN(energy) || Number.isNaN(seconds)) return undefined;
  return seconds > 0 && energy > 0 ? Math.sqrt(energy / seconds) : 0;
}

/**
 * The clocks between the target page and this machine. The page sends, in
 * each report, its own clock in seconds and the host's clock in ms at the
 * same time; `at` is this machine's clock when the report came. report()
 * takes each report in. delayMs(heardAt) is the time from the page's beep
 * to the time at which this machine heard it (its clock in ms), as
 * beepDelayMs() takes it with now(). null before the first report with a
 * clock. ahead() is how far this machine's clock is ahead of the host's, in
 * ms, or Infinity before such a report. now() is { ahead, zero }: ahead,
 * and the host's clock at the page's second 0, which the recording keeps
 * beside the beeps; null before such a report.
 *
 * This machine's clock is ahead of the host's by the least difference
 * between `at` and the host's clock in a report: a report takes some time
 * on its way and never comes early.
 */
export function beepClock() {
  let ahead = Infinity;   // this machine's clock minus the host's, and the fastest report's way
  let zero = null;        // the host's clock at the page's second 0
  return {
    report: ({ seconds, clock, at }) => {
      if (typeof seconds !== 'number' || typeof clock !== 'number') return;
      ahead = Math.min(ahead, at - clock);
      zero = clock - seconds * 1000;
    },
    ahead: () => ahead,
    now: () => (zero === null ? null : { ahead, zero }),
    delayMs: heardAt => (zero === null ? null : beepDelayMs(heardAt, { ahead, zero })),
  };
}

/**
 * The time from the target page's beep to the time at which this machine
 * heard it, heardAt on its clock in ms. clock: { ahead, zero } of
 * beepClock(). The beeps are on the whole seconds of the page's clock, one
 * per second, so the delay is known when it is under a second.
 */
export const beepDelayMs = (heardAt, { ahead, zero }) =>
  (((heardAt - ahead - zero) % BEEP_PERIOD_MS) + BEEP_PERIOD_MS) % BEEP_PERIOD_MS;
