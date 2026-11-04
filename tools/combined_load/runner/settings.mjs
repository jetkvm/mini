// The settings that a Mini keeps in its flash and that change what a list
// measures, and automatic updates, which change what the Mini runs after
// it. A run sets each of them to the Mini's default before its first
// list, so that a recording does not depend on what somebody set on the
// bench before, and puts what it found into the recording's first row.
//
//   name -> async rpc => { found, standard, restore }
//
// found is the stored value, in a short form for the recording; standard is
// the default in the same form; restore() stores the default. A setting
// without restore is recorded and reported only: the run cannot change it
// and know that the change is done.
//
// Not here, because a list measures the same with every value: the keyboard
// layout and the mouse mode (the tool sends key codes and absolute
// positions), the codec preference (the Mini has one codec), the screen's
// backlight and rotation, and the network settings. The audio setting, the
// cloud's address and the update server's address are variables of a list,
// and their switches turn them off.

/** Service video as the Mini's default settings have it. */
export const SERVICE_VIDEO = { capture_display: '', resolution: '2560x1440', display_mode: 'extend', adaptation: '' };

export const STORED_SETTINGS = {
  // 0 is Auto: the bitrate follows what the browser reports. Another value
  // is a fixed share of the mode's bitrate, and the Mini's bitrate policy
  // is then off.
  streamQualityFactor: async rpc => ({
    found: await rpc('getStreamQualityFactor'),
    standard: 0,
    restore: () => rpc('setStreamQualityFactor', { factor: 0 }),
  }),
  // The display that the Mini shows the host: the first preset is the
  // default. Another one can have another size or frame rate. The run does
  // not change it. The Mini stores a new EDID and answers before the host
  // has it, gives it to the host only when HDMI capture and not service
  // video has the receiver, and takes the old one back when that fails,
  // and no call says which EDID the host has. So the report shows another
  // EDID, and somebody sets the default in the Mini's settings.
  edid: async rpc => {
    const presets = await rpc('getEDIDPresets');
    const edid = String(await rpc('getEDID')).toLowerCase();
    return { found: presets.find(preset => preset.edid.toLowerCase() === edid)?.name ?? 'custom', standard: presets[0].name };
  },
  // With it, the Mini takes the display away from the host while nobody
  // watches, and the first video of a session comes later.
  hostDisplayIdle: async rpc => ({
    found: (await rpc('getHostDisplayIdleMode')).enabled,
    standard: false,
    restore: () => rpc('setHostDisplayIdleMode', { enabled: false }),
  }),
  // Seconds without a viewer after which video stops; -1 is never. The
  // answer's "enabled" says whether video sleeps now, which it does not
  // during a run, so the stored value is "duration".
  videoSleepSeconds: async rpc => {
    const mode = await rpc('getVideoSleepMode');
    return {
      found: mode.duration,
      standard: -1,
      restore: mode.supported ? () => rpc('setVideoSleepMode', { duration: -1 }) : undefined,
    };
  },
  // Which display of the host the sender captures (empty: its own choice),
  // and the size, the display mode and the fixed rate of service video. The
  // Mini leaves an empty adaptation out of its answer.
  osServicesVideo: async rpc => {
    const stored = await rpc('getOsServicesSettings');
    return {
      found: { capture_display: stored.capture_display ?? '', resolution: stored.resolution, display_mode: stored.display_mode, adaptation: stored.adaptation ?? '' },
      standard: SERVICE_VIDEO,
      restore: () => rpc('setOsServicesSettings', { settings: SERVICE_VIDEO }),
    };
  },
  // The jiggler moves the host's mouse by itself.
  mouseJiggler: async rpc => ({
    found: await rpc('getJigglerState'),
    standard: false,
    restore: () => rpc('setJigglerState', { enabled: false }),
  }),
  // A connection to an MQTT broker beside the session. The setting has the
  // broker's password, so only "enabled" is read from it here.
  mqtt: async rpc => ({
    found: (await rpc('getMqttSettings')).enabled,
    standard: false,
    restore: async () => rpc('setMqttSettings', { settings: { ...await rpc('getMqttSettings'), enabled: false } }),
  }),
  // With them, a Mini without a session installs the newest release by
  // itself, 15 minutes after its start and then once an hour, and the newest
  // sender on its host. A bench Mini runs a test build with a version below
  // the newest release, so it would lose that build between two runs. A
  // list measures the same with either value: this one is here for the
  // time after the run.
  automaticUpdates: async rpc => ({
    found: await rpc('getAutoUpdateState'),
    standard: false,
    restore: () => rpc('setAutoUpdateState', { enabled: false }),
  }),
  // The Wi-Fi band. A change takes the Mini off the network, so the run
  // leaves it and the report shows it.
  wifiBand: async rpc => ({ found: (await rpc('getWifiSettings')).band, standard: 'auto' }),
};

const same = (a, b) => JSON.stringify(a) === JSON.stringify(b);

