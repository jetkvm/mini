// Connection diagnosis: from a set of signals to one headline, a map of what
// is broken, and a list of findings. This module is pure (no React, no
// browser API, no imports), so the gallery page (routes/dev.diagnosis.tsx)
// can feed it mocked signals and tests/diagnosis.mjs can run it in Node.
//
// The signals are a mock for now: nothing collects them from a real browser
// or a real JetKVM yet.

export type CheckId =
  "browser" | "reach" | "connection" | "health" | "picture" | "input" | "audio" | "quality";

export const CHECKS: { id: CheckId; title: string }[] = [
  { id: "browser", title: "Browser" },
  { id: "reach", title: "Reach the JetKVM" },
  { id: "connection", title: "Connection" },
  { id: "health", title: "JetKVM health" },
  { id: "picture", title: "Picture" },
  { id: "input", title: "Keyboard and mouse" },
  { id: "audio", title: "Audio" },
  { id: "quality", title: "Quality" },
];

interface SignalDef {
  label: string;
  // The first option is the healthy value.
  options: Record<string, string>;
}

export const SIGNALS = {
  // Context: these select the wording and the path.
  access: { label: "Access", options: { local: "local network", cloud: "JetKVM Cloud" } },
  browser: {
    label: "Browser",
    options: { chromium: "Chrome", firefox: "Firefox", safari: "Safari" },
  },
  address: { label: "Opened by", options: { name: "host name", ip: "IP address" } },

  // Browser.
  webrtcApi: { label: "WebRTC", options: { present: "present", missing: "turned off" } },
  h264: { label: "H.264 decoder", options: { supported: "supported", missing: "missing" } },
  candidates: { label: "Network addresses shared", options: { gathered: "yes", none: "none" } },

  // Reach.
  miniActivity: { label: "JetKVM", options: { normal: "normal", rebooting: "restarting" } },
  http: { label: "HTTP request", options: { ok: "answered", unreachable: "no answer" } },
  cloudDevice: { label: "JetKVM in the cloud", options: { online: "online", offline: "offline" } },
  socket: { label: "WebSocket", options: { open: "opens", blocked: "does not open" } },
  session: { label: "Session", options: { ours: "this browser", taken_over: "another browser" } },
  answer: {
    label: "Answer from the JetKVM",
    options: { ok: "received", none: "none in 10 s", rejected: "refused by the browser" },
  },

  // Connection.
  ice: { label: "Network path for video", options: { connected: "found", failed: "none" } },
  route: { label: "Route", options: { direct: "direct", relay: "relayed" } },
  dataChannel: { label: "Secure channel", options: { open: "open", closed: "does not open" } },

  // JetKVM health.
  restart: {
    label: "Restart in the last 5 minutes",
    options: { none: "none", power: "power loss", brownout: "voltage drop", fault: "fault" },
  },
  clock: { label: "JetKVM clock", options: { set: "set", not_set: "not set" } },
  link: {
    label: "JetKVM network",
    options: { ethernet: "Ethernet", wifi_good: "Wi-Fi, good", wifi_weak: "Wi-Fi, weak" },
  },

  // Picture.
  videoPause: {
    label: "Video paused for",
    options: { none: "nothing", update: "an update", download: "a download", upload: "an upload" },
  },
  hdmi: {
    label: "HDMI input",
    options: {
      locked: "signal",
      no_cable: "no cable (no 5 V)",
      no_signal: "cable, no signal",
      unsupported_mode: "3840×2160 at 60 Hz",
    },
  },
  edid: { label: "EDID", options: { default: "JetKVM default", custom: "custom" } },
  video: {
    label: "Video frames",
    options: {
      flowing: "sent and decoded",
      not_sent: "the JetKVM sends none",
      not_decoded: "the browser decodes none",
    },
  },
  playback: { label: "Video playback", options: { playing: "plays", blocked: "autoplay blocked" } },

  // Keyboard and mouse.
  usb: {
    label: "USB",
    options: {
      configured: "configured",
      suspended: "suspended",
      attached: "attached, not configured",
      not_attached: "not attached",
    },
  },
  keyboardTest: { label: "Caps Lock test", options: { reply: "reply", no_reply: "no reply" } },

  // Audio.
  audioSetting: { label: "Audio setting", options: { on: "on", off: "off" } },
  usbAudio: { label: "USB Audio device", options: { on: "on", off: "off" } },
  audioSource: {
    label: "Audio from the computer",
    options: { streaming: "streams", idle: "no stream", not_ready: "device not set up" },
  },
  audioLevel: { label: "Audio level", options: { signal: "sound", silent: "silent" } },
  audioPlayback: { label: "Audio playback", options: { playing: "plays", blocked: "blocked" } },

  // Quality.
  loss: { label: "Packet loss", options: { low: "low", high: "high" } },
  delay: { label: "Network delay", options: { low: "low", high: "high" } },
} as const satisfies Record<string, SignalDef>;

export type SignalKey = keyof typeof SIGNALS;
export type DiagnosisInput = {
  -readonly [K in SignalKey]: keyof (typeof SIGNALS)[K]["options"];
};
export type InputChange = Partial<DiagnosisInput>;

export const SIGNAL_KEYS = Object.keys(SIGNALS) as SignalKey[];

export function healthyInput(): DiagnosisInput {
  const input: Record<string, string> = {};
  for (const key of SIGNAL_KEYS) input[key] = Object.keys(SIGNALS[key].options)[0];
  return input as DiagnosisInput;
}

export function applyChange(base: DiagnosisInput, change: InputChange): DiagnosisInput {
  return { ...base, ...change };
}

