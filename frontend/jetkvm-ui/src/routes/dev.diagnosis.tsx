// Connection diagnosis lab, served by the Vite dev server only
// (http://localhost:5173/dev/diagnosis). It needs no JetKVM: every signal is
// a control on the page. For each state it shows the screen of today's UI
// beside the proposed check panel, at a size you can change.
import { useCallback, useEffect, useMemo, useRef, useState } from "react";
import { useSearchParams } from "react-router";

import { m } from "@localizations/messages.js";
import { DiagnosisPanel } from "@components/DiagnosisPanel";
import {
  AudioPermissionBanner,
  ConnectionFailedOverlay,
  HDMIErrorOverlay,
  LoadingConnectionOverlay,
  LoadingVideoOverlay,
  NoAutoplayPermissionsOverlay,
  OverlayContent,
  RebootingOverlay,
  VideoPausedOverlay,
} from "@components/VideoOverlay";
import { cx } from "@/cva.config";
import {
  CHECKS,
  SCENARIOS,
  SIGNALS,
  SIGNAL_KEYS,
  applyChange,
  diagnose,
  healthyInput,
  liveSignals,
  todayScreen,
  type CheckId,
  type DiagnosisInput,
  type Finding,
  type Have,
  type InputChange,
  type SignalKey,
  type Source,
  type TodayScreenId,
} from "@/diagnosis/diagnosis";

const HEALTHY = healthyInput();

// The state lives in the URL (?s=hdmi:no_signal,usb:suspended), so a case can
// be shared as a link.
function decodeInput(text: string | null): DiagnosisInput {
  const input: Record<string, string> = { ...HEALTHY };
  for (const pair of (text ?? "").split(",")) {
    const [key, value] = pair.split(":");
    if (key in SIGNALS && value in SIGNALS[key as SignalKey].options) input[key] = value;
  }
  return input as DiagnosisInput;
}

function changedKeys(input: DiagnosisInput): SignalKey[] {
  return SIGNAL_KEYS.filter(key => input[key] !== HEALTHY[key]);
}

function encodeInput(input: DiagnosisInput): string {
  return changedKeys(input)
    .map(key => `${key}:${input[key]}`)
    .join(",");
}

const optionLabel = (key: SignalKey, value: string) =>
  (SIGNALS[key].options as Record<string, string>)[value];

const SOURCE_LABEL: Record<Source, string> = {
  browser: "Browser",
  mini: "JetKVM",
  cloud: "Cloud",
  test: "Active test",
};

const HAVE_LABEL: Record<Have, string> = {
  ui: "in the UI",
  discarded: "discarded",
  firmware: "firmware only",
  new: "new",
};

const HAVE_HELP: Record<Have, string> = {
  ui: "The web UI reads this signal today.",
  discarded: "The web UI sees this today and only logs it to the console.",
  firmware: "The firmware has this; the web UI does not read it.",
  new: "Needs new code.",
};

const HAVE_CLASS: Record<Have, string> = {
  ui: "bg-green-100 text-green-800",
  discarded: "bg-amber-100 text-amber-800",
  firmware: "bg-amber-100 text-amber-800",
  new: "bg-red-100 text-red-800",
};

const TODAY_LABEL: Record<TodayScreenId, string> = {
  rebooting: "“Device is Rebooting”",
  other_session: "“Another Active Session Detected”",
  connecting: "Spinner: “Connecting to device…”",
  connection_failed: "“Connection Issue Detected”",
  video_paused: "“Video paused …”",
  no_hdmi: "“No HDMI signal detected.”",
  loading_video: "Spinner: “Loading video stream…”",
  autoplay: "“Autoplay permissions required”",
  video: "The picture, no message",
};

const SIZES = [
  { name: "Large", width: 900, height: 560 },
  { name: "Medium", width: 700, height: 420 },
  { name: "Short", width: 700, height: 300 },
  { name: "Phone", width: 380, height: 620 },
];

