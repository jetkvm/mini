// Connection diagnosis: from a set of signals to one headline and a list of
// findings. This module is pure (no React, no browser API, no imports), so
// the lab page (routes/dev.diagnosis.tsx) can feed it mocked signals and
// tests/diagnosis.mjs can run it in Node.
//
// The signals are a mock for now: nothing collects them from a real browser
// or a real JetKVM yet. `have` on each signal says how far the product is
// from having it.

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

// Who can measure the signal.
export type Source = "browser" | "mini" | "cloud" | "test";

// How far the product is from having the signal:
//   ui        the web UI reads it today
//   discarded the web UI sees it today and only logs it to the console
//   firmware  the firmware has it (local HTTP or log), the web UI does not read it
//   new       needs new code
export type Have = "ui" | "discarded" | "firmware" | "new";

interface SignalDef {
  check: CheckId | "context";
  label: string;
  source: Source;
  have: Have;
  // The first option is the healthy value.
  options: Record<string, string>;
}

export const SIGNALS = {
  // Context: not checks, they select the wording and the path.
  access: {
    check: "context",
    label: "Access path",
    source: "browser",
    have: "ui",
    options: { local: "Local network", cloud: "JetKVM Cloud" },
  },
  browser: {
    check: "context",
    label: "Browser",
    source: "browser",
    have: "ui",
    options: { chromium: "Chrome, Edge", firefox: "Firefox", safari: "Safari" },
  },
  address: {
    check: "context",
    label: "Page opened by",
    source: "browser",
    have: "new",
    options: { name: "Host name", ip: "IP address" },
  },

  webrtcApi: {
    check: "browser",
    label: "WebRTC in the browser",
    source: "browser",
    have: "discarded",
    options: { present: "Present", missing: "Turned off" },
  },
  h264: {
    check: "browser",
    label: "H.264 decoder",
    source: "browser",
    have: "new",
    options: { supported: "Supported", missing: "Missing" },
  },
  candidates: {
    check: "browser",
    label: "Network addresses the browser shares",
    source: "browser",
    have: "new",
    options: { gathered: "Shared", none: "None" },
  },

  miniActivity: {
    check: "reach",
    label: "JetKVM activity",
    source: "mini",
    have: "ui",
    options: { normal: "Normal", rebooting: "Restarting" },
  },
  http: {
    check: "reach",
    label: "HTTP request to the JetKVM or the cloud",
    source: "browser",
    have: "new",
    options: { ok: "Answers", unreachable: "No answer" },
  },
  cloudDevice: {
    check: "reach",
    label: "JetKVM in the cloud (cloud only)",
    source: "cloud",
    have: "ui",
    options: { online: "Online", offline: "Offline" },
  },
  socket: {
    check: "reach",
    label: "Signalling WebSocket",
    source: "browser",
    have: "discarded",
    options: { open: "Opens", blocked: "Does not open" },
  },
  session: {
    check: "reach",
    label: "Session owner",
    source: "mini",
    have: "ui",
    options: { ours: "This browser", taken_over: "Another browser" },
  },
  answer: {
    check: "reach",
    label: "Answer to the connection request",
    source: "browser",
    have: "discarded",
    options: { ok: "Received", none: "None in 10 s", rejected: "Browser refused it" },
  },

  ice: {
    check: "connection",
    label: "Network path for video (ICE)",
    source: "browser",
    have: "discarded",
    options: { connected: "Found", failed: "None found" },
  },
  route: {
    check: "connection",
    label: "Route",
    source: "browser",
    have: "ui",
    options: { direct: "Direct", relay: "Relayed" },
  },
  dataChannel: {
    check: "connection",
    label: "Secure channel (DTLS, data channel)",
    source: "browser",
    have: "discarded",
    options: { open: "Open", closed: "Does not open" },
  },

  restart: {
    check: "health",
    label: "Restart in the last 5 minutes",
    source: "mini",
    have: "firmware",
    options: { none: "None", power: "Power loss", brownout: "Voltage drop", fault: "Fault" },
  },
  clock: {
    check: "health",
    label: "JetKVM clock",
    source: "mini",
    have: "new",
    options: { set: "Set", not_set: "Not set" },
  },
  link: {
    check: "health",
    label: "JetKVM network link",
    source: "mini",
    have: "ui",
    options: { ethernet: "Ethernet", wifi_good: "Wi-Fi, good", wifi_weak: "Wi-Fi, weak" },
  },

  videoPause: {
    check: "picture",
    label: "Video paused by the JetKVM",
    source: "mini",
    have: "ui",
    options: { none: "No", update: "Update", download: "Download", upload: "Upload" },
  },
  hdmi: {
    check: "picture",
    label: "HDMI input",
    source: "mini",
    have: "firmware",
    options: {
      locked: "Signal locked",
      no_cable: "No 5 V (no cable)",
      no_signal: "5 V, no signal",
      unsupported_mode: "Unsupported mode",
    },
  },
  edid: {
    check: "picture",
    label: "EDID",
    source: "mini",
    have: "ui",
    options: { default: "JetKVM default", custom: "Other" },
  },
  video: {
    check: "picture",
    label: "Video frames",
    source: "browser",
    have: "firmware",
    options: {
      flowing: "Sent and decoded",
      not_sent: "JetKVM sends none",
      not_decoded: "Browser decodes none",
    },
  },
  playback: {
    check: "picture",
    label: "Video playback",
    source: "browser",
    have: "ui",
    options: { playing: "Plays", blocked: "Autoplay blocked" },
  },

  usb: {
    check: "input",
    label: "USB state",
    source: "mini",
    have: "ui",
    options: {
      configured: "Configured",
      suspended: "Suspended",
      attached: "Attached, not configured",
      not_attached: "Not attached",
    },
  },
  keyboardTest: {
    check: "input",
    label: "Caps Lock test",
    source: "test",
    have: "new",
    options: { reply: "Computer replies", no_reply: "No reply" },
  },

  audioSetting: {
    check: "audio",
    label: "Audio setting",
    source: "mini",
    have: "ui",
    options: { on: "On", off: "Off" },
  },
  usbAudio: {
    check: "audio",
    label: "USB Audio in USB Devices",
    source: "mini",
    have: "ui",
    options: { on: "On", off: "Off" },
  },
  audioSource: {
    check: "audio",
    label: "Audio from the computer",
    source: "mini",
    have: "firmware",
    options: {
      streaming: "Streams",
      idle: "Device set up, no stream",
      not_ready: "Device not set up",
    },
  },
  audioLevel: {
    check: "audio",
    label: "Audio level",
    source: "mini",
    have: "new",
    options: { signal: "Has sound", silent: "Silent" },
  },
  audioPlayback: {
    check: "audio",
    label: "Audio playback",
    source: "browser",
    have: "ui",
    options: { playing: "Plays", blocked: "Blocked" },
  },

  loss: {
    check: "quality",
    label: "Packet loss",
    source: "browser",
    have: "ui",
    options: { low: "Low", high: "High" },
  },
  delay: {
    check: "quality",
    label: "Round-trip time",
    source: "browser",
    have: "ui",
    options: { low: "Low", high: "High" },
  },
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

export interface Finding {
  id: FindingId;
  check: CheckId;
  level: Level;
  // What is wrong, one sentence.
  title: string;
  // The evidence: which signals led here.
  detail: string;
  // What the user does.
  fix?: string;
  // A button the panel offers.
  action?: string;
  // What the panel watches to confirm the fix by itself.
  watch?: string;
  // The signal changes that the fix makes. The lab applies them to simulate
  // the action or the user's manual step.
  resolves?: InputChange;
  // Issues that show this case.
  refs?: string;
}

export type CheckStatus = Level | "pass" | "skipped";

export interface CheckResult {
  id: CheckId;
  title: string;
  status: CheckStatus;
  // One line for the check list.
  summary: string;
  findings: Finding[];
}

export interface Diagnosis {
  checks: CheckResult[];
  // The one thing the panel leads with: the first problem or wait state.
  headline: Finding | null;
  // Every other finding, in check order.
  others: Finding[];
}

const MINI_MAX_MODE = "1920×1080 at 30 Hz or 1280×720 at 60 Hz";

function browserFindings(i: DiagnosisInput): Finding[] {
  if (i.webrtcApi === "missing") {
    return [
      {
        id: "webrtc_off",
        check: "browser",
        level: "problem",
        title: "WebRTC is turned off in this browser.",
        detail: "The browser has no RTCPeerConnection. The JetKVM sends video only over WebRTC.",
        fix:
          i.browser === "firefox"
            ? "Open about:config and set media.peerconnection.enabled to true."
            : "Turn off the extension or the setting that blocks WebRTC, then reload.",
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
        title: "This browser cannot play H.264 video.",
        detail:
          "H.264 is not in the browser's list of WebRTC codecs. The JetKVM Mini sends H.264 only.",
        fix:
          i.browser === "firefox"
            ? "Open about:addons, select Plugins and enable OpenH264. On Linux, the Firefox from Flathub has it."
            : "Use a current Chrome, Edge, Firefox or Safari.",
        resolves: { h264: "supported" },
        refs: "mini #4, kvm #655",
      },
    ];
  }
  if (i.candidates === "none") {
    const fix: Record<DiagnosisInput["browser"], string> = {
      firefox: "Open about:config and set media.peerconnection.ice.no_host to false.",
      chromium:
        'Open chrome://flags/#webrtc-ip-handling-policy and select "Default". In Vivaldi or Brave, turn on the WebRTC IP setting. Turn off extensions that protect against WebRTC leaks.',
      safari: "Turn off extensions or content blockers that limit WebRTC.",
    };
    return [
      {
        id: "no_candidates",
        check: "browser",
        level: "problem",
        title: "The browser does not share a network address for video.",
        detail:
          "A test connection gathered no ICE candidate. A privacy setting, an extension or a VPN hides them.",
        fix: fix[i.browser],
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
        title: "The JetKVM is restarting.",
        detail: "The JetKVM said that it restarts. This takes 20 to 30 seconds.",
        watch: "the JetKVM to answer again",
        resolves: { miniActivity: "normal" },
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
          title: "This browser cannot reach JetKVM Cloud.",
          detail: "A request to the cloud API got no answer.",
          fix: "Check the internet connection of this computer.",
          watch: "the cloud to answer",
          resolves: { http: "ok" },
        },
      ];
    }
    return [
      {
        id: "mini_unreachable",
        check: "reach",
        level: "problem",
        title: "The JetKVM does not answer.",
        detail:
          "A request to this page's address got no answer. The browser cannot tell power loss from a network fault.",
        fix: "Look at the JetKVM's display. If it is dark, check the power. If it shows another address, open that address. If the address is the same, put this computer on the same network.",
        watch: "the JetKVM to answer",
        resolves: { http: "ok" },
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
        title: "The JetKVM is not connected to the cloud.",
        detail: "JetKVM Cloud has no link from this JetKVM.",
        fix: "Look at the JetKVM's display: it needs power and a network address. If it has both, its clock may not be set, or its network blocks the cloud.",
        watch: "the JetKVM to come online",
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
          title: "Safari cannot open the connection by IP address.",
          detail: "The page loads, but Safari 26 never starts the WebSocket to an IP address.",
          fix: "Open the JetKVM by its host name, or use another browser.",
          action: "Open by host name",
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
        title: "Something blocks the live connection to the JetKVM.",
        detail: "The page loads over HTTP, but the WebSocket for signalling does not open.",
        fix: "A proxy, security software or an extension stops WebSockets. Try a private window, or go around the proxy.",
        action: "Try again",
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
        title: "Another browser uses this JetKVM.",
        detail: "The JetKVM has one session at a time. Another browser took it.",
        action: "Use here",
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
        title: "The JetKVM does not answer the connection request.",
        detail: "The JetKVM accepted the request and sent no answer in 10 seconds, four times.",
        fix: "The JetKVM is busy or stuck. Disconnect its power for a moment.",
        action: "Try again",
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
        title: "The browser refused the JetKVM's answer.",
        detail:
          "The browser reported an error for the answer. This is a firmware fault or a rare browser setup.",
        fix: "Try another browser. Send a report, so that the fault can be corrected.",
        action: "Try again",
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
        title:
          i.access === "cloud"
            ? "No network path for video, direct or relayed."
            : "The network blocks video between this browser and the JetKVM.",
        detail:
          i.access === "cloud"
            ? "Signalling works, but no ICE candidate pair connected, also not through the relay."
            : "The page loads, but video uses UDP and no UDP packet got through.",
        fix:
          i.access === "cloud"
            ? "A firewall or VPN blocks UDP and the relay. Disconnect the VPN or try another network."
            : "Disconnect the VPN. If the JetKVM is on another VLAN or behind a proxy or an SSH tunnel, allow UDP to it.",
        action: "Try again",
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
        title: "The secure channel to the JetKVM does not start.",
        detail: "A network path exists, but the encrypted channel did not open in 10 seconds.",
        fix: "This is a fault in the JetKVM. Disconnect its power for a moment. Send a report if it returns.",
        action: "Try again",
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
      title: "The JetKVM started a few minutes ago after a power loss.",
      detail: "Reset reason: power on. Uptime is below 5 minutes.",
      fix: "If you did not unplug it: the JetKVM takes power from the computer. Connect a charger to the EXT port to keep it on while the computer is off.",
    });
  }
  if (i.restart === "brownout") {
    findings.push({
      id: "restart_brownout",
      check: "health",
      level: "warning",
      title: "The JetKVM restarted because its supply voltage dropped.",
      detail: "Reset reason: brownout. Uptime is below 5 minutes.",
      fix: "Use a shorter USB cable or another USB port, or connect a charger to the EXT port.",
    });
  }
  if (i.restart === "fault") {
    findings.push({
      id: "restart_fault",
      check: "health",
      level: "warning",
      title: "The JetKVM restarted after a fault.",
      detail: "Reset reason: panic or watchdog. Uptime is below 5 minutes.",
      fix: "Send a report if this happens again.",
    });
  }
  if (i.clock === "not_set") {
    findings.push({
      id: "clock_not_set",
      check: "health",
      level: "warning",
      title: "The JetKVM's clock is not set.",
      detail:
        "The JetKVM got no time from the network. Cloud access and updates need the correct time.",
      fix: "Let the JetKVM reach time.apple.com (UDP port 123).",
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
    const why: Record<Exclude<DiagnosisInput["videoPause"], "none">, string> = {
      update: "installs an update",
      download: "saves the OS Services installer to its card",
      upload: "saves a file to its card",
    };
    return [
      {
        id: "video_paused",
        check: "picture",
        level: "wait",
        title: `Video is paused while the JetKVM ${why[i.videoPause]}.`,
        detail: "The JetKVM stops video for this work, to keep enough memory free.",
        watch: "the work to end",
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
          title: "The computer is off or not connected.",
          detail:
            i.hdmi === "no_cable"
              ? "The HDMI input has no 5 V and USB is not attached."
              : "The HDMI input has no signal and USB is not attached.",
          fix: "Turn on the computer. If it is on, check the HDMI cable and the USB cable.",
          action: "Wake on LAN",
          watch: "HDMI signal and USB",
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
          title: "The HDMI cable is not connected.",
          detail: "USB is connected, but the HDMI input has no 5 V from the computer.",
          fix: "Connect the HDMI cable to a video output of the computer. For DisplayPort or USB-C, use an active adapter.",
          watch: "HDMI signal",
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
          title: "The computer is asleep.",
          detail: "The HDMI cable is connected and has no signal. USB is suspended.",
          action: "Wake computer",
          watch: "HDMI signal",
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
          title: "The computer is on and sends no picture.",
          detail:
            "The HDMI cable is connected, USB works, and there is no signal. The EDID is not the JetKVM default, and a wrong EDID can cause this.",
          action: "Use the default EDID",
          watch: "HDMI signal",
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
        title: "The computer is on and sends no picture.",
        detail:
          "The HDMI cable is connected, USB works, and there is no signal. The display output is asleep or turned off.",
        fix: "If the wake does not help: on a laptop, turn on the external display. With an adapter, use an active one.",
        action: "Wake display",
        watch: "HDMI signal",
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
        title: "The computer sends a video mode that the JetKVM Mini cannot capture.",
        detail: `Detected 3840×2160 at 60 Hz. The maximum is ${MINI_MAX_MODE}.`,
        fix:
          i.edid === "custom"
            ? "The EDID is not the JetKVM default and offers this mode to the computer."
            : "Set the display resolution on the computer to 1920×1080 or lower.",
        action: i.edid === "custom" ? "Use the default EDID" : undefined,
        watch: "a supported mode",
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
        title: "The JetKVM has a picture and sends no video.",
        detail: "The HDMI signal is locked, but the count of sent frames does not increase.",
        fix: "This is a fault in the JetKVM. A restart corrects it.",
        action: "Restart the JetKVM",
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
        title: "Video arrives, but the browser does not decode it.",
        detail: "Video packets arrive and the count of decoded frames stays at 0.",
        fix:
          i.browser === "firefox"
            ? "Open about:config and set media.webrtc.hw.h264.enabled to true. Then restart Firefox."
            : "Turn on hardware acceleration in the browser settings. Then restart the browser.",
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
        title: "The browser blocks video autoplay.",
        detail: "The browser refused to start the video without a click.",
        fix: "To skip this next time, allow autoplay for this address in the browser settings.",
        action: "Start video",
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
        title:
          computer === "asleep"
            ? "USB is suspended because the computer is asleep."
            : "USB is not attached because the computer is off or not connected.",
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
        title: "The computer does not see the JetKVM on USB.",
        detail: "The computer is on (it sends a picture), and USB is not attached.",
        fix: "Connect the USB-C port with the monitor icon to the computer. Use a cable that carries data, and no hub or splitter.",
        watch: "USB",
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
        title: "The computer found the JetKVM on USB and did not set it up.",
        detail:
          "USB is attached and not configured. Some BIOS versions and KVM switches refuse a combined USB device.",
        fix: "In USB Devices, select keyboard and mouse only.",
        action: "Open USB settings",
        watch: "USB",
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
        title: "USB is in low power mode.",
        detail: "The computer suspended the USB port. The first key or mouse move wakes it.",
      },
    ];
  }
  if (i.keyboardTest === "no_reply") {
    return [
      {
        id: "keyboard_no_reply",
        check: "input",
        level: "problem",
        title: "USB is connected, but the computer does not react to the keyboard.",
        detail:
          "The JetKVM pressed Caps Lock two times and the computer did not report the LED change.",
        fix: "In USB Devices, select keyboard and mouse only. In a BIOS screen without USB keyboard support, no fix exists.",
        action: "Open USB settings",
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
        title: "Audio is off.",
        detail: "The Audio setting of the JetKVM is off.",
        action: "Turn on audio",
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
        title: "USB Audio is off in USB Devices.",
        detail: "Audio is on, but the JetKVM does not present an audio device to the computer.",
        action: "Turn on USB Audio",
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
        title: "The computer did not set up the JetKVM audio device.",
        detail: "USB is configured, but the computer did not start the audio interface.",
        fix: "Disconnect and connect the USB cable. Check that the computer lists JetKVM as an audio device.",
        watch: "the audio device",
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
        title: "The computer sends no audio to the JetKVM.",
        detail:
          "The audio device is set up and no audio stream arrives. This is normal while nothing plays.",
        fix: "If something plays: on the computer, select JetKVM as the audio output.",
        watch: "audio from the computer",
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
        title: "Audio arrives, but it is silent.",
        detail: "The computer streams audio with a level of zero.",
        fix: "Check the volume and the mute state on the computer.",
        watch: "the audio level",
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
        title: "The browser blocks audio.",
        detail: "The browser refused to start audio without a click.",
        action: "Enable audio",
        resolves: { audioPlayback: "playing" },
      },
    ];
  }
  return [];
}

function qualityFindings(i: DiagnosisInput): Finding[] {
  const findings: Finding[] = [];
  if (i.route === "relay") {
    findings.push({
      id: "relayed",
      check: "quality",
      level: "info",
      title: "The connection goes through a relay.",
      detail:
        "No direct path exists between this browser and the JetKVM, so video goes through Cloudflare. Expect more delay.",
    });
  }
  if (i.link === "wifi_weak") {
    findings.push({
      id: "wifi_weak",
      check: "quality",
      level: "warning",
      title: "The Wi-Fi signal at the JetKVM is weak.",
      detail: "Signal strength is −78 dBm. Below −70 dBm, video stutters.",
      fix: "Move the JetKVM or the access point, or use the 2.4 GHz band for more range.",
      watch: "the signal strength",
      resolves: { link: "wifi_good" },
    });
  }
  if (i.loss === "high") {
    findings.push({
      id: "packet_loss",
      check: "quality",
      level: "warning",
      title: "The network loses video packets.",
      detail: "More than 2% of the video packets do not arrive.",
      fix:
        i.link === "wifi_weak"
          ? "The weak Wi-Fi at the JetKVM is the probable cause."
          : "Check the network between this computer and the JetKVM. On Wi-Fi, move closer to the access point.",
    });
  }
  if (i.delay === "high") {
    findings.push({
      id: "high_delay",
      check: "quality",
      level: "warning",
      title: "The network delay is high.",
      detail: "The round trip to the JetKVM takes more than 150 ms.",
      fix:
        i.route === "relay"
          ? "The relay adds delay. A direct path (same network, or Tailscale) removes it."
          : "Check the network between this computer and the JetKVM.",
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
  let chainStop: string | null = null;
  const chain: [CheckId, () => Finding[], string, string][] = [
    [
      "browser",
      () => browserFindings(i),
      "WebRTC and H.264 are available.",
      "Needs a browser that can connect.",
    ],
    [
      "reach",
      () => reachFindings(i),
      i.access === "cloud" ? "The JetKVM is online and answers." : "The JetKVM answers.",
      "Needs a JetKVM that answers.",
    ],
    [
      "connection",
      () => connectionFindings(i),
      i.route === "relay" ? "Connected through a relay." : "Connected on a direct path.",
      "Needs a connection.",
    ],
  ];
  for (const [id, find, passSummary, stopReason] of chain) {
    if (chainStop) {
      skip(id, chainStop);
    } else if (stops(run(id, find(), passSummary))) {
      chainStop = stopReason;
    }
  }

  if (chainStop) {
    for (const id of ["health", "picture", "input", "audio", "quality"] as const) {
      skip(id, "Needs a connection.");
    }
  } else {
    // With a connection, the JetKVM reports its state, and these checks do
    // not depend on each other, except where noted.
    run("health", healthFindings(i), "No recent restart. The clock is set.");

    const picture = run("picture", pictureFindings(i), "The picture arrives and plays.");
    const input = run("input", inputFindings(i), "The computer replied to the Caps Lock test.");

    // Audio comes over USB, so it needs a configured USB device. With audio
    // off, there is nothing to check and the check only says so.
    if (i.audioSetting === "on" && (i.usb !== "configured" || stops(input))) {
      skip("audio", "Needs a working USB connection to the computer.");
    } else {
      run("audio", audioFindings(i), "Audio arrives and plays.");
    }

    if (stops(picture)) {
      skip("quality", "Needs a picture.");
    } else {
      run("quality", qualityFindings(i), "Low packet loss and low delay.");
    }
  }

  const all = checks.flatMap(check => check.findings);
  const headline =
    all.find(finding => finding.level === "problem") ??
    all.find(finding => finding.level === "wait") ??
    null;
  return { checks, headline, others: all.filter(finding => finding !== headline) };
}

// The screen the web UI shows today for the same signals. This is a model of
// routes/devices.$id.tsx, WebRTCVideo.tsx and useWebSocketSignaling.ts, for
// the lab's side-by-side view; the product does not use it.
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
  // Why this screen, and what it leaves out.
  note: string;
}

export function todayScreen(i: DiagnosisInput): TodayScreen {
  if (i.webrtcApi === "missing") {
    return {
      id: "connection_failed",
      note: "Creating the peer connection throws; the fixed failure text shows at once.",
    };
  }
  if (i.miniActivity === "rebooting") {
    return { id: "rebooting", note: "Shown after the JetKVM announces a restart." };
  }
  if (i.http === "unreachable" || i.socket === "blocked") {
    return {
      id: "connecting",
      note: "The socket is retried up to 2,000 times with up to 10 s between tries. The spinner stays for hours.",
    };
  }
  if (i.access === "cloud" && i.cloudDevice === "offline") {
    return {
      id: "connecting",
      note: "The client has no timeout while it waits for the JetKVM. The spinner stays.",
    };
  }
  if (i.session === "taken_over") {
    return { id: "other_session", note: "A dialog offers to take the session back." };
  }
  if (i.h264 === "missing") {
    return {
      id: "connection_failed",
      note: "The browser refuses the answer; after 4 tries the fixed failure text shows (mini #4).",
    };
  }
  if (i.answer === "none") {
    return { id: "connection_failed", note: "4 tries of 10 s each, then the fixed failure text." };
  }
  if (i.answer === "rejected") {
    return {
      id: "connection_failed",
      note: "4 tries, then the fixed failure text. The error goes to the console.",
    };
  }
  if (i.candidates === "none") {
    return {
      id: "connection_failed",
      note: "No data channel in 10 s, 4 tries, then the fixed failure text (mini #3).",
    };
  }
  if (i.ice === "failed" || i.dataChannel === "closed") {
    return {
      id: "connection_failed",
      note: "4 tries, then the fixed failure text. The reason goes to the console.",
    };
  }
  if (i.videoPause !== "none") {
    return { id: "video_paused", note: "The text names the reason." };
  }
  if (i.hdmi !== "locked") {
    return {
      id: "no_hdmi",
      note: "The firmware reports every HDMI state as no_signal, so one text covers no cable, a sleeping computer and an unsupported mode.",
    };
  }
  if (i.video !== "flowing") {
    return {
      id: "loading_video",
      note: "The video element never fires 'playing'. The spinner stays.",
    };
  }
  if (i.playback === "blocked") {
    return { id: "autoplay", note: "A button starts the video." };
  }
  const silent: string[] = [];
  if (i.usb !== "configured") silent.push("USB shows only as a badge in the header");
  if (i.usb === "configured" && i.keyboardTest === "no_reply")
    silent.push("nothing shows that the keyboard is dead");
  if (i.audioSetting === "on" && i.usb === "configured") {
    if (i.usbAudio === "off" || i.audioSource !== "streaming" || i.audioLevel === "silent") {
      silent.push("nothing shows why there is no sound");
    } else if (i.audioPlayback === "blocked") {
      silent.push("a banner offers to enable audio");
    }
  }
  if (i.loss === "high" || i.delay === "high" || i.link === "wifi_weak") {
    silent.push("quality shows only in the Connection Stats sidebar");
  }
  return {
    id: "video",
    note: silent.length > 0 ? `The picture shows; ${silent.join("; ")}.` : "The picture shows.",
  };
}

// Named cases for the lab and the tests: the changes from the healthy state,
// and the headline they must give (null: no problem and no wait state).
export interface Scenario {
  id: string;
  check: CheckId | "healthy";
  name: string;
  set: InputChange;
  headline: FindingId | null;
  // Findings that must also be there.
  also?: FindingId[];
}

export const SCENARIOS: Scenario[] = [
  { id: "healthy", check: "healthy", name: "All good", set: {}, headline: null },

  {
    id: "webrtc-off",
    check: "browser",
    name: "Firefox with WebRTC turned off",
    set: { browser: "firefox", webrtcApi: "missing" },
    headline: "webrtc_off",
  },
  {
    id: "firefox-no-h264",
    check: "browser",
    name: "Firefox without OpenH264 (mini #4)",
    set: { browser: "firefox", h264: "missing" },
    headline: "no_h264",
  },
  {
    id: "firefox-no-host",
    check: "browser",
    name: "Firefox with ice.no_host (mini #3)",
    set: { browser: "firefox", candidates: "none" },
    headline: "no_candidates",
  },
  {
    id: "chrome-ip-policy",
    check: "browser",
    name: "Chrome with the WebRTC IP policy limited",
    set: { candidates: "none" },
    headline: "no_candidates",
  },

  {
    id: "rebooting",
    check: "reach",
    name: "The JetKVM restarts",
    set: { miniActivity: "rebooting", http: "unreachable" },
    headline: "rebooting",
  },
  {
    id: "mini-unreachable",
    check: "reach",
    name: "The JetKVM has no power or left the network",
    set: { address: "ip", http: "unreachable", socket: "blocked" },
    headline: "mini_unreachable",
  },
  {
    id: "cloud-unreachable",
    check: "reach",
    name: "Cloud: this computer is offline",
    set: { access: "cloud", http: "unreachable" },
    headline: "cloud_unreachable",
  },
  {
    id: "mini-offline",
    check: "reach",
    name: "Cloud: the JetKVM is offline",
    set: { access: "cloud", cloudDevice: "offline" },
    headline: "mini_offline",
  },
  {
    id: "safari-ip",
    check: "reach",
    name: "Safari 26 by IP address (kvm #1098)",
    set: { browser: "safari", address: "ip", socket: "blocked" },
    headline: "safari_ip",
  },
  {
    id: "proxy-no-websocket",
    check: "reach",
    name: "A proxy that passes HTTP and no WebSocket",
    set: { socket: "blocked" },
    headline: "socket_blocked",
  },
  {
    id: "session-taken",
    check: "reach",
    name: "Another browser took the session",
    set: { session: "taken_over" },
    headline: "session_taken",
  },
  {
    id: "no-answer",
    check: "reach",
    name: "The JetKVM is stuck and sends no answer",
    set: { answer: "none" },
    headline: "no_answer",
  },
  {
    id: "answer-refused",
    check: "reach",
    name: "The browser refuses the answer",
    set: { answer: "rejected" },
    headline: "answer_refused",
  },

  {
    id: "vpn-blocks-udp",
    check: "connection",
    name: "A VPN or VLAN firewall blocks UDP",
    set: { ice: "failed" },
    headline: "udp_blocked",
  },
  {
    id: "cloud-no-path",
    check: "connection",
    name: "Cloud: UDP and the relay are blocked",
    set: { access: "cloud", ice: "failed" },
    headline: "udp_blocked",
  },
  {
    id: "no-dtls",
    check: "connection",
    name: "ICE connects, the secure channel does not",
    set: { dataChannel: "closed" },
    headline: "no_secure_channel",
  },

  {
    id: "restart-power",
    check: "health",
    name: "The JetKVM lost power a moment ago",
    set: { restart: "power" },
    headline: null,
    also: ["restart_power"],
  },
  {
    id: "restart-brownout",
    check: "health",
    name: "The JetKVM restarted from a voltage drop",
    set: { restart: "brownout" },
    headline: null,
    also: ["restart_brownout"],
  },
  {
    id: "restart-fault",
    check: "health",
    name: "The JetKVM restarted after a fault",
    set: { restart: "fault" },
    headline: null,
    also: ["restart_fault"],
  },
  {
    id: "clock-not-set",
    check: "health",
    name: "The JetKVM's clock is not set",
    set: { clock: "not_set" },
    headline: null,
    also: ["clock_not_set"],
  },

  {
    id: "paused-update",
    check: "picture",
    name: "Video paused for an update",
    set: { videoPause: "update" },
    headline: "video_paused",
  },
  {
    id: "paused-upload",
    check: "picture",
    name: "Video paused for an upload",
    set: { videoPause: "upload" },
    headline: "video_paused",
  },
  {
    id: "computer-off",
    check: "picture",
    name: "The computer is off",
    set: { hdmi: "no_cable", usb: "not_attached" },
    headline: "computer_off",
    also: ["usb_follows_computer"],
  },
  {
    id: "computer-off-5v",
    check: "picture",
    name: "The computer is off, HDMI still has 5 V",
    set: { hdmi: "no_signal", usb: "not_attached" },
    headline: "computer_off",
  },
  {
    id: "no-hdmi-cable",
    check: "picture",
    name: "The HDMI cable is not connected",
    set: { hdmi: "no_cable" },
    headline: "no_hdmi_cable",
  },
  {
    id: "computer-asleep",
    check: "picture",
    name: "The computer is asleep",
    set: { hdmi: "no_signal", usb: "suspended" },
    headline: "computer_asleep",
    also: ["usb_follows_computer"],
  },
  {
    id: "display-off",
    check: "picture",
    name: "The computer is on, the display output is off",
    set: { hdmi: "no_signal" },
    headline: "no_picture",
  },
  {
    id: "edid-rejected",
    check: "picture",
    name: "No picture with a custom EDID",
    set: { hdmi: "no_signal", edid: "custom" },
    headline: "no_picture_edid",
  },
  {
    id: "mode-4k",
    check: "picture",
    name: "The computer sends 4K (kvm #1482)",
    set: { hdmi: "unsupported_mode" },
    headline: "unsupported_mode",
  },
  {
    id: "mode-4k-edid",
    check: "picture",
    name: "The computer sends 4K with a custom EDID",
    set: { hdmi: "unsupported_mode", edid: "custom" },
    headline: "unsupported_mode",
  },
  {
    id: "mini-no-video",
    check: "picture",
    name: "The JetKVM has a picture and sends none",
    set: { video: "not_sent" },
    headline: "mini_sends_no_video",
  },
  {
    id: "no-decode",
    check: "picture",
    name: "Firefox with hardware H.264 off (kvm #655)",
    set: { browser: "firefox", video: "not_decoded" },
    headline: "browser_decodes_no_video",
  },
  {
    id: "autoplay",
    check: "picture",
    name: "The browser blocks autoplay",
    set: { playback: "blocked" },
    headline: "autoplay_blocked",
  },

  {
    id: "usb-wrong-port",
    check: "input",
    name: "USB in the wrong port, or a power-only cable",
    set: { usb: "not_attached" },
    headline: "usb_not_attached",
  },
  {
    id: "usb-bios-rejects",
    check: "input",
    name: "A BIOS or KVM switch refuses the USB device",
    set: { usb: "attached" },
    headline: "usb_not_configured",
  },
  {
    id: "usb-low-power",
    check: "input",
    name: "USB suspended while the picture shows",
    set: { usb: "suspended" },
    headline: null,
    also: ["usb_suspended"],
  },
  {
    id: "keyboard-dead",
    check: "input",
    name: "USB configured, the keyboard gets no reply",
    set: { keyboardTest: "no_reply" },
    headline: "keyboard_no_reply",
  },

  {
    id: "audio-off",
    check: "audio",
    name: "Audio is off",
    set: { audioSetting: "off" },
    headline: null,
    also: ["audio_off"],
  },
  {
    id: "usb-audio-off",
    check: "audio",
    name: "Audio on, USB Audio off",
    set: { usbAudio: "off" },
    headline: "usb_audio_off",
  },
  {
    id: "audio-not-ready",
    check: "audio",
    name: "The computer did not set up the audio device",
    set: { audioSource: "not_ready" },
    headline: "audio_device_not_ready",
  },
  {
    id: "audio-other-output",
    check: "audio",
    name: "The computer plays to another output",
    set: { audioSource: "idle" },
    headline: null,
    also: ["audio_no_stream"],
  },
  {
    id: "audio-muted",
    check: "audio",
    name: "The computer is muted",
    set: { audioLevel: "silent" },
    headline: null,
    also: ["audio_silent"],
  },
  {
    id: "audio-blocked",
    check: "audio",
    name: "The browser blocks audio",
    set: { audioPlayback: "blocked" },
    headline: "audio_blocked",
  },

  {
    id: "relayed",
    check: "quality",
    name: "Cloud through a relay with high delay",
    set: { access: "cloud", route: "relay", delay: "high" },
    headline: null,
    also: ["relayed", "high_delay"],
  },
  {
    id: "wifi-weak",
    check: "quality",
    name: "Weak Wi-Fi at the JetKVM with packet loss",
    set: { link: "wifi_weak", loss: "high" },
    headline: null,
    also: ["wifi_weak", "packet_loss"],
  },

  // Two faults at once: the earlier check wins the headline.
  {
    id: "vpn-and-asleep",
    check: "connection",
    name: "A VPN blocks UDP and the computer is asleep",
    set: { ice: "failed", hdmi: "no_signal", usb: "suspended" },
    headline: "udp_blocked",
  },
  {
    id: "no-picture-and-no-usb-audio",
    check: "picture",
    name: "Unsupported mode and USB Audio off",
    set: { hdmi: "unsupported_mode", usbAudio: "off" },
    headline: "unsupported_mode",
    also: ["usb_audio_off"],
  },
];

export function applyChange(base: DiagnosisInput, change: InputChange): DiagnosisInput {
  return { ...base, ...change };
}

// The signals that can change the diagnosis from this state: a signal is live
// if one of its other values gives another result. The rest are hidden behind
// an earlier stop, and the lab greys them out.
export function liveSignals(input: DiagnosisInput): Set<SignalKey> {
  const now = JSON.stringify(diagnose(input));
  const live = new Set<SignalKey>();
  for (const key of SIGNAL_KEYS) {
    for (const option of Object.keys(SIGNALS[key].options)) {
      if (option === input[key]) continue;
      const changed = { ...input, [key]: option } as DiagnosisInput;
      if (JSON.stringify(diagnose(changed)) !== now) {
        live.add(key);
        break;
      }
    }
  }
  return live;
}