export const FINDING_IDS = [
  "webrtc_off",
  "no_h264",
  "no_candidates",
  "rebooting",
  "mini_unreachable",
  "cloud_unreachable",
  "mini_offline",
  "safari_ip",
  "socket_blocked",
  "session_taken",
  "no_answer",
  "answer_refused",
  "udp_blocked",
  "no_secure_channel",
  "restart_power",
  "restart_brownout",
  "restart_fault",
  "clock_not_set",
  "video_paused",
  "computer_off",
  "no_hdmi_cable",
  "computer_asleep",
  "no_picture_edid",
  "no_picture",
  "unsupported_mode",
  "mini_sends_no_video",
  "browser_decodes_no_video",
  "autoplay_blocked",
  "usb_follows_computer",
  "usb_not_attached",
  "usb_not_configured",
  "usb_suspended",
  "keyboard_no_reply",
  "audio_off",
  "usb_audio_off",
  "audio_device_not_ready",
  "audio_no_stream",
  "audio_silent",
  "audio_blocked",
  "relayed",
  "wifi_weak",
  "packet_loss",
  "high_delay",
] as const;
export type FindingId = (typeof FINDING_IDS)[number];

// problem: something is wrong and the user can act.
// wait:    an expected state that ends by itself (restart, update).
// warning: works, but worse than it could.
// info:    worth a line, needs no action.
export type Level = "problem" | "wait" | "warning" | "info";

// The parts of the map: three things and the three links between them.
//   browser --network-- mini --hdmi-- computer
//                            --usb---
export type Place = "browser" | "network" | "mini" | "hdmi" | "usb" | "computer";
export const PLACES: Place[] = ["browser", "network", "mini", "hdmi", "usb", "computer"];
// unknown: cannot be checked, because something before it is down.
export type PlaceState = "ok" | "problem" | "wait" | "warning" | "unknown";

export interface Finding {
  id: FindingId;
  check: CheckId;
  level: Level;
  // Where on the map the fault is.
  where: Place;
  // What is wrong, in a few words.
  title: string;
  // What to do, one sentence.
  fix?: string;
  // A button the panel offers.
  action?: string;
  // The evidence, for the details view.
  detail: string;
  // The signal changes that the fix makes. The gallery applies them to
  // simulate the action.
  resolves?: InputChange;
  // Issues that show this case.
  refs?: string;
}

export type CheckStatus = Level | "pass" | "skipped";

export interface CheckResult {
  id: CheckId;
  title: string;
  status: CheckStatus;
  summary: string;
  findings: Finding[];
}

export interface Diagnosis {
  checks: CheckResult[];
  // The first problem or wait state, if any.
  headline: Finding | null;
  // Every other finding, in check order.
  others: Finding[];
  map: Record<Place, PlaceState>;
}

function browserFindings(i: DiagnosisInput): Finding[] {
  if (i.webrtcApi === "missing") {
    return [
      {
        id: "webrtc_off",
        check: "browser",
        level: "problem",
        where: "browser",
        title: "WebRTC is turned off in this browser",
        fix:
          i.browser === "firefox"
            ? "In about:config, set media.peerconnection.enabled to true."
            : "Turn off the extension or setting that blocks WebRTC, then reload.",
        detail: "The browser has no RTCPeerConnection. The JetKVM sends video only over WebRTC.",
        resolves: { webrtcApi: "present" },
        refs: "kvm #84",
      },
    ];
  }
  if (i.h264 === "missing") {
    return [
      {
        id: "no_h264",
        check: "browser",
        level: "problem",
        where: "browser",
        title: "This browser cannot play the video",
        fix:
          i.browser === "firefox"
            ? "In about:addons, open Plugins and enable OpenH264."
            : "Use a current Chrome, Edge, Firefox or Safari.",
        detail:
          "H.264 is not in the browser's WebRTC codec list. The JetKVM Mini sends H.264 only.",
        resolves: { h264: "supported" },
        refs: "mini #4, kvm #655",
      },
    ];
  }
  if (i.candidates === "none") {
    const fix: Record<DiagnosisInput["browser"], string> = {
      firefox: "In about:config, set media.peerconnection.ice.no_host to false.",
      chromium: 'In chrome://flags, set "WebRTC IP handling policy" to Default.',
      safari: "Turn off the extension or content blocker that limits WebRTC.",
    };
    return [
      {
        id: "no_candidates",
        check: "browser",
        level: "problem",
        where: "browser",
        title: "This browser hides its network address",
        fix: fix[i.browser],
        detail:
          "A test connection gathered no ICE candidate. A privacy setting, an extension or a VPN hides them.",
        resolves: { candidates: "gathered" },
        refs: "mini #3, kvm #84",
      },
    ];
  }
  return [];
}