function SignalControl({
  signal,
  value,
  live,
  onChange,
}: {
  signal: SignalKey;
  value: string;
  live: boolean;
  onChange: (value: string) => void;
}) {
  const def = SIGNALS[signal];
  const changed = value !== HEALTHY[signal];
  return (
    <div
      className={cx("py-1.5", !live && "opacity-40")}
      title={live ? undefined : "In this state, this signal cannot change the result."}
    >
      <div className="flex flex-wrap items-center gap-x-1.5 gap-y-0.5">
        <span className={cx("text-xs font-semibold", changed ? "text-blue-700" : "text-slate-900")}>
          {def.label}
        </span>
        <span className="rounded-sm bg-slate-100 px-1 text-[10px] text-slate-600">
          {SOURCE_LABEL[def.source]}
        </span>
        <span
          className={cx("rounded-sm px-1 text-[10px]", HAVE_CLASS[def.have])}
          title={HAVE_HELP[def.have]}
        >
          {HAVE_LABEL[def.have]}
        </span>
      </div>
      <div className="mt-1 flex flex-wrap gap-1">
        {Object.entries(def.options).map(([option, label]) => (
          <button
            key={option}
            type="button"
            onClick={() => onChange(option)}
            className={cx(
              "cursor-pointer rounded-sm border px-1.5 py-0.5 text-xs",
              option === value
                ? option === HEALTHY[signal]
                  ? "border-slate-700 bg-slate-700 text-white"
                  : "border-blue-700 bg-blue-700 text-white"
                : "border-slate-300 bg-white text-slate-700 hover:border-slate-500",
            )}
          >
            {label}
          </button>
        ))}
      </div>
    </div>
  );
}

// The app's layout around the video area, drawn with placeholders: header,
// action bar, video area, status bar. The overlay goes in the video area, as
// in WebRTCVideo.
function AppFrame({
  input,
  size,
  children,
}: {
  input: DiagnosisInput;
  size: { width: number; height: number };
  children: React.ReactNode;
}) {
  return (
    <div
      // A new size preset replaces a size set by dragging the corner.
      key={`${size.width}x${size.height}`}
      style={{ width: size.width, height: size.height, resize: "both" }}
      className="flex max-w-full flex-col overflow-hidden rounded-md border border-slate-300 bg-white shadow-sm"
    >
      <div className="flex shrink-0 items-center justify-between border-b border-slate-200 px-3 py-1.5">
        <span className="text-sm font-bold text-blue-700">JetKVM</span>
        <span className="rounded-sm border border-slate-200 px-1.5 py-0.5 text-[10px] text-slate-600">
          USB: {optionLabel("usb", input.usb)}
        </span>
      </div>
      <div className="flex shrink-0 flex-wrap gap-1.5 border-b border-slate-200 px-3 py-1.5">
        {["Paste text", "Virtual Media", "Virtual Keyboard", "Settings"].map(name => (
          <span
            key={name}
            className="rounded-sm border border-slate-200 px-2 py-0.5 text-[10px] text-slate-400"
          >
            {name}
          </span>
        ))}
        <span className="text-[10px] leading-5 text-slate-400">(action bar)</span>
      </div>
      <div className="relative min-h-0 grow bg-blue-50/40">
        <div className="absolute inset-x-4 inset-y-2 flex items-center justify-center">
          <div className="relative h-full max-h-[720px] w-full max-w-7xl rounded-md">
            {children}
          </div>
        </div>
      </div>
      <div className="shrink-0 border-t border-slate-200 px-3 py-0.5 text-[10px] text-slate-400">
        (status bar)
      </div>
    </div>
  );
}

function FakePicture({ children }: { children?: React.ReactNode }) {
  return (
    <div className="relative flex h-full w-full items-center justify-center rounded-md bg-linear-to-br from-slate-700 to-slate-900 text-sm text-slate-300">
      The picture of the computer
      {children}
    </div>
  );
}

