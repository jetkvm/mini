import { useState } from "react";
import {
  LuAppWindow,
  LuCheck,
  LuChevronDown,
  LuCpu,
  LuMinus,
  LuMonitor,
  LuX,
} from "react-icons/lu";

import { Button } from "@components/Button";
import LoadingSpinner from "@components/LoadingSpinner";
import { OverlayContent } from "@components/VideoOverlay";
import { cx } from "@/cva.config";
import {
  leadFinding,
  type CheckStatus,
  type Diagnosis,
  type Finding,
  type PlaceState,
} from "@/diagnosis/diagnosis";

// Draft of the connection check panel: a map of the three things and the
// links between them, then what is wrong, what to do, and one button. The
// text is not in the message catalog yet: it changes with each design round.

const NODE_CLASS: Record<PlaceState, string> = {
  ok: "border-slate-300 bg-white text-slate-700 dark:border-slate-600 dark:bg-slate-800 dark:text-slate-200",
  problem: "border-red-500 bg-red-50 text-red-600 dark:bg-red-950 dark:text-red-400",
  wait: "animate-pulse border-blue-500 bg-blue-50 text-blue-700 dark:bg-blue-950 dark:text-blue-300",
  warning: "border-amber-400 bg-amber-50 text-amber-600 dark:bg-amber-950 dark:text-amber-400",
  unknown:
    "border-dashed border-slate-300 bg-transparent text-slate-300 dark:border-slate-600 dark:text-slate-600",
};

const LINE_CLASS: Record<PlaceState, string> = {
  ok: "border-green-500",
  problem: "border-dashed border-red-400",
  wait: "border-blue-400",
  warning: "border-amber-400",
  unknown: "border-dashed border-slate-300 dark:border-slate-600",
};

const DOT_CLASS: Record<PlaceState, string> = {
  ok: "bg-green-500 text-white",
  problem: "bg-red-500 text-white",
  wait: "bg-blue-500 text-white",
  warning: "bg-amber-400 text-white",
  unknown: "bg-slate-200 text-slate-400 dark:bg-slate-700 dark:text-slate-500",
};

function MapNode({
  state,
  label,
  Icon,
}: {
  state: PlaceState;
  label: string;
  Icon: React.FC<{ className?: string }>;
}) {
  return (
    <div className="flex w-16 shrink-0 flex-col items-center gap-y-1">
      <div
        className={cx(
          "flex h-11 w-11 items-center justify-center rounded-full border-2",
          NODE_CLASS[state],
        )}
      >
        <Icon className="h-5 w-5" />
      </div>
      <div
        className={cx(
          "text-center text-[11px] leading-tight font-semibold",
          state === "unknown" ? "text-slate-400 dark:text-slate-500" : "text-black dark:text-white",
        )}
      >
        {label}
      </div>
    </div>
  );
}

// A link of the map: a line with a status dot in the middle. The label sits
// above or below the line, so the line keeps its length in a narrow panel.
function MapLink({
  state,
  label,
  labelSide,
}: {
  state: PlaceState;
  label: string;
  labelSide: "top" | "bottom";
}) {
  const DotIcon = state === "ok" ? LuCheck : state === "problem" ? LuX : LuMinus;
  return (
    <div
      className={cx(
        "flex w-full items-center gap-y-0.5",
        labelSide === "top" ? "flex-col-reverse" : "flex-col",
      )}
    >
      <div className="flex w-full items-center">
        <div className={cx("h-0 grow border-t-2", LINE_CLASS[state])} />
        <span
          className={cx(
            "flex h-4 w-4 shrink-0 items-center justify-center rounded-full",
            DOT_CLASS[state],
          )}
        >
          <DotIcon className="h-3 w-3" />
        </span>
        <div className={cx("h-0 grow border-t-2", LINE_CLASS[state])} />
      </div>
      <div
        className={cx(
          "text-[10px] leading-3 font-semibold",
          state === "unknown"
            ? "text-slate-400 dark:text-slate-500"
            : "text-slate-700 dark:text-slate-300",
        )}
      >
        {label}
      </div>
    </div>
  );
}

function ConnectionMap({ map }: { map: Diagnosis["map"] }) {
  return (
    <div className="flex w-full items-start">
      <MapNode state={map.browser} label="This browser" Icon={LuAppWindow} />
      {/* One link: its line is at the middle of the circles (h-11), so the
          box starts 14 px down: (44 - 16) / 2. */}
      <div className="min-w-0 grow pt-3.5">
        <MapLink state={map.network} label="Network" labelSide="bottom" />
      </div>
      <MapNode state={map.mini} label="JetKVM" Icon={LuCpu} />
      {/* Two links, with their labels above and below. */}
      <div className="-mt-1.5 flex min-w-0 grow flex-col gap-y-1">
        <MapLink state={map.hdmi} label="HDMI" labelSide="top" />
        <MapLink state={map.usb} label="USB" labelSide="bottom" />
      </div>
      <MapNode state={map.computer} label="Computer" Icon={LuMonitor} />
    </div>
  );
}