function reachFindings(i: DiagnosisInput): Finding[] {
  if (i.miniActivity === "rebooting") {
    return [
      {
        id: "rebooting",
        check: "reach",
        level: "wait",
        where: "mini",
        title: "The JetKVM is restarting",
        fix: "This takes about 30 seconds.",
        detail: "The JetKVM said that it restarts.",
        resolves: { miniActivity: "normal", http: "ok", socket: "open" },
      },
    ];
  }
  if (i.http === "unreachable") {
    if (i.access === "cloud") {
      return [
        {
          id: "cloud_unreachable",
          check: "reach",
          level: "problem",
          where: "network",
          title: "This computer is offline",
          fix: "Check your internet connection.",
          detail: "A request to JetKVM Cloud got no answer.",
          resolves: { http: "ok" },
        },
      ];
    }
    return [
      {
        id: "mini_unreachable",
        check: "reach",
        level: "problem",
        where: "network",
        title: "The JetKVM does not answer",
        fix: "Check that it has power and that its display shows this address.",
        detail:
          "A request to this page's address got no answer. The browser cannot tell power loss from a network fault.",
        resolves: { http: "ok", socket: "open" },
        refs: "kvm #270",
      },
    ];
  }
  if (i.access === "cloud" && i.cloudDevice === "offline") {
    return [
      {
        id: "mini_offline",
        check: "reach",
        level: "problem",
        where: "network",
        title: "The JetKVM is offline",
        fix: "Check its power and its network. Its display shows both.",
        detail:
          "JetKVM Cloud has no link from this JetKVM. Its clock may not be set, or its network blocks the cloud.",
        resolves: { cloudDevice: "online" },
        refs: "kvm #293, #1730",
      },
    ];
  }
  if (i.socket === "blocked") {
    if (i.browser === "safari" && i.address === "ip" && i.access === "local") {
      return [
        {
          id: "safari_ip",
          check: "reach",
          level: "problem",
          where: "browser",
          title: "Safari cannot connect by IP address",
          fix: "Open the JetKVM by its name, or use another browser.",
          action: "Open by name",
          detail: "The page loads, but Safari 26 never starts the WebSocket to an IP address.",
          resolves: { address: "name", socket: "open" },
          refs: "kvm #1098",
        },
      ];
    }
    return [
      {
        id: "socket_blocked",
        check: "reach",
        level: "problem",
        where: "network",
        title: "Something blocks the connection",
        fix: "A proxy, an extension or security software stops it. Try a private window.",
        action: "Try again",
        detail: "The page loads over HTTP, but the WebSocket for signalling does not open.",
        resolves: { socket: "open" },
        refs: "kvm #431, #484",
      },
    ];
  }
  if (i.session === "taken_over") {
    return [
      {
        id: "session_taken",
        check: "reach",
        level: "problem",
        where: "mini",
        title: "Another browser is using this JetKVM",
        fix: "Only one browser can use it at a time.",
        action: "Use it here",
        detail: "Another browser took the session.",
        resolves: { session: "ours" },
      },
    ];
  }
  if (i.answer === "none") {
    return [
      {
        id: "no_answer",
        check: "reach",
        level: "problem",
        where: "mini",
        title: "The JetKVM is not responding",
        fix: "Unplug it for a moment, then try again.",
        action: "Try again",
        detail: "The JetKVM accepted the request and sent no answer in 10 seconds, four times.",
        resolves: { answer: "ok" },
        refs: "kvm #84",
      },
    ];
  }
  if (i.answer === "rejected") {
    return [
      {
        id: "answer_refused",
        check: "reach",
        level: "problem",
        where: "mini",
        title: "The connection could not start",
        fix: "Try another browser. If this stays, send a report.",
        action: "Try again",
        detail:
          "The browser refused the JetKVM's answer. This is a firmware fault or a rare browser setup.",
        resolves: { answer: "ok" },
        refs: "mini #4",
      },
    ];
  }
  return [];
}

function connectionFindings(i: DiagnosisInput): Finding[] {
  if (i.ice === "failed") {
    return [
      {
        id: "udp_blocked",
        check: "connection",
        level: "problem",
        where: "network",
        title: "Your network blocks the video",
        fix:
          i.access === "cloud"
            ? "Disconnect your VPN, or try another network."
            : "Disconnect your VPN. On another VLAN, allow UDP to the JetKVM.",
        action: "Try again",
        detail:
          i.access === "cloud"
            ? "Signalling works, but no path connected, also not through the relay."
            : "The page loads, but video uses UDP and no UDP packet got through.",
        resolves: { ice: "connected" },
        refs: "kvm #84, #458, #1165, #1770",
      },
    ];
  }
  if (i.dataChannel === "closed") {
    return [
      {
        id: "no_secure_channel",
        check: "connection",
        level: "problem",
        where: "mini",
        title: "The connection could not start",
        fix: "Unplug the JetKVM for a moment, then try again.",
        action: "Try again",
        detail: "A network path exists, but the encrypted channel did not open in 10 seconds.",
        resolves: { dataChannel: "open" },
        refs: "mini #3",
      },
    ];
  }
  return [];
}

function healthFindings(i: DiagnosisInput): Finding[] {
  const findings: Finding[] = [];
  if (i.restart === "power") {
    findings.push({
      id: "restart_power",
      check: "health",
      level: "info",
      where: "mini",
      title: "The JetKVM lost power a moment ago",
      fix: "To keep it on while the computer is off, connect a charger to the EXT port.",
      detail: "Reset reason: power on. Uptime is below 5 minutes.",
    });
  }
  if (i.restart === "brownout") {
    findings.push({
      id: "restart_brownout",
      check: "health",
      level: "warning",
      where: "mini",
      title: "The JetKVM restarted: its power is weak",
      fix: "Use a shorter USB cable, or connect a charger to the EXT port.",
      detail: "Reset reason: brownout. Uptime is below 5 minutes.",
    });
  }
  if (i.restart === "fault") {
    findings.push({
      id: "restart_fault",
      check: "health",
      level: "warning",
      where: "mini",
      title: "The JetKVM restarted after a fault",
      fix: "If this happens again, send a report.",
      detail: "Reset reason: panic or watchdog. Uptime is below 5 minutes.",
    });
  }
  if (i.clock === "not_set") {
    findings.push({
      id: "clock_not_set",
      check: "health",
      level: "warning",
      where: "mini",
      title: "The JetKVM's clock is not set",
      fix: "Cloud access and updates need it. Allow NTP (UDP port 123) for the JetKVM.",
      detail: "The JetKVM got no time from the network.",
      refs: "kvm #293, #576",
    });
  }
  return findings;
}