// Today's screen for the state, drawn with the product's own components.
function TodayView({ input, screen }: { input: DiagnosisInput; screen: TodayScreenId }) {
  switch (screen) {
    case "rebooting":
      return <RebootingOverlay show postRebootAction={null} />;
    case "connecting":
      return <LoadingConnectionOverlay show text={m.connecting_to_device()} />;
    case "connection_failed":
      return <ConnectionFailedOverlay show setupPeerConnection={async () => undefined} />;
    case "other_session":
      return (
        <div className="h-full w-full">
          <OverlayContent>
            <div className="max-w-sm space-y-2 text-left">
              <h2 className="text-xl font-bold text-black">{m.other_session_detected()}</h2>
              <p className="text-sm text-slate-700">{m.other_session_take_over()}</p>
              <p className="text-xs text-slate-500">(In the product this is a dialog.)</p>
            </div>
          </OverlayContent>
        </div>
      );
    case "video_paused": {
      const text = {
        update: [
          m.video_overlay_update_paused_title(),
          m.video_overlay_update_paused_description(),
        ],
        download: [
          m.video_overlay_download_paused_title(),
          m.video_overlay_download_paused_description(),
        ],
        upload: [
          m.video_overlay_upload_paused_title(),
          m.video_overlay_upload_paused_description(),
        ],
        none: ["", ""],
      }[input.videoPause];
      return <VideoPausedOverlay show title={text[0]} description={text[1]} />;
    }
    case "no_hdmi":
      return <HDMIErrorOverlay show hdmiState="no_signal" />;
    case "loading_video":
      return <LoadingVideoOverlay show />;
    case "autoplay":
      return <NoAutoplayPermissionsOverlay show onPlayClick={() => undefined} />;
    case "video":
      return (
        <FakePicture>
          <AudioPermissionBanner
            show={
              input.audioSetting === "on" &&
              input.usb === "configured" &&
              input.usbAudio === "on" &&
              input.audioSource === "streaming" &&
              input.audioPlayback === "blocked"
            }
            onEnableAudio={() => undefined}
          />
        </FakePicture>
      );
  }
}

const CHECK_TITLE = Object.fromEntries(CHECKS.map(check => [check.id, check.title])) as Record<
  CheckId,
  string
>;

const GROUPS: { id: CheckId | "context"; title: string }[] = [
  { id: "context", title: "Context" },
  ...CHECKS,
];

