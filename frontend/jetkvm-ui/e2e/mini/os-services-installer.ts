import { readFileSync } from "node:fs";
import * as path from "node:path";
import {
  SENDER_UNIT,
  TargetHost,
  USER_SESSION,
  pollUntil,
  quote,
  toolsDir,
  type ApprovalList,
} from "./os-services";

// The installer of OS Services as a user runs it: the KVM's "Mount installer"
// mounts the installer disc in its firmware as a CD-ROM, whose one page
// links to the download for the target computer; the offline installer in
// its menu saves the ISO with the installers to the TF card first and mounts
// that. These helpers do the target computer's part over SSH: they put the
// computer back to one without OS Services, start the AppImage in the
// desktop session, answer the sender's dialogs, and put the computer back
// the way it was.

/** The offline installer disc's volume name. */
export const INSTALLER_VOLUME = "OS_SERVICES";

/** The name the Mini saves the offline ISO under on its TF card. */
export const INSTALLER_FILENAME = "jetkvm-os-services.iso";

/**
 * The installer disc in the Mini's firmware: its volume name, the filename
 * getVirtualMediaState gives it, and its only file
 * (components/jetkvm_virtual_media/builtin_installer_iso.py).
 */
export const BUILTIN_VOLUME = "JETKVM";
export const BUILTIN_INSTALLER_FILENAME = "Install OS Services.iso";
export const BUILTIN_INSTALLER_PAGE = "Install OS Services.html";

/** The address the built-in disc's page links to for the Mini's version. */
export function builtinInstallerLink(version: string): string {
  return `https://jetkvm.com/os-services/install?mini=${encodeURIComponent(version)}`;
}

/** The sender's dialogs on Linux: the USB access request and the KVM approval. */
export const USB_ACCESS_DIALOG = { title: "Allow USB access?", button: "Allow USB Access" };
export const APPROVAL_DIALOG = { title: "Allow this KVM?", button: "Allow" };

/** The flag "Allow USB Access" runs the sender's own program with, through pkexec. */
export const ACCESS_FLAG = "--install-linux-access";

// --- The target computer ---------------------------------------------------

/** Where the computer's OS Services installation waits while the test runs. */
const SAVED = '"$HOME/.cache/jetkvm-e2e-os-services-install"';

// Every path an AppImage installation of OS Services writes, the program the
// unit runs, and the KVMs attached now ("<sysfs name> <bus> <device>").
const PATHS =
  USER_SESSION +
  `dir=${SAVED}; ` +
  "cfg=$(systemctl --user show-environment | sed -n 's/^XDG_CONFIG_HOME=//p'); " +
  'cfg="${cfg:-$HOME/.config}"; ' +
  `unit="$cfg/systemd/user/${SENDER_UNIT}"; ` +
  `wants="$cfg/systemd/user/graphical-session.target.wants/${SENDER_UNIT}"; ` +
  `dropin="$cfg/systemd/user/${SENDER_UNIT}.d"; ` +
  'data="$cfg/ipkvm-os-services"; ' +
  "rule=/etc/udev/rules.d/70-ipkvm-os-services.rules; " +
  "helper=/usr/local/libexec/drmtap-helper; " +
  `prog=$(systemctl --user show -p ExecStart --value ${SENDER_UNIT} | ` +
  "sed -n 's/^{ path=\\(.*\\) ; argv\\[\\].*/\\1/p'); " +
  "kvms() { for i in /sys/bus/usb/devices/*:*; do " +
  '[ "$(cat "$i/bInterfaceClass" "$i/bInterfaceSubClass" "$i/bInterfaceProtocol" 2>/dev/null | tr -d \'\\n\')" = ff4a01 ] || continue; ' +
  'd=$(dirname "$(readlink -f "$i")"); echo "${d##*/} $(cat "$d/busnum") $(cat "$d/devnum")"; ' +
  "done | sort -u; }; " +
  'usbnode() { printf "/dev/bus/usb/%03d/%03d" "$1" "$2"; }; ';

// The udev rule directories in the order udev reads them.
const RULE_COPIES = [
  "/etc/udev/rules.d",
  "/run/udev/rules.d",
  "/usr/local/lib/udev/rules.d",
  "/usr/lib/udev/rules.d",
  "/lib/udev/rules.d",
].map(dir => `${dir}/70-ipkvm-os-services.rules`);