const STATUS_DOT: Record<CheckStatus, string> = {
  pass: "bg-green-500",
  problem: "bg-red-500",
  wait: "bg-blue-500",
  warning: "bg-amber-400",
  info: "bg-blue-300",
  skipped: "bg-slate-200 dark:bg-slate-700",
};

function Details({
  diagnosis,
  lead,
  busy,
  onAction,
}: {
  diagnosis: Diagnosis;
  lead: Finding | null;
  busy: Finding["id"] | null;
  onAction: (finding: Finding) => void;
}) {
  const more = [diagnosis.headline, ...diagnosis.others].filter(
    (finding): finding is Finding => finding !== null && finding !== lead,
  );
  return (
    <div className="space-y-3 rounded-md border border-slate-800/10 bg-white/60 p-3 text-left text-xs leading-5 dark:border-slate-300/10 dark:bg-slate-900/40">
      {lead && <p className="text-slate-700 dark:text-slate-300">{lead.detail}</p>}

      {more.length > 0 && (
        <ul className="space-y-2">
          {more.map(finding => (
            <li key={finding.id} className="flex items-start gap-x-2">
              <span
                className={cx("mt-1.5 h-2 w-2 shrink-0 rounded-full", STATUS_DOT[finding.level])}
              />
              <div className="min-w-0 space-y-1">
                <span className="font-semibold text-black dark:text-white">{finding.title}.</span>{" "}
                {finding.fix && (
                  <span className="text-slate-600 dark:text-slate-400">{finding.fix}</span>
                )}
                {finding.action && (
                  <div>
                    <Button
                      size="XS"
                      theme="light"
                      text={finding.action}
                      loading={busy === finding.id}
                      onClick={() => onAction(finding)}
                    />
                  </div>
                )}
              </div>
            </li>
          ))}
        </ul>
      )}

      <ul className="grid grid-cols-1 gap-x-4 border-t border-slate-800/10 pt-2 @md:grid-cols-2 dark:border-slate-300/10">
        {diagnosis.checks.map(check => (
          <li key={check.id} className="flex items-start gap-x-2">
            <span
              className={cx("mt-1.5 h-2 w-2 shrink-0 rounded-full", STATUS_DOT[check.status])}
            />
            <span className="min-w-0">
              <span
                className={cx(
                  "font-semibold",
                  check.status === "skipped"
                    ? "text-slate-400 dark:text-slate-500"
                    : "text-black dark:text-white",
                )}
              >
                {check.title}:
              </span>{" "}
              <span className="text-slate-600 dark:text-slate-400">{check.summary}</span>
            </span>
          </li>
        ))}
      </ul>
    </div>
  );
}

interface DiagnosisPanelProps {
  readonly diagnosis: Diagnosis;
  // The finding whose action runs now.
  readonly busy: Finding["id"] | null;
  readonly onAction: (finding: Finding) => void;
}

export function DiagnosisPanel({ diagnosis, busy, onAction }: DiagnosisPanelProps) {
  const [showDetails, setShowDetails] = useState(false);
  const lead = leadFinding(diagnosis);
  const moreCount = [diagnosis.headline, ...diagnosis.others].filter(
    finding => finding && finding !== lead && finding.level !== "info",
  ).length;

  return (
    <div className="h-full w-full">
      <OverlayContent>
        {/* The card's content shrinks to fit, so the panel sets its own width. */}
        <div className="@container w-[30rem] max-w-full space-y-5">
          <ConnectionMap map={diagnosis.map} />

          <div className="space-y-1.5 text-center">
            <h2 className="flex items-center justify-center gap-x-2 text-xl font-bold text-black dark:text-white">
              {lead?.level === "wait" && (
                <LoadingSpinner className="h-5 w-5 shrink-0 text-blue-800 dark:text-blue-200" />
              )}
              {lead ? lead.title : "Everything works"}
            </h2>
            {lead?.fix && <p className="text-sm text-slate-700 dark:text-slate-300">{lead.fix}</p>}
          </div>

          <div className="flex flex-wrap items-center justify-center gap-x-3 gap-y-2">
            {lead?.action && (
              <Button
                size="MD"
                theme="primary"
                text={lead.action}
                loading={busy === lead.id}
                onClick={() => onAction(lead)}
              />
            )}
            <button
              type="button"
              onClick={() => setShowDetails(!showDetails)}
              className="flex cursor-pointer items-center gap-x-1 text-xs text-slate-600 hover:text-black dark:text-slate-400 dark:hover:text-white"
            >
              {moreCount > 0 ? `Details, and ${moreCount} more` : "Details"}
              <LuChevronDown className={cx("h-3.5 w-3.5", showDetails && "rotate-180")} />
            </button>
          </div>

          {showDetails && (
            <Details diagnosis={diagnosis} lead={lead} busy={busy} onAction={onAction} />
          )}
        </div>
      </OverlayContent>
    </div>
  );
}