/**
 * Sets every stored setting of STORED_SETTINGS that has another value to its
 * default, and waits until the Mini reports the default. rpc(method, params)
 * is the Mini's JSON-RPC. Returns what the recording's first row gets as
 * "settings": for each setting { found }, and for one that was not at its
 * default also { default, set }, where set says whether the run changed it.
 * A setting that the Mini's build does not have (the Mini answers "Method
 * not found") is { missing: <the answer> }. Throws when another call fails,
 * and when a setting does not take its default.
 */
export async function setDefaults(rpc, { settings = STORED_SETTINGS, waitMs = 10_000, sleep = ms => new Promise(resolve => setTimeout(resolve, ms)) } = {}) {
  const row = {};
  for (const [name, read] of Object.entries(settings)) {
    let stored;
    try {
      stored = await read(rpc);
    } catch (error) {
      // Only a setting that this build does not have is passed over: a call
      // that fails for another reason leaves the setting unknown. The Mini
      // also says "unavailable" when a read of its flash fails, so that
      // word does not count.
      if (!/Method not found/.test(error.message)) throw new Error(`the Mini's stored ${name} cannot be read: ${error.message}`);
      row[name] = { missing: error.message };
      continue;
    }
    if (same(stored.found, stored.standard)) {
      row[name] = { found: stored.found };
      continue;
    }
    row[name] = { found: stored.found, default: stored.standard, set: Boolean(stored.restore) };
    if (!stored.restore) continue;
    await stored.restore();
    const end = Date.now() + waitMs;
    for (;;) {
      const now = await read(rpc).catch(() => null);
      if (now && same(now.found, now.standard)) break;
      if (Date.now() >= end) throw new Error(`the Mini keeps ${name} at ${JSON.stringify(now?.found)} and does not take the default, ${JSON.stringify(stored.standard)}`);
      await sleep(250);
    }
  }
  return row;
}

/**
 * The drive that the Mini shows the host. It is a CD-ROM drive or a disk, as
 * the image that was mounted when the Mini last enumerated on USB, and it
 * keeps that type after the unmount, until the next enumeration or restart.
 * A CD-ROM image for a drive that is a disk makes the Mini enumerate again,
 * which ends an OS Services session, so a list whose first mount is
 * a CD-ROM would depend on what the run before mounted last. The Mini does
 * not report the type. So this mounts a CD-ROM image and unmounts it, which
 * leaves a CD-ROM drive in every case, and tells from the Mini's count of
 * enumerations whether the Mini enumerated again for it. It does that for
 * a drive that was a disk, and also for a CD-ROM drive that the host did
 * not ask for its medium in the 3 seconds before, so the count does not
 * say which of the two the drive was.
 *
 * enumerations(): the Mini's count of USB enumerations; mount(): mounts a
 * CD-ROM image and returns when the host has it; unmount(): unmounts it and
 * returns when the host has no medium. Returns the entry for "settings" of
 * the recording's first row: { enumerated }. A request for the count that
 * fails is made again for up to waitMs.
 *
 * The unmount of a host that kept a medium of an earlier run mounts a disk
 * image to make the Mini enumerate again, which leaves a disk. So when the
 * count grows during the unmount, the mount and the unmount are made once
 * more, on a host that has nothing left then.
 */
export async function cdromDrive({ enumerations, mount, unmount, waitMs = 15_000, sleep = ms => new Promise(resolve => setTimeout(resolve, ms)) }) {
  const count = async () => {
    const end = Date.now() + waitMs;
    for (;;) {
      let failure;
      const now = await enumerations().catch(error => void (failure = error.message));
      if (Number.isInteger(now)) return now;
      if (Date.now() >= end) throw new Error(`the Mini does not report its count of USB enumerations (${failure ?? JSON.stringify(now)})`);
      await sleep(500);
    }
  };
  const before = await count();
  for (let round = 1; ; round++) {
    await mount();
    const mounted = await count();
    await unmount();
    const after = await count();
    if (after === mounted) return { enumerated: after > before };
    if (round === 3) throw new Error('the Mini enumerates on USB again at each unmount of the CD-ROM image before the list, so its drive is not a CD-ROM drive');
  }
}

/**
 * The report's lines for the settings of a first row that were not at their
 * default, and for a drive that did not take the CD-ROM image of the run's
 * start as a new medium.
 */
export function settingLines(settings = {}) {
  return Object.entries(settings).filter(([, entry]) => 'default' in entry || entry.enumerated).map(([name, entry]) => (
    entry.enumerated ? `${name} did not take a CD-ROM image as a new medium on the Mini; the Mini enumerated on USB again before the list, and the drive is a CD-ROM drive now`
      : entry.set ? `${name} was ${JSON.stringify(entry.found)} on the Mini; the run set the default, ${JSON.stringify(entry.default)}`
        : `${name} is ${JSON.stringify(entry.found)} on the Mini, not the default, ${JSON.stringify(entry.default)}; the run does not change it`));
}