/** The computer's OS Services installation as the test found it. */
export interface InstallationState {
  /** The unit file is there, and the unit runs. */
  installed: boolean;
  active: boolean;
  /** The bench's systemd drop-in for the unit, which the test keeps. */
  dropin: boolean;
  approvals: ApprovalList;
}

/** What the Linux target computer does while OS Services is installed on it. */
export class InstallerHost {
  readonly target: TargetHost;

  constructor(target: TargetHost) {
    this.target = target;
  }

  /** Whether an earlier run saved an installation it did not put back. */
  async hasSavedInstallation(): Promise<boolean> {
    return this.target.succeeds(PATHS + '[ -f "$dir/state" ]');
  }

  /** Keeps `note` (the Mini's side of the bench) with the saved installation. */
  async writeNote(note: string): Promise<void> {
    await this.target.text(PATHS + 'cat > "$dir/mini.json"', { input: note });
  }

  /** The note an earlier run kept with its saved installation, if any. */
  async readNote(): Promise<string | undefined> {
    const result = await this.target.run(PATHS + 'cat "$dir/mini.json"');
    return result.status === 0 ? result.stdout.toString("utf8") : undefined;
  }

  /** Whether the .deb's unit serves this computer; the test installs the AppImage. */
  async hasPackage(): Promise<boolean> {
    return this.target.succeeds(`[ -e /usr/lib/systemd/user/${SENDER_UNIT} ]`);
  }

  /**
   * Saves the installation (the unit, its enablement, the sender's settings
   * and approved KVMs, its AppImage, the udev rule and the capture helper,
   * with their owners and capabilities) beside the user's cache, and notes
   * the unit's state, the capture group and each KVM's device node ACL,
   * for restore().
   */
  async save(): Promise<InstallationState> {
    const approvals = await this.target.readApprovalList();
    const text = await this.target.text(
      PATHS +
        "set -e; " +
        'mkdir -p "$dir"; : > "$dir/user-files"; : > "$dir/system-files"; ' +
        'for f in "$unit" "$wants" "$data"; do ' +
        'if [ -e "$f" ] || [ -L "$f" ]; then printf "%s\\0" "${f#/}" >> "$dir/user-files"; fi; done; ' +
        // An AppImage that ran from a disc is not saved: the disc has it.
        'if [ -f "$prog" ] && [ -w "$(dirname "$prog")" ]; then printf "%s\\0" "${prog#/}" >> "$dir/user-files"; fi; ' +
        'for f in "$rule" "$helper"; do ' +
        'if [ -e "$f" ]; then printf "%s\\0" "${f#/}" >> "$dir/system-files"; fi; done; ' +
        // The user's files as the user, so that the folders tar creates on
        // the way back are the user's; the system files with sudo, with
        // their owners and the helper's capability.
        'if [ -s "$dir/user-files" ]; then tar -cpf "$dir/user.tar" -C / --null -T "$dir/user-files"; fi; ' +
        'if [ -s "$dir/system-files" ]; then sudo -n tar -cpf "$dir/system.tar" --xattrs --xattrs-include="*" ' +
        '-C / --null -T "$dir/system-files"; fi; ' +
        // The KVM's device node ACL, which may grant access without the rule.
        // Each KVM's whole ACL, under its USB port.
        'kvms | while read -r name bus dev; do getfacl -cp "$(usbnode "$bus" "$dev")" > "$dir/acl-$name"; done; ' +
        `active=$(systemctl --user is-active ${SENDER_UNIT} || true); ` +
        '{ echo "active=$active"; echo "group=$(getent group ipkvm-capture || true)"; } > "$dir/state.new"; ' +
        'mv "$dir/state.new" "$dir/state"; ' +
        '[ -e "$unit" ] && echo installed=yes || echo installed=no; ' +
        'echo "active=$active"; ' +
        '[ -d "$dropin" ] && echo dropin=yes || echo dropin=no',
      { timeoutMs: 120_000 },
    );
    const values = Object.fromEntries(
      text
        .trim()
        .split("\n")
        .map(line => line.split("=", 2)),
    );
    return {
      installed: values.installed === "yes",
      active: values.active === "active",
      dropin: values.dropin === "yes",
      approvals,
    };
  }

