import { test, expect, type BrowserContext, type Page } from "@playwright/test";
import { callJsonRpc, ensureRpcReady, waitForWebRTCReady } from "../helpers";
import { restoreMedia, type MediaState } from "../helpers/hardware-state";
import { skipWithoutRemoteHost } from "../remote-agent/remote-agent";
import {
  TargetHost,
  connectedState,
  normalizeFingerprint,
  osServicesState,
  pollUntil,
  rpcAnswer,
  setBridgeSettings,
  settingsRequest,
  waitForState,
  type OsServicesSettings,
} from "./os-services";
import {
  APPROVAL_DIALOG,
  BUILTIN_INSTALLER_FILENAME,
  BUILTIN_INSTALLER_PAGE,
  BUILTIN_VOLUME,
  INSTALLER_FILENAME,
  InstallerHost,
  USB_ACCESS_DIALOG,
  builtinInstallerLink,
  type InstallationState,
} from "./os-services-installer";

// A user installs OS Services on a Linux computer from the KVM, as on a
// computer that never had it. Each test starts with the test's removal of
// OS Services from the remote host (JETKVM_REMOTE_HOST), as its own
// uninstall does, with the AppImage, the sender's settings, the udev rule,
// the capture helper and the access the rule gave to the KVM.
//
// The first test clicks "Mount installer" in the OS Services settings. The
// Mini mounts the installer disc in its firmware as a CD-ROM at once, with
// no TF card and no download. The test mounts the disc on the computer as a
// click on it in the Files app does, and checks that it holds only
// "Install OS Services.html", whose one link is the download page for the
// Mini's firmware version. A second mount is refused while the disc is
// mounted, and the test unmounts it.
//
// The second test installs the sender from the offline installer in the
// button's menu:
//
//   1. the Mini saves the offline ISO to its TF card, then mounts it;
//   2. the test mounts the disc as above and opens the disc's Linux AppImage
//      in the user's graphical session, as a double-click does, from a copy
//      in the home folder if the disc is mounted noexec;
//   3. the sender cannot open the KVM and asks "Allow USB access?". The
//      test installs the access that "Allow USB Access" installs through
//      pkexec, the AppImage with --install-linux-access, with sudo: pkexec
//      would need the user's password at a polkit dialog. The sender closes
//      its question once the rule is in place;
//   4. the sender asks "Allow this KVM?" with the Mini's fingerprint, the test
//      clicks "Allow", and the sender connects within CONNECT_LIMIT_MS of
//      opening the AppImage.
//
// The tests need a GNOME session on the remote host with AT-SPI (python3-gi,
// gir1.2-atspi-2.0), zenity and setfacl, and passwordless sudo. The second
// test needs a TF card and a release bucket with the offline ISO, whose
// AppImage asks for USB access. The tests put back what they removed,
// whether they pass or fail.

/** The download of the offline ISO to the TF card, from the internet. */
const DOWNLOAD_LIMIT_MS = 300_000;
/** From the Mini's CD-ROM mount to the disc mounted on the computer. */
const DISC_LIMIT_MS = 30_000;
/** From opening the AppImage to the sender's session with the Mini. */
const CONNECT_LIMIT_MS = 20_000;

interface StorageDownload {
  state: "idle" | "downloading" | "done" | "error";
  filename: string;
  bytes: number;
  total: number;
  reused: boolean;
  error: string;
}

/** The Mini's side of the bench that the test changes and puts back. */
interface MiniBench {
  media: MediaState | null;
  installerOnCard: boolean;
}

async function cardFiles(page: Page): Promise<string[]> {
  const { files } = (await callJsonRpc(page, "listStorageFiles")) as {
    files: { filename: string }[];
  };
  return files.map(file => file.filename);
}

/** Removes the installer ISO and a partial download of it from the TF card. */
async function deleteInstallerFromCard(page: Page): Promise<void> {
  for (const filename of await cardFiles(page)) {
    if (filename === INSTALLER_FILENAME || filename === `${INSTALLER_FILENAME}.incomplete`)
      await callJsonRpc(page, "deleteStorageFile", { filename });
  }
}