// What the HDMI and USB states together say about the computer.
function computerState(i: DiagnosisInput): "on" | "asleep" | "off" {
  if (i.hdmi === "locked" || i.hdmi === "unsupported_mode") return "on";
  if (i.usb === "not_attached") return "off";
  if (i.hdmi === "no_signal" && i.usb === "suspended") return "asleep";
  return "on";
}

function pictureFindings(i: DiagnosisInput): Finding[] {
  if (i.videoPause !== "none") {
    const work: Record<Exclude<DiagnosisInput["videoPause"], "none">, string> = {
      update: "the update",
      download: "the download",
      upload: "the upload",
    };
    return [
      {
        id: "video_paused",
        check: "picture",
        level: "wait",
        where: "mini",
        title: `Video is paused during ${work[i.videoPause]}`,
        fix: `It returns when ${work[i.videoPause]} ends.`,
        detail: "The JetKVM stops video for this work, to keep enough memory free.",
        resolves: { videoPause: "none" },
      },
    ];
  }

  if (i.hdmi === "no_cable" || i.hdmi === "no_signal") {
    const computer = computerState(i);
    if (computer === "off") {
      return [
        {
          id: "computer_off",
          check: "picture",
          level: "problem",
          where: "computer",
          title: "The computer is off",
          fix: "Turn it on. If it is on, check the HDMI cable and the USB cable.",
          action: "Wake on LAN",
          detail:
            i.hdmi === "no_cable"
              ? "The HDMI input has no 5 V and USB is not attached."
              : "The HDMI input has no signal and USB is not attached.",
          resolves: { hdmi: "locked", usb: "configured" },
          refs: "kvm #427",
        },
      ];
    }
    if (i.hdmi === "no_cable") {
      return [
        {
          id: "no_hdmi_cable",
          check: "picture",
          level: "problem",
          where: "hdmi",
          title: "The HDMI cable is not connected",
          fix: "Connect it to a video output of the computer.",
          detail:
            "USB is connected, but the HDMI input has no 5 V from the computer. For DisplayPort or USB-C, an active adapter is needed.",
          resolves: { hdmi: "locked" },
          refs: "kvm #297",
        },
      ];
    }
    if (computer === "asleep") {
      return [
        {
          id: "computer_asleep",
          check: "picture",
          level: "problem",
          where: "computer",
          title: "The computer is asleep",
          action: "Wake it",
          detail: "The HDMI cable is connected and has no signal. USB is suspended.",
          resolves: { hdmi: "locked", usb: "configured" },
          refs: "kvm #53, #394",
        },
      ];
    }
    if (i.edid === "custom") {
      return [
        {
          id: "no_picture_edid",
          check: "picture",
          level: "problem",
          where: "hdmi",
          title: "The computer sends no picture",
          fix: "The custom EDID can be the cause.",
          action: "Use the default EDID",
          detail:
            "The HDMI cable is connected, USB works, and there is no signal. The EDID is not the JetKVM default.",
          resolves: { hdmi: "locked", edid: "default" },
          refs: "kvm #554, #1057",
        },
      ];
    }
    return [
      {
        id: "no_picture",
        check: "picture",
        level: "problem",
        where: "hdmi",
        title: "The computer sends no picture",
        fix: "Its display is asleep or this output is turned off.",
        action: "Wake the display",
        detail:
          "The HDMI cable is connected, USB works, and there is no signal. A laptop may have its external display off; a passive adapter can also cause this.",
        resolves: { hdmi: "locked" },
        refs: "kvm #1193, #385",
      },
    ];
  }

  if (i.hdmi === "unsupported_mode") {
    return [
      {
        id: "unsupported_mode",
        check: "picture",
        level: "problem",
        where: "hdmi",
        title: "The resolution is too high",
        fix:
          i.edid === "custom"
            ? "The custom EDID offers 3840×2160. The JetKVM Mini needs 1920×1080 or lower."
            : "The computer sends 3840×2160. Set it to 1920×1080 or lower.",
        action: i.edid === "custom" ? "Use the default EDID" : undefined,
        detail:
          "Detected 3840×2160 at 60 Hz. The maximum is 1920×1080 at 30 Hz or 1280×720 at 60 Hz.",
        resolves: { hdmi: "locked", edid: "default" },
        refs: "kvm #1482, #1480",
      },
    ];
  }

  if (i.video === "not_sent") {
    return [
      {
        id: "mini_sends_no_video",
        check: "picture",
        level: "problem",
        where: "mini",
        title: "The JetKVM stopped sending video",
        fix: "A restart fixes this.",
        action: "Restart the JetKVM",
        detail: "The HDMI signal is locked, but the count of sent frames does not increase.",
        resolves: { video: "flowing" },
        refs: "kvm #84, #1275",
      },
    ];
  }
  if (i.video === "not_decoded") {
    return [
      {
        id: "browser_decodes_no_video",
        check: "picture",
        level: "problem",
        where: "browser",
        title: "This browser cannot show the video",
        fix:
          i.browser === "firefox"
            ? "In about:config, set media.webrtc.hw.h264.enabled to true. Then restart Firefox."
            : "Turn on hardware acceleration in the browser settings. Then restart the browser.",
        detail: "Video packets arrive and the count of decoded frames stays at 0.",
        resolves: { video: "flowing" },
        refs: "kvm #655, #84",
      },
    ];
  }
  if (i.playback === "blocked") {
    return [
      {
        id: "autoplay_blocked",
        check: "picture",
        level: "problem",
        where: "browser",
        title: "The browser paused the video",
        action: "Start video",
        detail:
          "The browser refused to start the video without a click. Allowing autoplay for this address skips this.",
        resolves: { playback: "playing" },
        refs: "kvm #84, #1504",
      },
    ];
  }
  return [];
}