export default function DiagnosisLabRoute() {
  const [searchParams, setSearchParams] = useSearchParams();
  const encoded = searchParams.get("s");
  const input = useMemo(() => decodeInput(encoded), [encoded]);
  const inputRef = useRef(input);
  inputRef.current = input;

  const setInput = useCallback(
    (next: DiagnosisInput) => {
      const text = encodeInput(next);
      setSearchParams(text ? { s: text } : {}, { replace: true });
    },
    [setSearchParams],
  );

  const diagnosis = useMemo(() => diagnose(input), [input]);
  const today = useMemo(() => todayScreen(input), [input]);
  const live = useMemo(() => liveSignals(input), [input]);
  const scenario = SCENARIOS.find(
    candidate => encodeInput(applyChange(HEALTHY, candidate.set)) === encodeInput(input),
  );

  const [size, setSize] = useState(SIZES[1]);

  // An action in the panel, or the user's manual step, is simulated: after a
  // moment the signals that the fix changes get their new values.
  const [busy, setBusy] = useState<Finding["id"] | null>(null);
  const timer = useRef<ReturnType<typeof setTimeout> | null>(null);
  useEffect(() => () => void (timer.current && clearTimeout(timer.current)), []);
  const simulateFix = useCallback(
    (finding: Finding) => {
      if (!finding.resolves || timer.current) return;
      setBusy(finding.id);
      timer.current = setTimeout(() => {
        timer.current = null;
        setBusy(null);
        setInput(applyChange(inputRef.current, finding.resolves as InputChange));
      }, 1200);
    },
    [setInput],
  );

  // "Check again" replays the run: one check after the other.
  const [progress, setProgress] = useState<number | null>(null);
  useEffect(() => {
    if (progress === null) return;
    const id = setTimeout(
      () => setProgress(progress + 1 < CHECKS.length ? progress + 1 : null),
      250,
    );
    return () => clearTimeout(id);
  }, [progress]);

  const headline = diagnosis.headline;
  const panelPlace = !headline
    ? "No problem: the panel opens only from the menu."
    : today.id === "video"
      ? "The picture works, so the panel does not cover it. A badge on the action bar opens it."
      : "Shown in place of today's screen.";

  return (
    <div className="h-screen overflow-y-auto bg-slate-50 text-slate-900">
      <title>Connection diagnosis lab</title>
      <div className="border-b border-slate-200 bg-white px-6 py-4">
        <h1 className="text-lg font-bold">Connection diagnosis lab</h1>
        <p className="max-w-4xl text-sm text-slate-600">
          Set the signals on the left. The right side shows the screen of today's UI and the
          proposed check panel for that state. Signals that cannot change the result in the current
          state are grey. The state is in the URL, so a case can be shared as a link.
        </p>
      </div>

      <div className="grid gap-6 p-6 xl:grid-cols-[25rem_minmax(0,1fr)]">
        <div className="space-y-3">
          <div className="flex items-center gap-2">
            <select
              className="min-w-0 grow rounded-sm border-slate-300 py-1 text-sm"
              value={scenario?.id ?? ""}
              onChange={event => {
                const picked = SCENARIOS.find(candidate => candidate.id === event.target.value);
                if (picked) setInput(applyChange(HEALTHY, picked.set));
              }}
            >
              <option value="" disabled>
                Custom state
              </option>
              {[{ id: "healthy" as const, title: "Healthy" }, ...CHECKS].map(group => (
                <optgroup key={group.id} label={group.title}>
                  {SCENARIOS.filter(candidate => candidate.check === group.id).map(candidate => (
                    <option key={candidate.id} value={candidate.id}>
                      {candidate.name}
                    </option>
                  ))}
                </optgroup>
              ))}
            </select>
            <button
              type="button"
              className="cursor-pointer rounded-sm border border-slate-300 bg-white px-2 py-1 text-sm hover:border-slate-500"
              onClick={() => setInput(HEALTHY)}
            >
              Reset
            </button>
          </div>

          {GROUPS.map(group => {
            const result = diagnosis.checks.find(check => check.id === group.id);
            return (
              <div key={group.id} className="rounded-md border border-slate-200 bg-white px-3 py-2">
                <div className="flex items-baseline justify-between gap-x-2">
                  <h2 className="text-sm font-bold">{group.title}</h2>
                  {result && (
                    <span
                      className={cx("text-[11px] font-semibold uppercase", {
                        "text-green-700": result.status === "pass",
                        "text-amber-700": result.status === "problem",
                        "text-blue-700": result.status === "wait" || result.status === "info",
                        "text-slate-600": result.status === "warning",
                        "text-slate-400": result.status === "skipped",
                      })}
                    >
                      {result.status === "skipped" ? "not checked" : result.status}
                    </span>
                  )}
                </div>
                {SIGNAL_KEYS.filter(key => SIGNALS[key].check === group.id).map(key => (
                  <SignalControl
                    key={key}
                    signal={key}
                    value={input[key]}
                    live={live.has(key)}
                    onChange={value => setInput({ ...input, [key]: value } as DiagnosisInput)}
                  />
                ))}
              </div>
            );
          })}
        </div>

        <div className="min-w-0 space-y-6">
          <div className="flex flex-wrap items-center gap-2 text-sm">
            <span className="font-semibold">Frame size</span>
            {SIZES.map(preset => (
              <button
                key={preset.name}
                type="button"
                onClick={() => setSize(preset)}
                className={cx(
                  "cursor-pointer rounded-sm border px-2 py-0.5 text-xs",
                  preset === size
                    ? "border-slate-700 bg-slate-700 text-white"
                    : "border-slate-300 bg-white hover:border-slate-500",
                )}
              >
                {preset.name} {preset.width}×{preset.height}
              </button>
            ))}
            <span className="text-xs text-slate-500">Drag a frame's corner for any size.</span>
          </div>

          <div className="flex flex-wrap items-start gap-6">
            <div className="max-w-full space-y-2">
              <h2 className="text-sm font-bold">
                Today: <span className="font-normal">{TODAY_LABEL[today.id]}</span>
              </h2>
              <AppFrame input={input} size={size}>
                <TodayView input={input} screen={today.id} />
              </AppFrame>
              <p className="text-xs text-slate-600" style={{ maxWidth: size.width }}>
                {today.note}
              </p>
            </div>

            <div className="max-w-full space-y-2">
              <h2 className="text-sm font-bold">
                Proposed:{" "}
                <span className="font-normal">{headline ? headline.title : "No problem"}</span>
              </h2>
              <AppFrame input={input} size={size}>
                <DiagnosisPanel
                  diagnosis={diagnosis}
                  busy={busy}
                  progress={progress}
                  onAction={simulateFix}
                  onCheckAgain={() => setProgress(0)}
                />
              </AppFrame>
              <p className="text-xs text-slate-600" style={{ maxWidth: size.width }}>
                {panelPlace}
                {headline?.refs && ` Seen in: ${headline.refs}.`}
              </p>
              {headline?.resolves && !headline.action && (
                <button
                  type="button"
                  className="cursor-pointer rounded-sm border border-slate-300 bg-white px-2 py-1 text-xs hover:border-slate-500"
                  onClick={() => simulateFix(headline)}
                >
                  {busy === headline.id ? "…" : "Simulate: the user does the fix"}
                </button>
              )}
            </div>
          </div>

          <div className="space-y-2">
            <h2 className="text-sm font-bold">All cases</h2>
            <p className="text-xs text-slate-600">
              Each row is one state. Select a row to load it above.
            </p>
            <div className="overflow-x-auto rounded-md border border-slate-200 bg-white">
              <table className="w-full text-left text-xs">
                <thead className="border-b border-slate-200 text-slate-600">
                  <tr>
                    <th className="px-2 py-1.5 font-semibold">Check</th>
                    <th className="px-2 py-1.5 font-semibold">Case</th>
                    <th className="px-2 py-1.5 font-semibold">Signals changed</th>
                    <th className="px-2 py-1.5 font-semibold">Today</th>
                    <th className="px-2 py-1.5 font-semibold">Proposed headline</th>
                    <th className="px-2 py-1.5 font-semibold">Button</th>
                    <th className="px-2 py-1.5 font-semibold">Also shown</th>
                  </tr>
                </thead>
                <tbody>
                  {SCENARIOS.map(candidate => {
                    const state = applyChange(HEALTHY, candidate.set);
                    const result = diagnose(state);
                    return (
                      <tr
                        key={candidate.id}
                        onClick={() => setInput(state)}
                        className={cx(
                          "cursor-pointer border-b border-slate-100 align-top last:border-0 hover:bg-blue-50",
                          candidate === scenario && "bg-blue-50",
                        )}
                      >
                        <td className="px-2 py-1.5 whitespace-nowrap text-slate-500">
                          {candidate.check === "healthy" ? "" : CHECK_TITLE[candidate.check]}
                        </td>
                        <td className="px-2 py-1.5 font-semibold">{candidate.name}</td>
                        <td className="px-2 py-1.5 text-slate-600">
                          {changedKeys(state).map(key => (
                            <div key={key}>
                              {SIGNALS[key].label}: {optionLabel(key, state[key])}
                            </div>
                          ))}
                        </td>
                        <td className="px-2 py-1.5 text-slate-600">
                          {TODAY_LABEL[todayScreen(state).id]}
                        </td>
                        <td className="px-2 py-1.5">{result.headline?.title ?? "No problem"}</td>
                        <td className="px-2 py-1.5 whitespace-nowrap text-slate-600">
                          {result.headline?.action ?? ""}
                        </td>
                        <td className="px-2 py-1.5 text-slate-600">
                          {result.others.map(finding => (
                            <div key={finding.id}>{finding.title}</div>
                          ))}
                        </td>
                      </tr>
                    );
                  })}
                </tbody>
              </table>
            </div>
          </div>
        </div>
      </div>
    </div>
  );
}
