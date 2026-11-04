import { appendFileSync } from "node:fs";

// Playwright reporter: list every skipped test with the reason from its skip
// annotation, so a run shows what did not run and why. With E2E_SKIP_LOG set
// (tools/e2e/run.sh) the lines go to that file, and run.sh prints the
// lines of all its Playwright runs together at the end.
export default class SkipSummary {
  skipped = [];

  onTestEnd(test, result) {
    if (result.status !== "skipped") return;
    const annotations = [...test.annotations, ...(result.annotations ?? [])];
    const reasons = [
      ...new Set(annotations.filter(a => a.type === "skip" && a.description).map(a => a.description)),
    ];
    const project = test.parent.project()?.name ?? "";
    // Playwright skips the rest of a serial group after a failure without an
    // annotation; a test.skip() without a description also lands here.
    const reason = reasons.join("; ") || "no skip reason (an earlier failure in its serial group?)";
    this.skipped.push(`[${project}] ${test.title}: ${reason}`);
  }

  onEnd() {
    if (this.skipped.length === 0) return;
    const log = process.env.E2E_SKIP_LOG;
    if (log) {
      appendFileSync(log, this.skipped.map(line => `${line}\n`).join(""));
      return;
    }
    console.log(`\nSkipped (${this.skipped.length}):`);
    for (const line of this.skipped) console.log(`  ${line}`);
  }

  printsToStdio() {
    return false;
  }
}
