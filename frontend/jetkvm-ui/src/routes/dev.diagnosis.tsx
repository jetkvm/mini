// Gallery of connection cases, served by the Vite dev server only
// (http://localhost:5173/dev/diagnosis). It needs no JetKVM. Each card is one
// case: what the user sees today beside the proposed screen.
import { useEffect, useMemo, useRef, useState } from "react";

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
  SCENARIOS,
  SIGNALS,
  SIGNAL_KEYS,
  SYMPTOMS,
  applyChange,
  diagnose,
  healthyInput,
  todayScreen,
  type DiagnosisInput,
  type Finding,
  type Scenario,
  type SignalKey,
  type TodayScreenId,
} from "@/diagnosis/diagnosis";

const HEALTHY = healthyInput();

const SIZES = [
  { name: "Desktop", width: 600, height: 340 },
  { name: "Phone", width: 340, height: 480 },
];
type Size = (typeof SIZES)[number];

const optionLabel = (key: SignalKey, value: string) =>
  (SIGNALS[key].options as Record<string, string>)[value];

// The signals that differ from the healthy state, in words.
function signalsText(input: DiagnosisInput) {
  return SIGNAL_KEYS.filter(key => input[key] !== HEALTHY[key])
    .map(key => `${SIGNALS[key].label}: ${optionLabel(key, input[key])}`)
    .join(" · ");
}

function FakePicture({ children }: { children?: React.ReactNode }) {
  return (
    <div className="relative flex h-full w-full items-center justify-center rounded-md bg-linear-to-br from-slate-700 to-slate-900 text-sm text-slate-300">
      The computer's picture
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
              <h2 className="text-xl font-bold text-black dark:text-white">
                {m.other_session_detected()}
              </h2>
              <p className="text-sm text-slate-700 dark:text-slate-300">
                {m.other_session_take_over()}
              </p>
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
              input.audioLevel === "signal" &&
              input.audioPlayback === "blocked"
            }
            onEnableAudio={() => undefined}
          />
        </FakePicture>
      );
  }
}

// The video area of the app, at a fixed size.
function Screen({ size, children }: { size: Size; children: React.ReactNode }) {
  return (
    <div
      style={{ width: size.width, height: size.height }}
      className="max-w-full rounded-md border border-slate-300 bg-blue-50/40 p-2 dark:border-slate-700 dark:bg-slate-800/40"
    >
      <div className="relative h-full w-full">{children}</div>
    </div>
  );
}

function CaseCard({ scenario, size, both }: { scenario: Scenario; size: Size; both: boolean }) {
  const start = useMemo(() => applyChange(HEALTHY, scenario.set), [scenario]);
  // A button in the new screen changes the state of this card only.
  const [input, setInput] = useState(start);
  const [busy, setBusy] = useState<Finding["id"] | null>(null);
  const timer = useRef<ReturnType<typeof setTimeout> | null>(null);
  useEffect(() => () => void (timer.current && clearTimeout(timer.current)), []);

  const diagnosis = useMemo(() => diagnose(input), [input]);
  const today = useMemo(() => todayScreen(start), [start]);
  const refs = useMemo(() => diagnose(start).headline?.refs, [start]);

  // The fix is simulated: after a moment the signals it changes get their
  // new values, as if the action worked.
  const onAction = (finding: Finding) => {
    if (!finding.resolves || timer.current) return;
    setBusy(finding.id);
    timer.current = setTimeout(() => {
      timer.current = null;
      setBusy(null);
      setInput(current => applyChange(current, finding.resolves ?? {}));
    }, 1000);
  };

  return (
    <div id={scenario.id} className="space-y-2">
      <div>
        <h3 className="text-sm font-bold text-slate-900 dark:text-white">{scenario.name}</h3>
        <p className="text-xs text-slate-500 dark:text-slate-400">
          {signalsText(start) || "All signals healthy"}
        </p>
      </div>
      <div className="flex flex-wrap items-start gap-4">
        {both && (
          <div className="max-w-full space-y-1">
            <div className="text-[11px] font-semibold tracking-wide text-slate-500 uppercase dark:text-slate-400">
              Today
            </div>
            <Screen size={size}>
              <TodayView input={start} screen={today.id} />
            </Screen>
            <p
              className="text-xs text-slate-500 dark:text-slate-400"
              style={{ maxWidth: size.width }}
            >
              {today.note}
            </p>
          </div>
        )}
        <div className="max-w-full space-y-1">
          <div className="flex items-center gap-x-2 text-[11px] font-semibold tracking-wide text-slate-500 uppercase dark:text-slate-400">
            New
            {input !== start && (
              <button
                type="button"
                className="cursor-pointer text-blue-700 normal-case hover:underline dark:text-blue-400"
                onClick={() => setInput(start)}
              >
                Back to the start
              </button>
            )}
          </div>
          <Screen size={size}>
            <DiagnosisPanel diagnosis={diagnosis} busy={busy} onAction={onAction} />
          </Screen>
          {refs && (
            <p
              className="text-xs text-slate-500 dark:text-slate-400"
              style={{ maxWidth: size.width }}
            >
              Reported in {refs}.
            </p>
          )}
        </div>
      </div>
    </div>
  );
}