  /**
   * Leaves the computer as one that never had OS Services: the sender's own
   * uninstall, then its AppImage, settings, udev rule and capture helper
   * removed, and the access the rule gave to the attached KVM taken back.
   * The bench's drop-in stays.
   */
  async clean(): Promise<void> {
    await this.target.text(
      PATHS +
        "set -e; " +
        'if [ -f "$prog" ]; then "$prog" --uninstall-service; fi; ' +
        `if [ -e "$unit" ]; then systemctl --user disable --now ${SENDER_UNIT}; fi; ` +
        'rm -f "$unit" "$wants"; systemctl --user daemon-reload; ' +
        `systemctl --user reset-failed ${SENDER_UNIT} 2>/dev/null || true; ` +
        'if [ -f "$prog" ] && [ -w "$(dirname "$prog")" ]; then rm -f "$prog"; fi; ' +
        'rm -rf "$data"; ' +
        'sudo -n rm -f "$rule" "$helper"; ' +
        "sudo -n udevadm control --reload-rules; " +
        'kvms | while read -r name bus dev; do sudo -n setfacl -b "$(usbnode "$bus" "$dev")"; done',
      { timeoutMs: 60_000 },
    );
  }

  /** What still gives the user OS Services; empty on a clean computer. */
  async leftovers(): Promise<string[]> {
    const text = await this.target.text(
      PATHS +
        `[ "$(systemctl --user show -p LoadState --value ${SENDER_UNIT})" = not-found ] || echo "the unit ${SENDER_UNIT}"; ` +
        '[ ! -e "$data" ] || echo "$data"; ' +
        '[ ! -e "$helper" ] || echo "$helper"; ' +
        RULE_COPIES.map(rule => `[ ! -e ${rule} ] || echo ${rule}; `).join("") +
        'kvms | while read -r name bus dev; do n=$(usbnode "$bus" "$dev"); ' +
        '[ ! -w "$n" ] || echo "write access to the KVM $n"; done',
    );
    return text.split("\n").filter(Boolean);
  }

  /** Whether the drop-in that the bench keeps for the unit is there. */
  async hasDropin(): Promise<boolean> {
    return this.target.succeeds(PATHS + '[ -d "$dropin" ]');
  }

  /**
   * Puts back the installation save() kept: removes what the test installed,
   * restores the saved files, the capture group and the KVM's access, and
   * starts the sender if it ran. Does nothing without a saved installation.
   */
  async restore(): Promise<void> {
    await this.target.text(
      PATHS +
        'if [ ! -f "$dir/state" ]; then exit 0; fi; ' +
        "set -e; user=$(id -un); " +
        `if [ -e "$unit" ]; then systemctl --user disable --now ${SENDER_UNIT}; fi; ` +
        'rm -f "$unit" "$wants"; rm -rf "$data"; sudo -n rm -f "$rule" "$helper"; ' +
        'if [ -s "$dir/user-files" ]; then tar -xpf "$dir/user.tar" -C /; fi; ' +
        'if [ -s "$dir/system-files" ]; then sudo -n tar -xpf "$dir/system.tar" --xattrs --xattrs-include="*" ' +
        "--same-owner -C /; fi; " +
        "systemctl --user daemon-reload; " +
        `systemctl --user reset-failed ${SENDER_UNIT} 2>/dev/null || true; ` +
        "sudo -n udevadm control --reload-rules; " +
        'member() { echo "$1" | cut -d: -f4 | tr , \'\\n\' | grep -qx "$user"; }; ' +
        "before=$(sed -n 's/^group=//p' \"$dir/state\"); now=$(getent group ipkvm-capture || true); " +
        'if [ -z "$before" ] && [ -n "$now" ]; then sudo -n groupdel ipkvm-capture; ' +
        'elif [ -n "$now" ] && ! member "$before" && member "$now"; then sudo -n gpasswd -d "$user" ipkvm-capture > /dev/null; fi; ' +
        "kvms | while read -r name bus dev; do " +
        'if [ -e "$rule" ]; then sudo -n udevadm trigger --action=change --subsystem-match=usb --sysname-match="$name"; ' +
        'else n=$(usbnode "$bus" "$dev"); ' +
        'if [ -s "$dir/acl-$name" ]; then sudo -n setfacl --set-file="$dir/acl-$name" "$n"; ' +
        'else sudo -n setfacl -b "$n"; fi; fi; done; ' +
        `if [ "$(sed -n 's/^active=//p' "$dir/state")" = active ]; then systemctl --user start ${SENDER_UNIT}; fi; ` +
        'rm -rf "$dir"',
      { timeoutMs: 120_000 },
    );
  }

  // --- The installer disc ---