function inputFindings(i: DiagnosisInput): Finding[] {
  const computer = computerState(i);
  if (computer !== "on") {
    return [
      {
        id: "usb_follows_computer",
        check: "input",
        level: "info",
        where: "usb",
        title:
          computer === "asleep"
            ? "USB is suspended because the computer is asleep"
            : "USB is off because the computer is off",
        detail: "This follows from the state of the computer. It is not a second fault.",
      },
    ];
  }
  if (i.usb === "not_attached") {
    return [
      {
        id: "usb_not_attached",
        check: "input",
        level: "problem",
        where: "usb",
        title: "The USB cable is not connected",
        fix: "Connect the port with the monitor icon to the computer. Use a data cable, no hub.",
        detail: "The computer is on (it sends a picture), and USB is not attached.",
        resolves: { usb: "configured" },
        refs: "kvm #1208, #1167, #412",
      },
    ];
  }
  if (i.usb === "attached") {
    return [
      {
        id: "usb_not_configured",
        check: "input",
        level: "problem",
        where: "usb",
        title: "The computer rejected the USB device",
        fix: "Some BIOS versions and KVM switches accept a keyboard and mouse only.",
        action: "Use keyboard and mouse only",
        detail: "USB is attached and not configured.",
        resolves: { usb: "configured" },
        refs: "kvm #236, #79, #764",
      },
    ];
  }
  if (i.usb === "suspended") {
    return [
      {
        id: "usb_suspended",
        check: "input",
        level: "info",
        where: "usb",
        title: "USB is asleep",
        fix: "The first key or mouse move wakes it.",
        detail: "The computer suspended the USB port.",
      },
    ];
  }
  if (i.keyboardTest === "no_reply") {
    return [
      {
        id: "keyboard_no_reply",
        check: "input",
        level: "problem",
        where: "usb",
        title: "The computer ignores the keyboard",
        fix: "Some BIOS versions and KVM switches accept a keyboard and mouse only.",
        action: "Use keyboard and mouse only",
        detail:
          "The JetKVM pressed Caps Lock two times and the computer did not report the LED change.",
        resolves: { keyboardTest: "reply" },
        refs: "kvm #236, #1314, #764",
      },
    ];
  }
  return [];
}

function audioFindings(i: DiagnosisInput): Finding[] {
  if (i.audioSetting === "off") {
    return [
      {
        id: "audio_off",
        check: "audio",
        level: "info",
        where: "mini",
        title: "Audio is off",
        action: "Turn on audio",
        detail: "The Audio setting of the JetKVM is off.",
        resolves: { audioSetting: "on" },
      },
    ];
  }
  if (i.usbAudio === "off") {
    return [
      {
        id: "usb_audio_off",
        check: "audio",
        level: "problem",
        where: "usb",
        title: "No sound: USB Audio is off",
        action: "Turn on USB Audio",
        detail: "Audio is on, but the JetKVM does not present an audio device to the computer.",
        resolves: { usbAudio: "on" },
      },
    ];
  }
  if (i.audioSource === "not_ready") {
    return [
      {
        id: "audio_device_not_ready",
        check: "audio",
        level: "problem",
        where: "usb",
        title: "No sound: the computer did not set up audio",
        fix: "Unplug the USB cable and plug it in again.",
        detail: "USB is configured, but the computer did not start the audio interface.",
        resolves: { audioSource: "streaming" },
      },
    ];
  }
  if (i.audioSource === "idle") {
    return [
      {
        id: "audio_no_stream",
        check: "audio",
        level: "warning",
        where: "computer",
        title: "No sound from the computer",
        fix: "On the computer, select JetKVM as the audio output.",
        detail:
          "The audio device is set up and no audio stream arrives. This is normal while nothing plays.",
        resolves: { audioSource: "streaming" },
      },
    ];
  }
  if (i.audioLevel === "silent") {
    return [
      {
        id: "audio_silent",
        check: "audio",
        level: "warning",
        where: "computer",
        title: "The computer's sound is silent",
        fix: "Check the volume and mute on the computer.",
        detail: "The computer streams audio with a level of zero.",
        resolves: { audioLevel: "signal" },
      },
    ];
  }
  if (i.audioPlayback === "blocked") {
    return [
      {
        id: "audio_blocked",
        check: "audio",
        level: "problem",
        where: "browser",
        title: "The browser blocks sound",
        action: "Turn on sound",
        detail: "The browser refused to start audio without a click.",
        resolves: { audioPlayback: "playing" },
      },
    ];
  }
  return [];
}

function qualityFindings(i: DiagnosisInput): Finding[] {
  const findings: Finding[] = [];
  if (i.link === "wifi_weak") {
    findings.push({
      id: "wifi_weak",
      check: "quality",
      level: "warning",
      where: "network",
      title: "The JetKVM's Wi-Fi is weak",
      fix: "Move it closer to the access point.",
      detail: "Signal strength is −78 dBm. Below −70 dBm, video stutters.",
      resolves: { link: "wifi_good" },
    });
  }
  if (i.loss === "high") {
    findings.push({
      id: "packet_loss",
      check: "quality",
      level: "warning",
      where: "network",
      title: "The network drops video data",
      fix:
        i.link === "wifi_weak"
          ? "The weak Wi-Fi at the JetKVM is the probable cause."
          : "On Wi-Fi, move closer to the access point.",
      detail: "More than 2% of the video packets do not arrive.",
    });
  }
  if (i.delay === "high") {
    findings.push({
      id: "high_delay",
      check: "quality",
      level: "warning",
      where: "network",
      title: "The network is slow",
      fix:
        i.route === "relay"
          ? "The video goes through a relay. On the same network it goes direct."
          : "Check the network between this computer and the JetKVM.",
      detail: "The round trip to the JetKVM takes more than 150 ms.",
    });
  }
  if (i.route === "relay") {
    findings.push({
      id: "relayed",
      check: "quality",
      level: "info",
      where: "network",
      title: "The video goes through a relay",
      fix: "Expect more delay than on a direct connection.",
      detail:
        "No direct path exists between this browser and the JetKVM, so video goes through Cloudflare.",
    });
  }
  return findings;
}