function Choice<T extends { name: string }>({
  options,
  value,
  onChange,
}: {
  options: T[];
  value: T;
  onChange: (value: T) => void;
}) {
  return (
    <div className="flex">
      {options.map(option => (
        <button
          key={option.name}
          type="button"
          onClick={() => onChange(option)}
          className={cx(
            "cursor-pointer border px-2.5 py-1 text-xs first:rounded-l-sm last:rounded-r-sm",
            option === value
              ? "border-slate-800 bg-slate-800 text-white dark:border-slate-200 dark:bg-slate-200 dark:text-slate-900"
              : "border-slate-300 bg-white text-slate-700 hover:border-slate-500 dark:border-slate-600 dark:bg-slate-800 dark:text-slate-300",
          )}
        >
          {option.name}
        </button>
      ))}
    </div>
  );
}

const VIEWS = [{ name: "Today and new" }, { name: "New only" }];

export default function DiagnosisGalleryRoute() {
  const [size, setSize] = useState(SIZES[0]);
  const [view, setView] = useState(VIEWS[0]);

  return (
    <div className="h-screen overflow-y-auto scroll-smooth bg-slate-50 text-slate-900 dark:bg-slate-900 dark:text-white">
      <title>Connection cases</title>
      <div className="sticky top-0 z-20 flex flex-wrap items-center gap-x-6 gap-y-2 border-b border-slate-200 bg-white px-6 py-3 dark:border-slate-700 dark:bg-slate-800">
        <h1 className="text-base font-bold">Connection cases</h1>
        <nav className="flex flex-wrap gap-x-4 gap-y-1 text-sm">
          {SYMPTOMS.map(symptom => (
            <a
              key={symptom.id}
              href={`#${symptom.id}`}
              className="text-blue-700 hover:underline dark:text-blue-400"
            >
              {symptom.title}
            </a>
          ))}
        </nav>
        <div className="ml-auto flex flex-wrap gap-3">
          <Choice options={VIEWS} value={view} onChange={setView} />
          <Choice options={SIZES} value={size} onChange={setSize} />
        </div>
      </div>

      <div className="space-y-12 p-6">
        {SYMPTOMS.map(symptom => (
          <section key={symptom.id} id={symptom.id} className="scroll-mt-16 space-y-6">
            <h2 className="border-b border-slate-200 pb-1 text-lg font-bold dark:border-slate-700">
              {symptom.title}
            </h2>
            <div className={cx("grid gap-x-8 gap-y-10", view === VIEWS[1] && "xl:grid-cols-2")}>
              {SCENARIOS.filter(scenario => scenario.symptom === symptom.id).map(scenario => (
                <CaseCard
                  // A new size starts each card again, so its screens fit.
                  key={`${scenario.id}-${size.name}`}
                  scenario={scenario}
                  size={size}
                  both={view === VIEWS[0]}
                />
              ))}
            </div>
          </section>
        ))}
      </div>
    </div>
  );
}