test.describe("OS Services install from the installer disc (Mini)", () => {
  test.describe.configure({ mode: "serial" });

  let context: BrowserContext | undefined;
  let page: Page;
  let host: InstallerHost;
  let fingerprint: string;
  let settings: OsServicesSettings;
  let media: MediaState | null;
  /** The Mini has a TF card with a mounted filesystem. */
  let cardReady = false;
  let installerOnCard = false;
  let installation: InstallationState | undefined;
  /** The folder of the opened installer on the computer (host.open). */
  let run: string | undefined;

  /** No OS Services on the computer, no disc on it and no media on the Mini. */
  async function cleanStart(): Promise<void> {
    await host.clean();
    expect(await host.leftovers(), "OS Services left on the computer").toEqual([]);
    expect(await host.hasDropin(), "the bench's drop-in for the unit").toBe(installation!.dropin);
    await host.unmountDisc();
    if (await callJsonRpc(page, "getVirtualMediaState")) await callJsonRpc(page, "unmountImage");
    await host.waitForDiscGone(30_000);
    await waitForState(
      page,
      "no OS Services session",
      state => !state.osServices.connected && !state.osServices.approvalPending,
    );
  }

  test.beforeAll(async ({ browser }) => {
    test.setTimeout(300_000);
    skipWithoutRemoteHost();
    const target = TargetHost.fromEnv();
    test.skip(
      await target.isMacos(),
      "the test opens the Linux AppImage; the macOS DMG has no test yet",
    );
    host = new InstallerHost(target);
    test.skip(
      !(await target.succeeds("sudo -n true")),
      `${target.login} needs passwordless sudo (to install USB access, and to save and restore it)`,
    );
    test.skip(
      !(await target.succeeds("command -v zenity && command -v setfacl")),
      `${target.login} needs zenity and setfacl (acl)`,
    );
    test.skip(
      !(await host.hasAccessibility()),
      `${target.login} needs AT-SPI in its desktop session (python3-gi, gir1.2-atspi-2.0) to click the dialogs`,
    );
    test.skip(
      await host.hasPackage(),
      `${target.login} has the OS Services .deb; the test installs the AppImage on a computer without OS Services`,
    );

    context = await browser.newContext({ baseURL: process.env.JETKVM_URL });
    page = await context.newPage();
    await ensureRpcReady(page, { timeoutMs: 90_000, navigateFirst: true });
    const answer = await rpcAnswer(page, "getOsServicesSettings");
    expect(answer.error, "getOsServicesSettings").toBeUndefined();
    const download = await rpcAnswer(page, "getStorageDownloadState");
    expect(download.error, "getStorageDownloadState").toBeUndefined();
    settings = answer.result as OsServicesSettings;
    fingerprint = (await osServicesState(page)).osServices.fingerprint;
    const storage = (await callJsonRpc(page, "getStorageState")) as {
      cardPresent: boolean;
      filesystemMounted: boolean;
    };
    cardReady = storage.cardPresent && storage.filesystemMounted;

    // A run that stopped before its restore left the installation saved,
    // and the Mini as it found it in a note: the installer the Mini shows
    // now is that run's.
    let mini: MiniBench = {
      media: (await callJsonRpc(page, "getVirtualMediaState")) as MediaState | null,
      installerOnCard: cardReady && (await cardFiles(page)).includes(INSTALLER_FILENAME),
    };
    if (await host.hasSavedInstallation()) {
      const note = await host.readNote();
      if (note) mini = JSON.parse(note) as MiniBench;
      await host.restore();
    }
    ({ media, installerOnCard } = mini);
    installation = await host.save();
    await host.writeNote(JSON.stringify(mini));
  });

  test.afterAll(async () => {
    test.setTimeout(300_000);
    const errors: unknown[] = [];
    const step = async (action: () => Promise<unknown>) => {
      try {
        await action();
      } catch (error) {
        errors.push(error);
      }
    };
    if (installation) {
      // The sender the test installed runs from the disc: it stops first.
      if (run) await step(() => host.close(run!));
      await step(() => host.restore());
      await step(async () => {
        await ensureRpcReady(page, { timeoutMs: 60_000 });
        await host.unmountDisc();
        await restoreMedia(page, media);
        if (!media) await host.waitForDiscGone(30_000);
        // An installer that was on the card before stays there as the copy
        // the test downloaded, a newer one of the same release ISO, which the
        // media restored above may name.
        if (cardReady && !installerOnCard) await deleteInstallerFromCard(page);
      });
      await step(async () => {
        await setBridgeSettings(page, settingsRequest(settings));
        const approved = installation!.approvals.content
          .split("\n")
          .some(line => normalizeFingerprint(line) === normalizeFingerprint(fingerprint));
        if (installation!.active && approved)
          await waitForState(page, "the sender is back", connectedState, 60_000);
      });
      await step(async () => {
        expect(await host.hasDropin(), "the bench's drop-in for the unit").toBe(
          installation!.dropin,
        );
        expect(await host.target.senderActive(), "the sender runs as it did").toBe(
          installation!.active,
        );
      });
    }
    await step(async () => context?.close());
    if (errors.length) throw new AggregateError(errors, "restoring the remote host failed");
  });

  test("Mount installer mounts the disc in the firmware, whose page links to the download", async () => {
    test.setTimeout(DISC_LIMIT_MS + 120_000);

    await test.step("clean start: no OS Services on the computer, no disc", cleanStart);

    await test.step("Mount installer: the Mini mounts its built-in disc at once", async () => {
      await page.goto("/settings/os-services");
      await waitForWebRTCReady(page, 60_000);
      const button = page.getByTestId("os-services-mount-installer");
      await expect(button).toBeEnabled({ timeout: 30_000 });
      await button.click();
      await expect(
        page.getByText("Installer mounted. Open the disc on the target computer."),
      ).toBeVisible({ timeout: 30_000 });
      const mounted = (await callJsonRpc(page, "getVirtualMediaState")) as MediaState & {
        size: number;
      };
      expect(mounted).toMatchObject({
        source: "Builtin",
        mode: "CDROM",
        filename: BUILTIN_INSTALLER_FILENAME,
      });
      expect(mounted.size % 2048, "the disc's size in whole CD-ROM sectors").toBe(0);
    });

    await test.step("a second mount is refused while the disc is mounted", async () => {
      const again = await rpcAnswer(page, "mountBuiltinInstaller", {}, 30_000);
      expect(again.error?.data).toBe("An image is already mounted. Unmount it first.");
      expect(await callJsonRpc(page, "getVirtualMediaState")).toMatchObject({
        source: "Builtin",
      });
    });

    await test.step("the computer shows the disc's one page, with the link for the Mini's version", async () => {
      const { systemVersion } = (await callJsonRpc(page, "getLocalVersion")) as {
        systemVersion: string;
      };
      const mountPoint = await host.discMountPoint(DISC_LIMIT_MS, BUILTIN_VOLUME);
      const disc = await host.builtinDisc(mountPoint);
      expect(disc.files).toEqual([BUILTIN_INSTALLER_PAGE]);
      expect(disc.links).toEqual([builtinInstallerLink(systemVersion)]);
    });

    await test.step("the user unmounts the disc", async () => {
      await host.unmountDisc();
      await callJsonRpc(page, "unmountImage");
      expect(await callJsonRpc(page, "getVirtualMediaState")).toBeNull();
      await host.waitForDiscGone(30_000);
    });
  });

  test("a user installs the sender from the offline installer disc", async () => {
    test.skip(!cardReady, "the Mini needs a TF card for the offline installer");
    test.setTimeout(DOWNLOAD_LIMIT_MS + DISC_LIMIT_MS + CONNECT_LIMIT_MS + 180_000);
    const times: Record<string, number> = {};

    await test.step("clean start: no OS Services on the computer, no installer on the card", async () => {
      await cleanStart();
      await deleteInstallerFromCard(page);
    });

    await test.step("Offline installer: the Mini saves the ISO to the card and mounts it", async () => {
      await page.goto("/settings/os-services");
      await waitForWebRTCReady(page, 60_000);
      const menu = page.getByTestId("os-services-installer-menu");
      await expect(menu).toBeEnabled({ timeout: 30_000 });
      const clicked = Date.now();
      await menu.click();
      await page.getByRole("menuitem", { name: /^Offline installer/ }).click();
      await expect(page.getByText(/^Saving the installer to the TF card: \d+%/)).toBeVisible({
        timeout: 30_000,
      });
      const download = await pollUntil(
        async () => {
          const state = (await callJsonRpc(page, "getStorageDownloadState")) as StorageDownload;
          const ended = state.state === "done" || state.state === "error";
          return state.filename === INSTALLER_FILENAME && ended ? state : undefined;
        },
        { timeout: DOWNLOAD_LIMIT_MS, interval: 500, message: () => "the ISO on the TF card" },
      );
      times.download = Date.now() - clicked;
      expect(download.error, "the download's error").toBe("");
      expect(download.state).toBe("done");
      expect(download.reused, "a new download, not the card's copy").toBe(false);
      await expect(
        page.getByText("Installer mounted. Open the disc on the target computer."),
      ).toBeVisible({ timeout: 30_000 });
      expect(await callJsonRpc(page, "getVirtualMediaState")).toMatchObject({
        source: "Storage",
        mode: "CDROM",
        filename: INSTALLER_FILENAME,
      });
      times.mount = Date.now() - clicked - times.download;
    });

    let image = "";
    let opened = 0;
    await test.step("the desktop mounts the disc; the user opens the AppImage", async () => {
      const mountRequested = Date.now();
      const mountPoint = await host.discMountPoint(DISC_LIMIT_MS);
      times.disc = Date.now() - mountRequested;
      image = await host.appImage(mountPoint);
      if (!(await host.allowsPrograms(mountPoint))) {
        // A noexec mount, such as Debian's /etc/fstab line for /dev/sr0,
        // cannot run the AppImage where it is; the user copies it first.
        test.info().annotations.push({
          type: "disc",
          description: `${mountPoint} is mounted noexec; the AppImage runs from a copy in the home folder`,
        });
        image = await host.copyToHome(image);
      }
      opened = Date.now();
      run = await host.open(image);
      const exit = await host.opened(run, 30_000);
      expect(exit.status, `the AppImage's exit status; output: ${exit.output}`).toBe(0);
      times.registered = Date.now() - opened;
    });

    await test.step("the sender asks for USB access; the access is installed", async () => {
      const sender = await pollUntil(() => host.dialogOwner(USB_ACCESS_DIALOG.title), {
        timeout: 30_000,
        message: () =>
          `the dialog "${USB_ACCESS_DIALOG.title}" (an older AppImage, ` +
          `such as ${image}, does not ask)`,
      });
      times.usbPrompt = Date.now() - opened;
      const unit = await host.target.unitProperties();
      expect(Number(unit.MainPID), "the dialog is the service's").toBe(sender);
      // What "Allow USB Access" runs through pkexec, with sudo in place of
      // the password at polkit's dialog.
      const output = await host.installAccess(image);
      times.accessInstalled = Date.now() - opened;
      await pollUntil(
        async () =>
          (await host.dialogOwner(USB_ACCESS_DIALOG.title)) === undefined ? true : undefined,
        {
          timeout: 15_000,
          message: () =>
            `the sender closes "${USB_ACCESS_DIALOG.title}"; install output: ${output}`,
        },
      );
    });

    await test.step("the user allows the Mini; the sender connects", async () => {
      await pollUntil(
        async () =>
          (await host.click(APPROVAL_DIALOG.button, [
            APPROVAL_DIALOG.title,
            `Fingerprint: ${fingerprint}`,
          ])) || undefined,
        {
          timeout: 30_000,
          interval: 500,
          message: () => `"${APPROVAL_DIALOG.title}" for ${fingerprint} clicked`,
        },
      );
      times.approved = Date.now() - opened;
      const state = await waitForState(
        page,
        "the sender connects",
        connectedState,
        Math.max(CONNECT_LIMIT_MS - (Date.now() - opened), 1_000),
      );
      times.connected = Date.now() - opened;
      expect(state.osServices.sender?.version, "the sender reports its version").toBeTruthy();
      expect(await host.leftovers(), "what the installer put on the computer").toEqual(
        expect.arrayContaining([
          "/etc/udev/rules.d/70-ipkvm-os-services.rules",
          "/usr/local/libexec/drmtap-helper",
        ]),
      );
    });

    test.info().annotations.push({ type: "times (ms)", description: JSON.stringify(times) });
    console.log(`os-services-install times (ms): ${JSON.stringify(times)}`);
    expect(times.connected, "from opening the AppImage to the session").toBeLessThanOrEqual(
      CONNECT_LIMIT_MS,
    );
  });
});
