import { useState } from "react";
import { LuChevronDown, LuChevronUp } from "react-icons/lu";

import { Button } from "@components/Button";
import LoadingSpinner from "@components/LoadingSpinner";
import { OverlayContent } from "@components/VideoOverlay";
import { cx } from "@/cva.config";
import {
  leadFinding,
  type Diagnosis,
  type Finding,
  type Level,
  type Place,
  type PlaceState,
} from "@/diagnosis/diagnosis";

// Draft of the connection check panel: a map of the three parts and the
// links between them, then what is wrong, what to do, and one button. The
// parts look like the status cards in the header. The text is not in the
// message catalog yet: it changes with each design round.

// The status dot of the header's status cards.
const DOT: Record<PlaceState | Level, string> = {
  ok: "border-green-600 bg-green-500",
  problem: "border-red-600 bg-red-500",
  wait: "border-blue-600 bg-blue-500",
  warning: "border-amber-500 bg-amber-400",
  info: "border-slate-400 bg-slate-300",
  unknown: "border-slate-300 bg-slate-200 dark:border-slate-600 dark:bg-slate-700",
};

const LINE: Record<PlaceState, string> = {
  ok: "border-green-500",
  problem: "border-red-500",
  wait: "border-blue-500",
  warning: "border-amber-400",
  unknown: "border-slate-200 dark:border-slate-700",
};

const LINK_LABEL: Record<PlaceState, string> = {
  ok: "text-slate-600 dark:text-slate-400",
  problem: "font-semibold text-red-600 dark:text-red-400",
  wait: "text-blue-700 dark:text-blue-300",
  warning: "text-amber-600 dark:text-amber-400",
  unknown: "text-slate-400 dark:text-slate-600",
};

// The word under a part's name. A finding can give a more exact one.
const NODE_STATUS: Record<"browser" | "mini" | "computer", Record<PlaceState, string>> = {
  browser: { ok: "Ready", problem: "Problem", wait: "Busy", warning: "Ready", unknown: "Unknown" },
  mini: {
    ok: "Connected",
    problem: "Problem",
    wait: "Busy",
    warning: "Connected",
    unknown: "Unknown",
  },
  computer: { ok: "On", problem: "Problem", wait: "Busy", warning: "On", unknown: "Unknown" },
};

function MapNode({
  place,
  title,
  diagnosis,
  wide,
}: {
  place: "browser" | "mini" | "computer";
  title: string;
  diagnosis: Diagnosis;
  // One row over the full width, for the vertical map.
  wide?: boolean;
}) {
  const state = diagnosis.map[place];
  const finding = [diagnosis.headline, ...diagnosis.others].find(
    candidate => candidate?.where === place && candidate.level === state,
  );
  return (
    <div
      className={cx(
        "shrink-0 rounded-md border bg-white px-2 py-1.5 dark:bg-slate-800",
        wide ? "flex w-full items-center justify-between gap-x-3" : "space-y-1",
        state === "problem" ? "border-red-500/70" : "border-slate-800/20 dark:border-slate-600",
      )}
    >
      <div
        className={cx(
          "text-xs leading-none font-semibold",
          state === "unknown" ? "text-slate-400 dark:text-slate-500" : "text-black dark:text-white",
        )}
      >
        {title}
      </div>
      <div
        className={cx(
          "flex items-center gap-x-1 text-xs leading-none",
          state === "unknown"
            ? "text-slate-400 dark:text-slate-500"
            : "text-slate-700 dark:text-slate-300",
        )}
      >
        <div className={cx("h-2 w-2 shrink-0 rounded-full border", DOT[state])} />
        {finding?.mapLabel ?? NODE_STATUS[place][state]}
      </div>
    </div>
  );
}

// The links are as high as the parts (40 px). One link runs through the
// middle; two links run at one third and two thirds, with the labels outside.
function OneLink({ state, label }: { state: PlaceState; label: string }) {
  return (
    <div className="relative h-10 min-w-10 grow">
      <div
        className={cx(
          "absolute inset-x-0 bottom-1/2 pb-1 text-center text-[10px] leading-none",
          LINK_LABEL[state],
        )}
      >
        {label}
      </div>
      <div className={cx("absolute inset-x-0 top-1/2 border-t", LINE[state])} />
    </div>
  );
}

function TwoLinks({
  top,
  bottom,
}: {
  top: { state: PlaceState; label: string };
  bottom: { state: PlaceState; label: string };
}) {
  return (
    <div className="relative h-10 min-w-10 grow">
      <div
        className={cx(
          "absolute inset-x-0 top-0 text-center text-[10px] leading-none",
          LINK_LABEL[top.state],
        )}
      >
        {top.label}
      </div>
      <div className={cx("absolute inset-x-0 top-[13px] border-t", LINE[top.state])} />
      <div className={cx("absolute inset-x-0 top-[26px] border-t", LINE[bottom.state])} />
      <div
        className={cx(
          "absolute inset-x-0 bottom-0 text-center text-[10px] leading-none",
          LINK_LABEL[bottom.state],
        )}
      >
        {bottom.label}
      </div>
    </div>
  );
}