const LEVEL_ORDER: Level[] = ["problem", "wait", "warning", "info"];

function statusOf(findings: Finding[]): CheckStatus {
  for (const level of LEVEL_ORDER) {
    if (findings.some(finding => finding.level === level)) return level;
  }
  return "pass";
}

const stops = (findings: Finding[]) =>
  findings.some(finding => finding.level === "problem" || finding.level === "wait");

export function diagnose(i: DiagnosisInput): Diagnosis {
  const checks: CheckResult[] = [];
  const title = (id: CheckId) => CHECKS.find(check => check.id === id)!.title;

  const run = (id: CheckId, findings: Finding[], passSummary: string) => {
    checks.push({
      id,
      title: title(id),
      status: statusOf(findings),
      summary: findings.length > 0 ? findings[0].title : passSummary,
      findings,
    });
    return findings;
  };
  const skip = (id: CheckId, why: string) => {
    checks.push({ id, title: title(id), status: "skipped", summary: why, findings: [] });
  };

  // Browser, reach and connection depend on each other: each needs the one
  // before it. The first one that stops ends the chain.
  let stoppedAt: CheckId | null = null;
  const chain: [CheckId, () => Finding[], string][] = [
    ["browser", () => browserFindings(i), "WebRTC and H.264 are available"],
    ["reach", () => reachFindings(i), "The JetKVM answers"],
    [
      "connection",
      () => connectionFindings(i),
      i.route === "relay" ? "Connected through a relay" : "Connected directly",
    ],
  ];
  for (const [id, find, passSummary] of chain) {
    if (stoppedAt) {
      skip(id, "Not checked");
    } else if (stops(run(id, find(), passSummary))) {
      stoppedAt = id;
    }
  }

  let computerDown = false;
  if (stoppedAt) {
    for (const id of ["health", "picture", "input", "audio", "quality"] as const) {
      skip(id, "Needs a connection");
    }
  } else {
    // With a connection, the JetKVM reports its state, and these checks do
    // not depend on each other, except where noted.
    run("health", healthFindings(i), "No recent restart");

    const picture = run("picture", pictureFindings(i), "The picture arrives and plays");
    const input = run("input", inputFindings(i), "The computer accepts the keyboard");
    computerDown = computerState(i) !== "on" && i.videoPause === "none";

    // Audio comes over USB, so it needs a configured USB device. With audio
    // off, there is nothing to check and the check only says so.
    if (i.audioSetting === "on" && (i.usb !== "configured" || stops(input))) {
      skip("audio", "Needs USB");
    } else {
      run("audio", audioFindings(i), "Sound arrives and plays");
    }

    if (stops(picture)) {
      skip("quality", "Needs a picture");
    } else {
      run("quality", qualityFindings(i), "Low loss and low delay");
    }
  }

  const all = checks.flatMap(check => check.findings);
  const headline =
    all.find(finding => finding.level === "problem") ??
    all.find(finding => finding.level === "wait") ??
    null;

  // The map: the worst finding at each place, and "unknown" for the places
  // that could not be checked.
  const map = Object.fromEntries(PLACES.map(place => [place, "ok"])) as Record<Place, PlaceState>;
  for (const level of ["warning", "wait", "problem"] as const) {
    for (const finding of all) {
      if (finding.level === level) map[finding.where] = level;
    }
  }
  const unknown = (places: Place[]) => {
    for (const place of places) if (map[place] === "ok") map[place] = "unknown";
  };
  if (stoppedAt === "browser") unknown(["network", "mini", "hdmi", "usb", "computer"]);
  else if (stoppedAt) unknown(["mini", "hdmi", "usb", "computer"]);
  else if (computerDown) unknown(["hdmi", "usb"]);

  return { checks, headline, others: all.filter(finding => finding !== headline), map };
}

// What the panel leads with: the headline, or without one the first warning,
// or the first note.
export function leadFinding(diagnosis: Diagnosis): Finding | null {
  return (
    diagnosis.headline ??
    diagnosis.others.find(finding => finding.level === "warning") ??
    diagnosis.others[0] ??
    null
  );
}

// The screen the web UI shows today for the same signals. This is a model of
// routes/devices.$id.tsx, WebRTCVideo.tsx and useWebSocketSignaling.ts, for
// the gallery's side-by-side view; the product does not use it.
export type TodayScreenId =
  | "rebooting"
  | "other_session"
  | "connecting"
  | "connection_failed"
  | "video_paused"
  | "no_hdmi"
  | "loading_video"
  | "autoplay"
  | "video";

export interface TodayScreen {
  id: TodayScreenId;
  // What happens today, in one line.
  note: string;
}

