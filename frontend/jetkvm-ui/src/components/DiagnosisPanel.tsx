import {
  CheckCircleIcon,
  ExclamationTriangleIcon,
  InformationCircleIcon,
  MinusCircleIcon,
} from "@heroicons/react/24/solid";
import { ArrowPathIcon } from "@heroicons/react/16/solid";

import { Button } from "@components/Button";
import LoadingSpinner from "@components/LoadingSpinner";
import { OverlayContent } from "@components/VideoOverlay";
import { cx } from "@/cva.config";
import type { CheckResult, CheckStatus, Diagnosis, Finding } from "@/diagnosis/diagnosis";

// Draft of the connection check panel. The text is not in the message
// catalog yet: it changes with each design round.

function StatusIcon({ status, className }: { status: CheckStatus; className?: string }) {
  switch (status) {
    case "pass":
      return <CheckCircleIcon className={cx("text-green-600 dark:text-green-500", className)} />;
    case "problem":
      return <ExclamationTriangleIcon className={cx("text-yellow-500", className)} />;
    case "warning":
      return <ExclamationTriangleIcon className={cx("text-slate-400", className)} />;
    case "wait":
      return <LoadingSpinner className={cx("text-blue-800 dark:text-blue-200", className)} />;
    case "info":
      return (
        <InformationCircleIcon className={cx("text-blue-600 dark:text-blue-400", className)} />
      );
    case "skipped":
      return <MinusCircleIcon className={cx("text-slate-300 dark:text-slate-600", className)} />;
  }
}

// A row of the check list. While the checks run, a row is `running` or
// `pending` and its result stays hidden.
function CheckRow({ check, run }: { check: CheckResult; run: "done" | "running" | "pending" }) {
  const dim = run === "pending" || (run === "done" && check.status === "skipped");
  return (
    <li className="flex items-start gap-x-2 py-1">
      {run === "done" ? (
        <StatusIcon status={check.status} className="mt-0.5 h-4 w-4 shrink-0" />
      ) : run === "running" ? (
        <LoadingSpinner className="mt-0.5 h-4 w-4 shrink-0 text-blue-800 dark:text-blue-200" />
      ) : (
        <span className="mt-0.5 h-4 w-4 shrink-0 rounded-full border-2 border-slate-200 dark:border-slate-700" />
      )}
      <div className="min-w-0 text-xs leading-5">
        <span
          className={cx(
            "font-semibold",
            dim ? "text-slate-400 dark:text-slate-500" : "text-black dark:text-white",
          )}
        >
          {check.title}
        </span>{" "}
        {run === "done" && (
          <span className="text-slate-600 dark:text-slate-400">{check.summary}</span>
        )}
      </div>
    </li>
  );
}

interface DiagnosisPanelProps {
  readonly diagnosis: Diagnosis;
  // The finding whose action runs or whose fix is being watched.
  readonly busy: Finding["id"] | null;
  // While the checks run: the index of the check that runs now. Null when done.
  readonly progress: number | null;
  readonly onAction: (finding: Finding) => void;
  readonly onCheckAgain: () => void;
}

export function DiagnosisPanel({
  diagnosis,
  busy,
  progress,
  onAction,
  onCheckAgain,
}: DiagnosisPanelProps) {
  const { headline, others, checks } = diagnosis;
  const headlineStatus: CheckStatus = headline?.level ?? "pass";
  const running = progress !== null;

  const checkList = (
    <ul className="self-start rounded-md border border-slate-800/10 bg-white/60 px-3 py-2 text-left dark:border-slate-300/10 dark:bg-slate-900/40">
      {checks.map((check, index) => (
        <CheckRow
          key={check.id}
          check={check}
          run={!running || index < progress ? "done" : index === progress ? "running" : "pending"}
        />
      ))}
    </ul>
  );

  return (
    <div className="h-full w-full">
      <OverlayContent>
        {/* The card's content shrinks to fit, so the panel sets its own width. */}
        <div className="@container w-[46rem] max-w-full">
          <div className="grid gap-x-8 gap-y-4 @xl:grid-cols-[minmax(0,1fr)_16rem]">
            {running ? (
              <div className="space-y-3 text-left">
                <LoadingSpinner className="h-8 w-8 text-blue-800 dark:text-blue-200" />
                <h2 className="text-xl font-bold text-black dark:text-white">
                  Checking the connection…
                </h2>
              </div>
            ) : (
              <div className="space-y-3 text-left">
                <StatusIcon status={headlineStatus} className="h-8 w-8" />
                <div className="space-y-1.5">
                  <h2 className="text-xl font-bold text-black dark:text-white">
                    {headline ? headline.title : "The connection works."}
                  </h2>
                  {headline && (
                    <p className="text-sm text-slate-600 dark:text-slate-400">{headline.detail}</p>
                  )}
                  {headline?.fix && (
                    <p className="text-sm text-black dark:text-white">{headline.fix}</p>
                  )}
                </div>

                <div className="flex flex-wrap items-center gap-2">
                  {headline?.action && (
                    <Button
                      size="SM"
                      theme="primary"
                      text={headline.action}
                      loading={busy === headline.id}
                      onClick={() => onAction(headline)}
                    />
                  )}
                  <Button
                    size="SM"
                    theme="light"
                    text="Check again"
                    LeadingIcon={ArrowPathIcon}
                    onClick={onCheckAgain}
                  />
                </div>

                {headline?.watch && (
                  <p className="flex items-center gap-x-2 text-xs text-slate-600 dark:text-slate-400">
                    <LoadingSpinner className="h-3.5 w-3.5 text-blue-800 dark:text-blue-200" />
                    Waiting for {headline.watch}. This updates by itself.
                  </p>
                )}

                {others.length > 0 && (
                  <ul className="space-y-2 border-t border-slate-800/10 pt-3 dark:border-slate-300/10">
                    {others.map(finding => (
                      <li key={finding.id} className="flex items-start gap-x-2">
                        <StatusIcon status={finding.level} className="mt-0.5 h-4 w-4 shrink-0" />
                        <div className="min-w-0 space-y-1 text-xs leading-5">
                          <div className="font-semibold text-black dark:text-white">
                            {finding.title}
                          </div>
                          {finding.fix && (
                            <div className="text-slate-600 dark:text-slate-400">{finding.fix}</div>
                          )}
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
              </div>
            )}

            {checkList}
          </div>
        </div>
      </OverlayContent>
    </div>
  );
}