// A link of the vertical map: a short line down, with the label beside it.
function DownLink({ state, label }: { state: PlaceState; label: string }) {
  return (
    <div className="flex h-5 items-center gap-x-1.5">
      <div className={cx("h-full border-l", LINE[state])} />
      <div className={cx("text-[10px] leading-none", LINK_LABEL[state])}>{label}</div>
    </div>
  );
}

function ConnectionMap({ diagnosis }: { diagnosis: Diagnosis }) {
  const { map } = diagnosis;
  return (
    <>
      {/* Left to right where the panel is wide enough, top to bottom on a phone. */}
      <div className="hidden w-full items-center @sm:flex">
        <MapNode place="browser" title="Browser" diagnosis={diagnosis} />
        <OneLink state={map.network} label="Network" />
        <MapNode place="mini" title="JetKVM" diagnosis={diagnosis} />
        <TwoLinks
          top={{ state: map.hdmi, label: "HDMI" }}
          bottom={{ state: map.usb, label: "USB" }}
        />
        <MapNode place="computer" title="Computer" diagnosis={diagnosis} />
      </div>
      <div className="flex w-full flex-col @sm:hidden">
        <MapNode place="browser" title="Browser" diagnosis={diagnosis} wide />
        <div className="pl-4">
          <DownLink state={map.network} label="Network" />
        </div>
        <MapNode place="mini" title="JetKVM" diagnosis={diagnosis} wide />
        <div className="flex gap-x-8 pl-4">
          <DownLink state={map.hdmi} label="HDMI" />
          <DownLink state={map.usb} label="USB" />
        </div>
        <MapNode place="computer" title="Computer" diagnosis={diagnosis} wide />
      </div>
    </>
  );
}

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
  // Only the names: a check that failed is already in the title or the list
  // above, and a check that did not run has no result to show.
  const passed = diagnosis.checks.filter(check => check.status === "pass");
  const notChecked = diagnosis.checks.filter(check => check.status === "skipped");
  const place: Record<Place, string> = {
    browser: "Browser",
    network: "Network",
    mini: "JetKVM",
    hdmi: "HDMI",
    usb: "USB",
    computer: "Computer",
  };

  return (
    <div className="space-y-3 border-t border-slate-800/20 pt-4 text-xs leading-5 dark:border-slate-300/20">
      {lead && <p className="text-slate-700 dark:text-slate-300">{lead.detail}</p>}

      {more.length > 0 && (
        <ul className="space-y-2">
          {more.map(finding => (
            <li key={finding.id} className="flex items-baseline gap-x-2">
              <div className={cx("h-2 w-2 shrink-0 rounded-full border", DOT[finding.level])} />
              <div className="min-w-0 space-y-1">
                <div>
                  <span className="text-slate-500 dark:text-slate-400">
                    {place[finding.where]}:
                  </span>{" "}
                  <span className="font-semibold text-black dark:text-white">{finding.title}.</span>{" "}
                  {finding.fix && (
                    <span className="text-slate-700 dark:text-slate-300">{finding.fix}</span>
                  )}
                </div>
                {finding.action && (
                  <Button
                    size="XS"
                    theme="light"
                    text={finding.action}
                    loading={busy === finding.id}
                    onClick={() => onAction(finding)}
                  />
                )}
              </div>
            </li>
          ))}
        </ul>
      )}

      {passed.length > 0 && (
        <p className="text-slate-500 dark:text-slate-400">
          Works: {passed.map(check => check.title).join(", ")}.
        </p>
      )}
      {notChecked.length > 0 && (
        <p className="text-slate-500 dark:text-slate-400">
          Not checked until this is fixed: {notChecked.map(check => check.title).join(", ")}.
        </p>
      )}
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

  return (
    <div className="h-full w-full">
      <OverlayContent>
        {/* The card's content shrinks to fit, so the panel sets its own width. */}
        <div className="@container w-[27rem] max-w-full space-y-5 text-left">
          <ConnectionMap diagnosis={diagnosis} />

          <div className="space-y-2">
            <h2 className="flex items-center gap-x-2 text-xl font-bold text-black dark:text-white">
              {lead?.level === "wait" && (
                <LoadingSpinner className="h-5 w-5 shrink-0 text-blue-800 dark:text-blue-200" />
              )}
              {lead ? lead.title : "Everything works"}
            </h2>
            {lead?.fix && <p className="text-sm text-slate-700 dark:text-slate-300">{lead.fix}</p>}
          </div>

          <div className="flex flex-wrap items-center gap-2">
            {lead?.action && (
              <Button
                size="SM"
                theme="primary"
                text={lead.action}
                loading={busy === lead.id}
                onClick={() => onAction(lead)}
              />
            )}
            <Button
              size="SM"
              theme="light"
              text="Details"
              TrailingIcon={showDetails ? LuChevronUp : LuChevronDown}
              onClick={() => setShowDetails(!showDetails)}
            />
          </div>

          {showDetails && (
            <Details diagnosis={diagnosis} lead={lead} busy={busy} onAction={onAction} />
          )}
        </div>
      </OverlayContent>
    </div>
  );
}