export function todayScreen(i: DiagnosisInput): TodayScreen {
  if (i.webrtcApi === "missing") {
    return { id: "connection_failed", note: "Fixed text, at once." };
  }
  if (i.miniActivity === "rebooting") {
    return { id: "rebooting", note: "Shown after the JetKVM announces a restart." };
  }
  if (i.http === "unreachable" || i.socket === "blocked") {
    return { id: "connecting", note: "The spinner stays for hours: 2,000 silent retries." };
  }
  if (i.access === "cloud" && i.cloudDevice === "offline") {
    return {
      id: "connecting",
      note: "The spinner stays: no timeout while waiting for the JetKVM.",
    };
  }
  if (i.session === "taken_over") {
    return { id: "other_session", note: "A dialog offers to take the session back." };
  }
  if (i.h264 === "missing" || i.answer !== "ok" || i.candidates === "none") {
    return {
      id: "connection_failed",
      note: "Fixed text after 4 tries (10 to 40 s). The reason goes to the console.",
    };
  }
  if (i.ice === "failed" || i.dataChannel === "closed") {
    return {
      id: "connection_failed",
      note: "Fixed text after 4 tries. The reason goes to the console.",
    };
  }
  if (i.videoPause !== "none") {
    return { id: "video_paused", note: "The text names the reason." };
  }
  if (i.hdmi !== "locked") {
    return {
      id: "no_hdmi",
      note: "One text for no cable, a sleeping computer and a too-high resolution.",
    };
  }
  if (i.video !== "flowing") {
    return { id: "loading_video", note: "The spinner stays." };
  }
  if (i.playback === "blocked") {
    return { id: "autoplay", note: "A button starts the video." };
  }
  const audioChecked = i.audioSetting === "on" && i.usb === "configured";
  if (i.usb !== "configured" || i.keyboardTest === "no_reply") {
    return {
      id: "video",
      note: "The picture shows. Nothing says that the keyboard does not work.",
    };
  }
  if (
    audioChecked &&
    (i.usbAudio === "off" || i.audioSource !== "streaming" || i.audioLevel === "silent")
  ) {
    return { id: "video", note: "The picture shows. Nothing says why there is no sound." };
  }
  if (audioChecked && i.audioPlayback === "blocked") {
    return { id: "video", note: "The picture shows, with a banner to turn on sound." };
  }
  if (i.loss === "high" || i.delay === "high" || i.link === "wifi_weak") {
    return {
      id: "video",
      note: "The picture shows. Quality is only in the Connection Stats sidebar.",
    };
  }
  return { id: "video", note: "The picture shows." };
}

// Named cases for the gallery and the tests: the changes from the healthy
// state, and the headline they must give (null: no problem, no wait state).
export type SymptomId = "connect" | "picture" | "input" | "sound" | "notes";

export const SYMPTOMS: { id: SymptomId; title: string }[] = [
  { id: "connect", title: "Cannot connect" },
  { id: "picture", title: "No picture" },
  { id: "input", title: "Keyboard and mouse do not work" },
  { id: "sound", title: "No sound" },
  { id: "notes", title: "It works, with a note" },
];

export interface Scenario {
  id: string;
  symptom: SymptomId;
  // The situation, as the user would describe it.
  name: string;
  set: InputChange;
  headline: FindingId | null;
  // Findings that must also be there.
  also?: FindingId[];
}

const scenario = (
  symptom: SymptomId,
  id: string,
  name: string,
  set: InputChange,
  headline: FindingId | null,
  also?: FindingId[],
): Scenario => ({ id, symptom, name, set, headline, also });

