// node --test e2e/helpers/video-freeze.unit.ts
//
// The video watch's freeze rule on samples that the watch recorded on a
// Wi-Fi Mini. Each case holds the browser's samples ([Date.now(), framesDecoded]) of one window and
// the Mini's guestVideo readings ([from, to, source, received]) taken in it.
import assert from "node:assert/strict";
import { test } from "node:test";
import { MAX_FREEZE_MS, longestFreeze, type Sample, type SenderSample } from "./video-freeze.ts";

const readings = (rows: [number, number, string, number][]): SenderSample[] =>
  rows.map(([from, to, source, received]) => ({ from, to, source, received }));

test("service video: a gap in which the Mini received no frame is not a freeze", () => {
  // The sender process was stopped for 1.5 s, so the Mini received nothing,
  // as from an unchanged screen. The reading at ...5764 counts the frame that
  // ended the gap; the browser showed it 224 ms later.
  const samples: Sample[] = [
    [1791449674250, 38],
    [1791449674500, 38],
    [1791449674750, 38],
    [1791449675000, 38],
    [1791449675250, 38],
    [1791449675500, 38],
    [1791449675750, 38],
    [1791449676000, 46],
    [1791449676250, 53],
    [1791449676500, 61],
    [1791449676750, 68],
    [1791449677000, 76],
    [1791449677250, 83],
    [1791449677500, 91],
    [1791449677750, 98],
  ];
  const sender = readings([
    [1791449674502, 1791449674528, "guest", 82780],
    [1791449674750, 1791449674760, "guest", 82780],
    [1791449675001, 1791449675011, "guest", 82780],
    [1791449675251, 1791449675261, "guest", 82780],
    [1791449675501, 1791449675511, "guest", 82780],
    [1791449675764, 1791449675776, "guest", 82781],
    [1791449678251, 1791449678261, "switching_to_physical", 82845],
  ]);
  // Without the readings, the 1750 ms gap counts as a freeze.
  assert.equal(longestFreeze(samples).ms, 1750);
  const freeze = longestFreeze(samples, 0, Infinity, sender);
  assert.ok(freeze.ms <= MAX_FREEZE_MS, `${freeze.ms} ms`);
  // A window that ends before the next frame: 1500 ms without a new frame,
  // and five readings with the same count.
  const open = longestFreeze(samples, 0, 1791449675750, sender);
  assert.ok(open.ms <= MAX_FREEZE_MS, `${open.ms} ms`);
  // The same gap with only its first reading, as when the Mini stops
  // answering after it: the screen is unchanged only up to that answer.
  assert.deepEqual(longestFreeze(samples, 0, Infinity, sender.slice(0, 1)), {
    ms: 1791449676000 - 1791449674528,
    from: 1791449674528,
    serviceVideo: false,
  });
  // The same gap without the four readings between its first and the one
  // that counts the new frame, as when the Mini answers again only then.
  assert.deepEqual(longestFreeze(samples, 0, Infinity, [sender[0], sender[5]]), {
    ms: 1791449675776 - 1791449674528,
    from: 1791449674528,
    serviceVideo: false,
  });
});

test("service video: a gap in which the Mini received frames is a freeze", () => {
  // A pointer circled on the guest screen, and setVideoStreamPaused held the
  // Mini's stream for 2 s: the Mini kept receiving about 30 frames a second.
  const samples: Sample[] = [
    [1791448550916, 425],
    [1791448551168, 425],
    [1791448551416, 425],
    [1791448551666, 425],
    [1791448551920, 425],
    [1791448552166, 425],
    [1791448552416, 425],
    [1791448552672, 425],
    [1791448552916, 425],
    [1791448553168, 425],
    [1791448553416, 428],
    [1791448553666, 434],
    [1791448553920, 441],
    [1791448554166, 449],
    [1791448554416, 458],
    [1791448554674, 466],
    [1791448554916, 467],
    [1791448555168, 476],
    [1791448555416, 483],
    [1791448555666, 489],
  ];
  const sender = readings([
    [1791448551170, 1791448551192, "guest", 51468],
    [1791448551417, 1791448551432, "guest", 51475],
    [1791448551666, 1791448551676, "guest", 51482],
    [1791448551921, 1791448551934, "guest", 51490],
    [1791448552167, 1791448552195, "guest", 51497],
    [1791448552417, 1791448552431, "guest", 51505],
    [1791448552673, 1791448552687, "guest", 51513],
    [1791448552917, 1791448552933, "guest", 51520],
    [1791448553170, 1791448553181, "guest", 51522],
  ]);
  // From the first reading that shows a new frame to the next decoded frame.
  assert.deepEqual(longestFreeze(samples, 0, Infinity, sender), {
    ms: 1791448553416 - 1791448551432,
    from: 1791448551432,
    serviceVideo: true,
  });
});

test("HDMI: a gap is a freeze although the sender's count does not change", () => {
  // Service video off; setVideoStreamPaused held the Mini's HDMI stream for 2 s.
  const samples: Sample[] = [
    [1791449683250, 149],
    [1791449683500, 149],
    [1791449683750, 149],
    [1791449684000, 149],
    [1791449684250, 149],
    [1791449684500, 149],
    [1791449684750, 149],
    [1791449685000, 149],
    [1791449685250, 150],
    [1791449685500, 158],
    [1791449685750, 165],
    [1791449686000, 173],
    [1791449686250, 179],
    [1791449686500, 187],
    [1791449686750, 194],
    [1791449687000, 202],
  ];
  const sender = readings([
    [1791449683500, 1791449683510, "physical", 82845],
    [1791449684750, 1791449684760, "physical", 82845],
    [1791449687500, 1791449687515, "switching_to_guest", 82845],
  ]);
  assert.deepEqual(longestFreeze(samples, 0, Infinity, sender), {
    ms: 2000,
    from: 1791449683250,
    serviceVideo: false,
  });
});