  /**
   * The mount point of the installer disc the KVM shows, of the volume
   * `volume`. GNOME lists the disc in the Files app but does not mount it on
   * its own: udisks gives a USB CD-ROM no automount hint. So, as the user's
   * click on the disc in Files does, it mounts the disc through GIO once the
   * disc is there.
   */
  async discMountPoint(timeout: number, volume = INSTALLER_VOLUME): Promise<string> {
    let last = "";
    return pollUntil(
      async () => {
        last = await this.target.text(
          PATHS +
            "kvms | while read -r name bus dev; do for b in /sys/block/sr*; do " +
            'case "$(readlink -f "$b/device")" in */"$name"/*) d="/dev/${b##*/}"; ' +
            'echo "$d"; lsblk -dno LABEL "$d" || echo; findmnt -rn -o TARGET -S "$d" || echo;; ' +
            "esac; done; done",
        );
        const [device, label, mountPoint] = last.split("\n").map(line => line.trim());
        if (!device || label !== volume) return undefined;
        if (mountPoint)
          return mountPoint.replace(/\\x([0-9a-f]{2})/gi, (_, hex: string) =>
            String.fromCharCode(Number.parseInt(hex, 16)),
          );
        // GNOME may mount it at the same moment; the next round reads the mount.
        await this.target.run(USER_SESSION + `gio mount -d ${quote(device)}`);
        return undefined;
      },
      {
        timeout,
        interval: 250,
        message: () => `the disc ${volume} mounted on ${this.target.login}: ${last}`,
      },
    );
  }

  /**
   * Unmounts the KVM's disc on the computer, as ejecting it in Files does.
   * A mount through an /etc/fstab line (Debian's installer writes one for
   * /dev/sr0) outlives the disc, so the test unmounts it before the KVM
   * takes the disc away.
   */
  async unmountDisc(): Promise<void> {
    await this.target.text(
      PATHS +
        "kvms | while read -r name bus dev; do for b in /sys/block/sr*; do " +
        'case "$(readlink -f "$b/device")" in */"$name"/*) d="/dev/${b##*/}"; ' +
        'while findmnt -rn -S "$d" > /dev/null; do udisksctl unmount --no-user-interaction -b "$d" > /dev/null 2>&1 || umount "$d" || exit 1; done;; ' +
        "esac; done; done",
    );
  }

  /** Waits until no disc from the KVM is mounted. */
  async waitForDiscGone(timeout: number): Promise<void> {
    await pollUntil(
      async () => {
        const text = await this.target.text(
          PATHS +
            "kvms | while read -r name bus dev; do for b in /sys/block/sr*; do " +
            'case "$(readlink -f "$b/device")" in */"$name"/*) findmnt -rn -o TARGET -S "/dev/${b##*/}" || true;; esac; ' +
            "done; done",
        );
        return text.trim() === "" ? true : undefined;
      },
      { timeout, message: () => `the KVM's disc unmounted on ${this.target.login}` },
    );
  }

  /**
   * The files on the built-in disc at `mountPoint` and the links of its page
   * BUILTIN_INSTALLER_PAGE, as the user sees them in Files and the browser.
   */
  async builtinDisc(mountPoint: string): Promise<{ files: string[]; links: string[] }> {
    const files = (await this.target.text(`ls -1A ${quote(mountPoint)}`))
      .split("\n")
      .filter(Boolean);
    const page = await this.target.text(`cat ${quote(`${mountPoint}/${BUILTIN_INSTALLER_PAGE}`)}`);
    const links = [...page.matchAll(/href="([^"]+)"/g)].map(match => match[1]);
    return { files, links };
  }

  /** Whether programs on the mount at `mountPoint` can run (no noexec). */
  async allowsPrograms(mountPoint: string): Promise<boolean> {
    const options = await this.target.text(`findmnt -rn -o OPTIONS -T ${quote(mountPoint)}`);
    return !options.trim().split(",").includes("noexec");
  }

  /**
   * Copies `image` to a folder of the user's, as install.md's first step
   * says, and returns the copy. The folder goes with restore().
   */
  async copyToHome(image: string): Promise<string> {
    return (
      await this.target.text(
        PATHS +
          'mkdir -p "$dir/app" && ' +
          `cp ${quote(image)} "$dir/app/" && chmod 755 "$dir/app/$(basename ${quote(image)})" && ` +
          `echo "$dir/app/$(basename ${quote(image)})"`,
        { timeoutMs: 60_000 },
      )
    ).trim();
  }