export const SCENARIOS: Scenario[] = [
  scenario(
    "connect",
    "firefox-no-h264",
    "Firefox without the H.264 plugin",
    { browser: "firefox", h264: "missing" },
    "no_h264",
  ),
  scenario(
    "connect",
    "webrtc-off",
    "Firefox with WebRTC turned off",
    { browser: "firefox", webrtcApi: "missing" },
    "webrtc_off",
  ),
  scenario(
    "connect",
    "firefox-no-host",
    "Firefox with a privacy setting that hides addresses",
    { browser: "firefox", candidates: "none" },
    "no_candidates",
  ),
  scenario(
    "connect",
    "chrome-ip-policy",
    "Chrome with the WebRTC IP policy limited",
    { candidates: "none" },
    "no_candidates",
  ),
  scenario("connect", "vpn-blocks-udp", "A VPN is on", { ice: "failed" }, "udp_blocked"),
  scenario(
    "connect",
    "cloud-no-path",
    "Cloud: an office firewall blocks video",
    { access: "cloud", ice: "failed" },
    "udp_blocked",
  ),
  scenario(
    "connect",
    "mini-unreachable",
    "The JetKVM lost power or left the network",
    { address: "ip", http: "unreachable", socket: "blocked" },
    "mini_unreachable",
  ),
  scenario(
    "connect",
    "mini-offline",
    "Cloud: the JetKVM is offline",
    { access: "cloud", cloudDevice: "offline" },
    "mini_offline",
  ),
  scenario(
    "connect",
    "cloud-unreachable",
    "Cloud: this computer has no internet",
    { access: "cloud", http: "unreachable" },
    "cloud_unreachable",
  ),
  scenario(
    "connect",
    "safari-ip",
    "Safari, opened by IP address",
    { browser: "safari", address: "ip", socket: "blocked" },
    "safari_ip",
  ),
  scenario(
    "connect",
    "proxy-no-websocket",
    "Behind a proxy that stops WebSockets",
    { socket: "blocked" },
    "socket_blocked",
  ),
  scenario(
    "connect",
    "session-taken",
    "Opened in a second browser",
    { session: "taken_over" },
    "session_taken",
  ),
  scenario("connect", "no-answer", "The JetKVM is stuck", { answer: "none" }, "no_answer"),
  scenario(
    "connect",
    "answer-refused",
    "The browser refuses the JetKVM's answer",
    { answer: "rejected" },
    "answer_refused",
  ),
  scenario(
    "connect",
    "no-dtls",
    "A path exists, the secure channel does not start",
    { dataChannel: "closed" },
    "no_secure_channel",
  ),
  scenario(
    "connect",
    "rebooting",
    "The JetKVM restarts",
    { miniActivity: "rebooting", http: "unreachable", socket: "blocked" },
    "rebooting",
  ),
  scenario(
    "connect",
    "vpn-and-asleep",
    "A VPN is on and the computer is asleep",
    { ice: "failed", hdmi: "no_signal", usb: "suspended" },
    "udp_blocked",
  ),

  scenario(
    "picture",
    "computer-asleep",
    "The computer is asleep",
    { hdmi: "no_signal", usb: "suspended" },
    "computer_asleep",
    ["usb_follows_computer"],
  ),
  scenario(
    "picture",
    "computer-off",
    "The computer is off",
    { hdmi: "no_cable", usb: "not_attached" },
    "computer_off",
    ["usb_follows_computer"],
  ),
  scenario(
    "picture",
    "computer-off-5v",
    "The computer is off, its HDMI port keeps power",
    { hdmi: "no_signal", usb: "not_attached" },
    "computer_off",
  ),
  scenario(
    "picture",
    "no-hdmi-cable",
    "The HDMI cable is not connected",
    { hdmi: "no_cable" },
    "no_hdmi_cable",
  ),
  scenario(
    "picture",
    "display-off",
    "The computer is on, its display is asleep or off",
    { hdmi: "no_signal" },
    "no_picture",
  ),
  scenario(
    "picture",
    "edid-rejected",
    "No picture after a change to a custom EDID",
    { hdmi: "no_signal", edid: "custom" },
    "no_picture_edid",
  ),
  scenario(
    "picture",
    "mode-4k",
    "The computer sends 4K",
    { hdmi: "unsupported_mode" },
    "unsupported_mode",
  ),
  scenario(
    "picture",
    "mode-4k-edid",
    "The computer sends 4K because of a custom EDID",
    { hdmi: "unsupported_mode", edid: "custom" },
    "unsupported_mode",
  ),
  scenario(
    "picture",
    "mini-no-video",
    "The JetKVM has a picture and sends none",
    { video: "not_sent" },
    "mini_sends_no_video",
  ),
  scenario(
    "picture",
    "no-decode",
    "Firefox with hardware H.264 off",
    { browser: "firefox", video: "not_decoded" },
    "browser_decodes_no_video",
  ),
  scenario(
    "picture",
    "autoplay",
    "The browser blocks autoplay",
    { playback: "blocked" },
    "autoplay_blocked",
  ),
  scenario(
    "picture",
    "paused-update",
    "An update is installing",
    { videoPause: "update" },
    "video_paused",
  ),
  scenario(
    "picture",
    "paused-upload",
    "A file upload is running",
    { videoPause: "upload" },
    "video_paused",
  ),
  scenario(
    "picture",
    "mode-and-usb-audio",
    "4K, and USB Audio is off",
    { hdmi: "unsupported_mode", usbAudio: "off" },
    "unsupported_mode",
    ["usb_audio_off"],
  ),

  scenario(
    "input",
    "usb-wrong-port",
    "USB is in the wrong port, or the cable carries power only",
    { usb: "not_attached" },
    "usb_not_attached",
  ),
  scenario(
    "input",
    "usb-bios-rejects",
    "A BIOS or KVM switch rejects the USB device",
    { usb: "attached" },
    "usb_not_configured",
  ),
  scenario(
    "input",
    "keyboard-dead",
    "USB is connected, keys do nothing",
    { keyboardTest: "no_reply" },
    "keyboard_no_reply",
  ),
  scenario("input", "usb-low-power", "The computer put USB to sleep", { usb: "suspended" }, null, [
    "usb_suspended",
  ]),

  scenario(
    "sound",
    "usb-audio-off",
    "Audio is on, the USB Audio device is off",
    { usbAudio: "off" },
    "usb_audio_off",
  ),
  scenario(
    "sound",
    "audio-not-ready",
    "The computer did not set up the audio device",
    { audioSource: "not_ready" },
    "audio_device_not_ready",
  ),
  scenario(
    "sound",
    "audio-other-output",
    "The computer plays to another output",
    { audioSource: "idle" },
    null,
    ["audio_no_stream"],
  ),
  scenario("sound", "audio-muted", "The computer is muted", { audioLevel: "silent" }, null, [
    "audio_silent",
  ]),
  scenario(
    "sound",
    "audio-blocked",
    "The browser blocks sound",
    { audioPlayback: "blocked" },
    "audio_blocked",
  ),
  scenario("sound", "audio-off", "Audio is off", { audioSetting: "off" }, null, ["audio_off"]),

  scenario("notes", "healthy", "Everything works", {}, null),
  scenario(
    "notes",
    "wifi-weak",
    "Weak Wi-Fi at the JetKVM",
    { link: "wifi_weak", loss: "high" },
    null,
    ["wifi_weak", "packet_loss"],
  ),
  scenario(
    "notes",
    "relayed",
    "Cloud through a relay, slow",
    { access: "cloud", route: "relay", delay: "high" },
    null,
    ["relayed", "high_delay"],
  ),
  scenario(
    "notes",
    "restart-power",
    "The JetKVM lost power a moment ago",
    { restart: "power" },
    null,
    ["restart_power"],
  ),
  scenario(
    "notes",
    "restart-brownout",
    "The JetKVM restarted from weak power",
    { restart: "brownout" },
    null,
    ["restart_brownout"],
  ),
  scenario(
    "notes",
    "restart-fault",
    "The JetKVM restarted after a fault",
    { restart: "fault" },
    null,
    ["restart_fault"],
  ),
  scenario("notes", "clock-not-set", "The JetKVM's clock is not set", { clock: "not_set" }, null, [
    "clock_not_set",
  ]),
];
