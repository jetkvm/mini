import { expect, test, type Page } from "@playwright/test";
import { callJsonRpc } from "../helpers";
import { expectUpdateCheckRuns, tlsAdmission } from "./ota";
import { remoteHostExec } from "../remote-agent/shared";
import { remoteHostCan } from "./remote-host";

/** A public ISO the Mini mounts as its CD-ROM over HTTPS. */
export const HTTPS_IMAGE_URL = "https://boot.netboot.xyz/ipxe/netboot.xyz.iso";
// The host's block device of the CD-ROM: on macOS, the JetKVM CD media in the
// I/O Registry.
const FIND_CDROM =
  'if [ "$(uname -s)" = Darwin ]; then ioreg -r -c IOCDMedia -l -w0 | ' +
  `awk -F'"' '/^[+]-o/ { found = /^[+]-o JetKVM / } found && $2 == "BSD Name" { print "/dev/" $4; exit }'; ` +
  "else lsblk -Snpo NAME,TRAN,TYPE,VENDOR 2>/dev/null | " +
  'awk \'$2=="usb" && $3=="rom" && $4=="JetKVM" {print $1; exit}\'; fi';

// The host's reader runs in its own session and process group, with this
// process group ID. Killing only its shell would leave the running `sudo dd`
// reading the image, so the whole group is stopped: SIGTERM, which sudo
// passes on to dd, then up to 10 s for the group to exit, then SIGKILL as root.
let readerGroup = "";

// Starts the shell script in argv[1] in a new session and prints its PID,
// which is also its process group ID. Python does this on Linux and macOS;
// macOS has no setsid command.
const LAUNCH_IN_NEW_SESSION =
  "import subprocess, sys; " +
  'print(subprocess.Popen(["sh", "-c", sys.argv[1]], start_new_session=True, ' +
  "stdin=subprocess.DEVNULL, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL).pid)";

function quote(value: string): string {
  return `'${value.replace(/'/g, "'\\''")}'`;
}

/**
 * Mount the image as the Mini's CD-ROM and wait for the host to see it;
 * returns the host's block device. A mount re-enumerates the Mini's USB
 * device on the host.
 */
export async function mountHttpsImage(page: Page): Promise<string> {
  await callJsonRpc(page, "unmountImage").catch(() => undefined);
  await callJsonRpc(page, "mountWithHTTP", { url: HTTPS_IMAGE_URL, mode: "CDROM" }, 60_000);
  await expect
    .poll(() => remoteHostExec(FIND_CDROM).trim(), {
      timeout: 45_000,
      message: "the CD-ROM must appear on the host",
    })
    .not.toBe("");
  return remoteHostExec(FIND_CDROM).trim();
}

/** Skips the test when the remote host cannot run startHostReads: its
 * reader is a python3 launcher and a `sudo -n dd` with GNU dd's
 * iflag=direct, which BSD dd (macOS) and uutils dd do not have. */
export function skipWithoutHostReads(): void {
  test.skip(
    !remoteHostCan(
      "command -v python3 >/dev/null && sudo -n true 2>/dev/null && " +
        '"$(command -v gnudd || echo dd)" --version 2>/dev/null | head -n 1 | ' +
        "grep -v uutils | grep -q coreutils",
    ),
    `${process.env.JETKVM_REMOTE_HOST} needs python3, passwordless sudo and GNU dd for its reads ` +
      "of the CD-ROM (https-image-read.ts)",
  );
}

/**
 * Start the host reading the CD-ROM in the background, until `passes` whole
 * reads have succeeded or `seconds` have passed. The reads bypass the host's
 * page cache, so each pass fetches the image from the web server again over
 * the Mini's HTTPS client. Each pass looks for the CD-ROM again, and one that
 * fails (the Mini left the bus, or nothing is mounted) is retried a second
 * later, so the reads resume on their own once the image is back. uutils dd
 * fails iflag=direct with EINVAL, so GNU dd (gnudd where installed) is used.
 */
export function startHostReads({ passes = 60, seconds = 600 } = {}): void {
  const reader =
    "dd=$(command -v gnudd || echo dd); " +
    `end=$(($(date +%s) + ${seconds})); i=0; ` +
    `while [ "$i" -lt ${passes} ] && [ "$(date +%s)" -lt "$end" ]; do ` +
    `d=$(${FIND_CDROM}); ` +
    'if [ -n "$d" ] && sudo -n "$dd" if="$d" of=/dev/null bs=64k iflag=direct 2>/dev/null; ' +
    "then i=$((i + 1)); else sleep 1; fi; done";
  const group = remoteHostExec(
    `python3 -c ${quote(LAUNCH_IN_NEW_SESSION)} ${quote(reader)}`,
  ).trim();
  if (!/^\d+$/.test(group)) {
    throw new Error(`the host's CD-ROM reader did not start: ${group}`);
  }
  readerGroup = group;
}

/**
 * Stop the host's reads and unmount the image. Call it from afterEach, which
 * also runs after a test timeout, when a finally block is skipped.
 */
export async function stopHttpsImageRead(page: Page): Promise<void> {
  if (readerGroup) {
    const group = readerGroup;
    readerGroup = "";
    remoteHostExec(
      `kill -TERM -- -${group} 2>/dev/null; ` +
        `for i in $(seq 1 50); do pgrep -g ${group} >/dev/null || exit 0; sleep 0.2; done; ` +
        `sudo -n kill -KILL -- -${group} 2>/dev/null; true`,
    );
  }
  await callJsonRpc(page, "unmountImage", {}, 30_000).catch(() => undefined);
}

/**
 * Mount an HTTPS image as the Mini's CD-ROM, have the host read it, and run
 * five update checks while the reads go on. The checks must be admitted, and
 * the reads must keep their TLS admission.
 */
export async function expectUpdateChecksDuringHttpsImageRead(page: Page): Promise<void> {
  await mountHttpsImage(page);
  startHostReads({ passes: 60 });
  // Image reads on the open connection take "https-media-range" tickets;
  // one that must connect again takes "https-media-connect".
  const media = await tlsAdmission("https-media-range");
  const connect = await tlsAdmission("https-media-connect");
  await expect
    .poll(async () => (await tlsAdmission("https-media-range")).admitted, {
      timeout: 30_000,
      message: "the host's reads must reach the image over HTTPS",
    })
    .toBeGreaterThan(media.admitted);
  // A read holds its TLS ticket only while a request is in flight, so one
  // check can fall between two requests; five checks overlap a read. The
  // 16 KiB margin of a check without a read is not required here.
  for (let check = 0; check < 5; check++) {
    await expectUpdateCheckRuns(page);
  }
  expect(
    (await tlsAdmission("https-media-range")).denied,
    "image reads keep their TLS admission during the checks",
  ).toBe(media.denied);
  expect(
    (await tlsAdmission("https-media-connect")).denied,
    "image reconnects keep their TLS admission during the checks",
  ).toBe(connect.denied);
}