  /** The Linux AppImage on the disc. */
  async appImage(mountPoint: string): Promise<string> {
    const found = (
      await this.target.text(`ls -1 ${quote(mountPoint)}/*_linux-x86_64.AppImage`)
    ).trim();
    const images = found.split("\n").filter(Boolean);
    if (images.length !== 1)
      throw new Error(`${mountPoint} holds ${images.length} Linux AppImages: ${found}`);
    return images[0];
  }

  /**
   * Opens `image` as a double-click in the Files app does: a process of its
   * own in the user's graphical session, with the session's display and bus,
   * from the home folder. Returns a folder where its exit status appears;
   * close() ends what is left of it.
   */
  async open(image: string): Promise<string> {
    const run = (
      await this.target.text(
        USER_SESSION +
          "eval \"$(systemctl --user show-environment | grep -E '^(WAYLAND_DISPLAY|DISPLAY|XAUTHORITY|XDG_SESSION_TYPE|XDG_CURRENT_DESKTOP|XDG_SESSION_DESKTOP|DESKTOP_SESSION)=' | sed 's/^/export /')\"; " +
          'run=$(mktemp -d "${TMPDIR:-/tmp}/osvc-e2e-installer.XXXXXX"); cd "$HOME"; ' +
          // The shell leads a session of its own: its pid names the group.
          `setsid -f sh -c 'echo $$ > "$2/pid"; "$1"; echo $? > "$2/status"' sh ${quote(image)} "$run" ` +
          '< /dev/null > "$run/output" 2>&1; echo "$run"',
      )
    ).trim();
    return run;
  }

  /**
   * Ends the processes open() started that still run, such as an installer
   * that never exited, and removes its folder. The sender the systemd unit
   * runs is not one of them.
   */
  async close(run: string): Promise<void> {
    await this.target.text(
      `r=${quote(run)}; if [ -s "$r/pid" ]; then pkill -KILL -s "$(cat "$r/pid")" || true; fi; rm -rf "$r"`,
    );
  }

  /** The opened installer's exit status and output, once it has exited. */
  async opened(run: string, timeout: number): Promise<{ status: number; output: string }> {
    return pollUntil(
      async () => {
        const result = await this.target.run(
          `[ -f ${quote(run)}/status ] && cat ${quote(run)}/status && cat ${quote(run)}/output`,
        );
        if (result.status !== 0) return undefined;
        const [status, ...output] = result.stdout.toString("utf8").split("\n");
        return { status: Number(status), output: output.join("\n").trim() };
      },
      { timeout, message: () => `the installer on the disc exits (${run})` },
    );
  }

  // --- The sender's dialogs ---

  /** The sender's pid while it shows the zenity dialog `title`, else undefined. */
  async dialogOwner(title: string): Promise<number | undefined> {
    const text = await this.target.text('ps -u "$(id -u)" -o ppid=,args=');
    const line = text
      .split("\n")
      .find(entry => entry.includes("zenity --question") && entry.includes(`--title ${title}`));
    return line ? Number(line.trim().split(/\s+/)[0]) : undefined;
  }

  /**
   * Installs USB access as "Allow USB Access" does, with sudo in place of
   * pkexec: the program on the disc with ACCESS_FLAG, as root, for the login
   * user (SUDO_UID). Returns its output.
   */
  async installAccess(image: string): Promise<string> {
    return this.target.text(`sudo -n ${quote(image)} ${ACCESS_FLAG} 2>&1`, { timeoutMs: 60_000 });
  }

  /**
   * Clicks `button` of the zenity dialog that shows every one of `texts`,
   * through AT-SPI (tools/e2e/os_services_dialog.py). False while no such
   * dialog is on screen.
   */
  async click(button: string, texts: string[]): Promise<boolean> {
    const result = await this.target.run(
      USER_SESSION + `python3 - ${[button, ...texts].map(quote).join(" ")}`,
      { input: readFileSync(path.resolve(toolsDir(), "os_services_dialog.py")) },
    );
    const report = result.stdout.toString("utf8").trim();
    if (result.status === 0) return true;
    if (result.status === 3) return false;
    throw new Error(`the dialog script failed (exit ${result.status}): ${report} ${result.stderr}`);
  }

  /** Whether the computer can click dialogs through AT-SPI. */
  async hasAccessibility(): Promise<boolean> {
    return this.target.succeeds(
      USER_SESSION +
        'python3 -c \'import gi; gi.require_version("Atspi", "2.0"); from gi.repository import Atspi; Atspi.get_desktop(0)\'',
    );
  }
}
