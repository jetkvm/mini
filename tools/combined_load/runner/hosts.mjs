// What the tool runs on the host, the computer that the Mini's USB and HDMI
// connect to, for each operating system of the host. Every command is POSIX
// sh, which the tool starts over SSH as `sh -c <command>`. A host is:
//
//   watchKeys      prints "p<usage>" for each press of a key of HID_KEYS and
//                  "m<x> <ms>" for each move of the pointer to x that the
//                  host gets from the Mini's USB keyboard and mice, and "."
//                  once per second, until its SSH session closes. <ms> is
//                  the time on the host's clock at which the host got the
//                  move. The watch reports each move of a mouse that moves
//                  30 times per second
//   watchMedia     prints at least once per second one line for each medium
//                  of the Mini that the host has, or "none", until its SSH
//                  session closes
//   medium(fields) the type ("rom" or "disk"), the device and the bytes read
//                  of one such line, split at spaces, and the reader's counts
//                  of the blocks that it checked and that were wrong, when
//                  the line has them
//   canRead        optional: fails with the reason when the host cannot read
//                  the Mini's media
//   expectBlocks(sums, { firstReads, forget })  says what the next reader
//                  will read: the SHA-256 of each block of 1 MiB of the
//                  medium, in their order. It sets the reader's counts to
//                  zero. With firstReads the reader notes which blocks it
//                  has read, also from one reader to the next, and counts a
//                  wrong block apart when it reads that block for the first
//                  time; forget empties that note first
//   startReader    starts a reader that continues after the command: it reads
//                  the whole medium again and again, past the host's cache,
//                  and stops by itself after an hour. It compares the
//                  SHA-256 of each whole block that it has read with the
//                  one that expectBlocks named, and counts the blocks that
//                  it checked, those that were wrong at their first read,
//                  and those that were wrong at a later read
//   stopReader     stops that reader, also the reader of an earlier run
//   releaseMedia   optional, for a host that keeps a medium which the Mini
//                  unmounts: gives up the Mini's media before the unmount,
//                  and prints each one that the host still has
//   closeDiskDialogs  optional, for a host that shows a dialog for each
//                  medium of the Mini that it cannot read: closes them
//   senderPackage  the name of the host's sender package in releases.json
//   senderSuffix   the end of the file name of such a package, its type
//   installSender  installs one release of the sender with the Mini's key
//                  approved, whatever was there before, and leaves it stopped
//   startSender, stopSender
//   checksumsAndRemove(paths)  prints, with one process for all of them, a
//                  line with the SHA-256 and the path of each of the files
//                  that exist, and removes the files
//   countInTerminal  what to type into the sender's terminal so that the
//                  host counts up from 1, one number per line, without end,
//                  in writes of about 8 KiB: the same work for the session
//                  on every host. Each write begins with a line of its own
//                  that has "t" and the host's clock in ms at that time
//   answerInTerminal  what to type so that the host prints "ready" and
//                  then answers each line "e<n>" that it gets with the line
//                  "r<n>" at once: a terminal in which a user types short
//                  commands
//   displayMode(size)  optional (a macOS host has none):
//                  prints "width height rate" of the mode that the host's
//                  display has, which is the one that the Mini captures.
//                  With size ({ width, height, hz }) it then sets a mode of
//                  that size, at the rate nearest to hz or, without hz, to
//                  the current rate, as the host's display settings do,
//                  for the desktop session only: the host does not store
//                  the mode
//   fullVolume     optional, for a host that plays into the Mini's audio
//                  output below full volume: sets that output's volume to
//                  full, so that the Mini gets the sound at the level at
//                  which the target page plays it
//   openPage(url)  shows the page at url in a browser window of its own on
//                  the host's screen, in the desktop session of the SSH
//                  login's user, in place of the page that an earlier call
//                  opened; the screen is on, and stays on while the page is
//                  open
//   closePage      closes that page, also the page of an earlier run

export const quote = value => `'${value.replace(/'/g, "'\\''")}'`;

// The reader's process group ID, or on Windows its process ID, on the host.
const READER_FILE = '/tmp/jetkvm-combined-load-reader.pgid';
// What the reader is to read: a first line "first" or "all", then one
// SHA-256 per line for each block of the medium. The reader's counts: the
// blocks that it has checked, those that had another SHA-256 at their first
// read, and those that had another one at a later read, as three numbers.
// The numbers of the blocks that a reader has read, for a medium whose first
// reads count apart ("first").
const EXPECTED_FILE = '/tmp/jetkvm-combined-load-blocks.sha256';
const BLOCKS_FILE = '/tmp/jetkvm-combined-load-reader.blocks';
const READ_BLOCKS_FILE = '/tmp/jetkvm-combined-load-reader.read';
// Prints what a reader with first reads has noted: a line with the numbers
// of the blocks that it has read, and a line with its three counts. Either
// line is empty before the reader has written it. The integration tests read
// it (tools/integration/lib/host.mjs).
export const READER_REPORT = `cat ${READ_BLOCKS_FILE} 2>/dev/null; echo; cat ${BLOCKS_FILE} 2>/dev/null; true`;
const expectBlocks = (sums, { firstReads = false, forget = false } = {}) =>
  (forget ? `rm -f ${READ_BLOCKS_FILE}; ` : '') +
  `printf '%s\\n' ${firstReads ? 'first' : 'all'} ${sums.join(' ')} > ${EXPECTED_FILE}; echo "0 0 0" > ${BLOCKS_FILE}`;
// The reader's counts in a line of the watch: "blocks=<checked>,<wrong at the first read>,<wrong later>".
const blocksOf = fields => {
  const counts = /^blocks=(\d+),(\d+),(\d+)$/.exec(fields.find(field => field.startsWith('blocks=')) ?? '');
  return counts ? { blocksChecked: Number(counts[1]), blocksWrongFirst: Number(counts[2]), blocksWrongLater: Number(counts[3]) } : {};
};
// Python, for every host's reader: Blocks(counts, expected, read) takes the
// bytes of a pass over a medium with add(), from the medium's start after
// begin(), and counts each whole block of 1 MiB in the file `counts`, as
// checked and, with another SHA-256 than the line of its number in the file
// `expected`, as wrong: at its first read when `expected` begins with
// "first" and the file `read` does not have the block's number yet, which
// the reader then adds, and at a later read otherwise. A block that
// `expected` does not name is not counted, and neither is the part of a
// block at the end of a pass that was cut off. shown(counts) is the field of
// the watch's line. The text has no backslash and no quote but the double
// one.
const PYTHON_BLOCKS = `
import hashlib, os
class Blocks:
    def __init__(self, counts, expected, read):
        self.counts = counts
        self.read = read
        try:
            with open(expected) as f:
                mode, *self.sums = f.read().split()
        except (OSError, ValueError):
            mode, self.sums = "all", []
        self.first = mode == "first"
        self.begin()
    def begin(self):
        self.index = 0
        self.size = 0
        self.hash = hashlib.sha256()
    def add(self, data):
        view = memoryview(data)
        while len(view):
            take = min(len(view), 1048576 - self.size)
            self.hash.update(view[:take])
            self.size += take
            view = view[take:]
            if self.size == 1048576:
                if self.index < len(self.sums):
                    self.count(self.index, self.hash.hexdigest() != self.sums[self.index])
                self.index += 1
                self.size = 0
                self.hash = hashlib.sha256()
    def count(self, index, wrong):
        first = False
        if self.first:
            try:
                with open(self.read) as f:
                    first = str(index) not in f.read().split()
            except OSError:
                first = True
            if first:
                with open(self.read, "a") as f:
                    f.write("%d " % index)
        checked, at_first, later = read_counts(self.counts)
        with open(self.counts + ".new", "w") as f:
            f.write("%d %d %d" % (checked + 1, at_first + (wrong and first), later + (wrong and not first)))
        os.replace(self.counts + ".new", self.counts)
def read_counts(counts):
    try:
        with open(counts) as f:
            checked, at_first, later = [int(n) for n in f.read().split()]
        return checked, at_first, later
    except (OSError, ValueError):
        return 0, 0, 0
def shown(counts):
    return "blocks=%d,%d,%d" % read_counts(counts)
`;
// Python: checks the bytes on its input as one pass, and fails without a byte.
export const PYTHON_CHECK_INPUT = PYTHON_BLOCKS + `
import sys
blocks = Blocks(sys.argv[1], sys.argv[2], sys.argv[3])
total = 0
while True:
    data = sys.stdin.buffer.read(65536)
    if not data:
        break
    total += len(data)
    blocks.add(data)
sys.exit(0 if total else 1)
`;

// Starts the shell script in argv[1] in its own session and prints its PID,
// which is also its process group ID: stopping the group also stops the
// running `dd`.
const LAUNCH_IN_NEW_SESSION =
  'import subprocess, sys; ' +
  'print(subprocess.Popen(["sh", "-c", sys.argv[1]], start_new_session=True, ' +
  'stdin=subprocess.DEVNULL, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL).pid)';
// The terminal's printer on Linux and macOS: one Perl process that writes,
// again and again, a line with "t" and the host's clock in ms and then the
// next numbers, one per line, until the write has 8192 bytes or a few more.
// The stamp is taken when the write is put together, so a write that waits
// for the terminal is that much older when it arrives.
const PERL_COUNT =
  'perl -MTime::HiRes=time -e \'for($i=1;$i<2000000000;){$b=sprintf("t%d\\n",time*1000);' +
  'while(length($b)<8192){$b.=$i++."\\n"}syswrite(STDOUT,$b)}\'\r';
// The answer of the light terminal on Linux and macOS: one Perl process that
// prints "ready", and then each line that it reads with its first "e" made
// an "r", each line at once ($|). The terminal's line discipline gives Perl
// a typed line when its Enter comes.
const PERL_ANSWER = 'perl -pe \'BEGIN{$|=1;print "ready\\n"}s/^e/r/\'\r';
// Where the tool keeps its files on the host.
export const SENDER_DIRECTORY = 'dir="$HOME/.local/share/jetkvm-combined-load"; mkdir -p "$dir"; ';

// A Chromium browser that shows the page at url in a window without the
// browser's bars (--app), large enough for the page's moving part of 960
// by 540 pixels inside the frame that a desktop gives a window, with no questions on a first start, sound without a
// click, and no question for the desktop's password store.
// The page plays its sound only on the Mini's audio output, which it finds
// by name: a browser gives the names of the audio outputs to a page that
// may use the microphone (--use-fake-ui-for-media-stream allows that
// without a question; the page opens no microphone), and gives the outputs
// only to a secure page, which a page from the tool's HTTP server is by
// the last option. --test-type keeps the browser from showing a warning
// about that option over the page.
const pageBrowserOptions = url => [`--app=${url}`, '--window-size=1000,620', '--window-position=300,150', '--no-first-run', '--no-default-browser-check',
  '--autoplay-policy=no-user-gesture-required', '--password-store=basic', '--disable-features=Translate',
  '--use-fake-ui-for-media-stream', '--test-type', `--unsafely-treat-insecure-origin-as-secure=${new URL(url).origin}`];

// The keys that hid and slowKeys press, in this order: a to z, then 0 to 9.
// Each key comes again only after the 35 others, so the host's press of a
// key is the press of its newest send. The target page on the host's screen
// has no key handlers, so the keys type nothing there. usage: the key's HID
// usage, which the page sends and the macOS watch reads; linux: its evdev
// code; windows: its virtual-key code.
const LINUX_LETTERS = [30, 48, 46, 32, 18, 33, 34, 35, 23, 36, 37, 38, 50, 49, 24, 25, 16, 19, 31, 20, 22, 47, 17, 45, 21, 44];
export const HID_KEYS = [
  ...[...'abcdefghijklmnopqrstuvwxyz'].map((name, i) => ({ name, usage: 0x04 + i, linux: LINUX_LETTERS[i], windows: 0x41 + i })),
  // The usages and the evdev codes of the digits begin at 1 and end at 0.
  ...[...'0123456789'].map(name => Number(name)).map(digit => ({
    name: String(digit), usage: digit === 0 ? 0x27 : 0x1d + digit, linux: digit === 0 ? 11 : 1 + digit, windows: 0x30 + digit,
  })),
];
// For a watch: a Python dict from the host's code of each key to its usage.
const usageByCode = code => `{${HID_KEYS.map(key => `${key[code]}: ${key.usage}`).join(', ')}}`;

// --- Linux ---
//
// The host has python3, GNU dd, curl and sudo without a password.

// The Mini's keyboard and mice as evdev devices, with the keys of HID_KEYS
// by their Linux key codes. The devices leave and return when the Mini
// re-enumerates, so they are opened again. A read takes every event that a
// device has, up to 64, so the watch does not fall behind a mouse that
// moves often. The kernel gives each event the time at which it got it, on
// the calendar clock. The script ends within a second of its SSH session
// closing, when a line cannot be written.
const LINUX_KEY_WATCHER = `
import select, struct, time
keys = ${usageByCode('linux')}
base = "/dev/input/by-id/usb-JetKVM_USB_Emulation_Device-"
names = ["event-kbd", "if01-event-mouse", "if02-event-mouse"]
devices = {}
beat = time.time()
while True:
    if time.time() - beat >= 1:
        beat = time.time()
        print(".", flush=True)
    for name in names:
        if name not in devices:
            try:
                devices[name] = open(base + name, "rb", buffering=0)
            except OSError:
                pass
    if not devices:
        time.sleep(0.2)
        continue
    for name, device in list(devices.items()):
        try:
            if not select.select([device], [], [], 0)[0]:
                continue
            events = device.read(24 * 64)
        except OSError:
            events = b""
        if len(events) < 24:
            device.close()
            del devices[name]
            continue
        lines = []
        for seconds, micros, kind, code, value in struct.iter_unpack("llHHi", events[:len(events) // 24 * 24]):
            if kind == 1 and code in keys and value == 1:
                lines.append("p%d" % keys[code])
            if kind == 3 and code == 0:
                lines.append("m%d %.3f" % (value, seconds * 1000 + micros / 1000))
        if lines:
            print("\\n".join(lines), flush=True)
    select.select(list(devices.values()), [], [], 0.2)
`;

// The sender is a systemd user unit. reset-failed clears the start rate
// limit that repeated starts reach.
export const LINUX_SENDER_UNIT = 'ipkvm-os-services.service';
// The systemd user manager of the SSH login's user, and the display of that
// user's desktop session, which the sender's own start reads.
export const LINUX_USER_SESSION =
  'export XDG_RUNTIME_DIR="${XDG_RUNTIME_DIR:-/run/user/$(id -u)}"; ' +
  'export DBUS_SESSION_BUS_ADDRESS="${DBUS_SESSION_BUS_ADDRESS:-unix:path=$XDG_RUNTIME_DIR/bus}"; ' +
  "eval \"$(systemctl --user show-environment | grep -E '^(WAYLAND_DISPLAY|DISPLAY|XAUTHORITY|XDG_SESSION_TYPE)=' | sed 's/^/export /')\"; ";

// The sender is an AppImage: each start mounts it at /tmp/.mount_IP-KVM<six
// letters> through FUSE, and a process of the AppImage serves that mount and
// unmounts it when the sender ends. The sender's unit has KillMode=mixed, so
// a stop of the unit kills that process before it unmounts, and each stop
// leaves a mount that nothing serves. A user has 1000 FUSE mounts at most;
// then the sender no longer starts. This unmounts each of the sender's
// mounts whose AppImage has no process (a process has the first 15
// characters of the AppImage's file name as its name). It takes the mounts
// of the SSH user only, which are the ones that this user can unmount and
// that count against this user's limit. A mount that stays is no error: the
// command goes on, also inside a command that ends at a failure (set -e).
const LINUX_REMOVE_SENDER_MOUNTS =
  'findmnt -rno SOURCE,TARGET,OPTIONS 2>/dev/null | while read -r source target options; do ' +
  'case "$target" in /tmp/.mount_IP-KVM*) ;; *) continue;; esac; ' +
  'case ",$options," in *",user_id=$(id -u),"*) ;; *) continue;; esac; ' +
  'pgrep -x "$(printf %.15s "$source")" >/dev/null && continue; ' +
  'unmount=$(command -v fusermount3 || command -v fusermount) || continue; ' +
  '"$unmount" -u "$target" 2>/dev/null || "$unmount" -uz "$target" 2>/dev/null || true; ' +
  'rmdir "$target" 2>/dev/null || true; ' +
  'done || true; ';

// Prints the ID of the Mini's audio output among PipeWire's objects
// (`pw-dump`), or nothing without one.
const LINUX_MINI_SINK = `
import json, re, sys
for thing in json.load(sys.stdin):
    props = (thing.get("info") or {}).get("props") or {}
    name = "%s %s" % (props.get("node.name"), props.get("node.description"))
    if props.get("media.class") == "Audio/Sink" and re.search("JetKVM|USB Emulation Device", name):
        print(thing["id"])
        break
`;

// The page's browser is a unit of the systemd user manager, which starts it
// with the desktop session's display. Stopping the unit ends every process
// of the browser.
const LINUX_PAGE_UNIT = 'jetkvm-combined-load-page';
const LINUX_CLOSE_PAGE =
  `systemctl --user stop ${LINUX_PAGE_UNIT} 2>/dev/null; systemctl --user reset-failed ${LINUX_PAGE_UNIT} 2>/dev/null; `;

// The host's block devices of the Mini: its CD-ROM drive and, while an image
// is mounted as a disk, that disk. One line per device: type, name.
const LINUX_FIND_MEDIA =
  "lsblk -Snpo NAME,TRAN,TYPE,VENDOR 2>/dev/null | awk '$2==\"usb\" && ($3==\"rom\" || $3==\"disk\") && $4==\"JetKVM\" {print $3 \":\" $1}'";

// Mutter's display configuration over D-Bus, which GNOME's display settings
// use, on X11 and on Wayland. Method 1 of ApplyMonitorsConfig is
// "temporary": Mutter does not store the mode, so no stored configuration
// comes back at a later plug of the monitor, and the mode is gone with the
// session. The host has one display in use, at a scale of 100 %, as a
// bench host has: with more displays a new size needs a new layout of them,
// and another scale is not valid for every mode. A host that has it
// otherwise is refused, also when only the mode is read, which is before
// the list.
const LINUX_DISPLAY_MODE = `
import sys
from gi.repository import Gio, GLib
bus = Gio.bus_get_sync(Gio.BusType.SESSION, None)
call = lambda m, a: bus.call_sync("org.gnome.Mutter.DisplayConfig", "/org/gnome/Mutter/DisplayConfig",
    "org.gnome.Mutter.DisplayConfig", m, a, None, Gio.DBusCallFlags.NONE, -1, None)
serial, monitors, logicals, _ = call("GetCurrentState", None).unpack()
in_use = [(connector[0], logical[2]) for logical in logicals for connector in logical[5]]
if len(in_use) != 1 or in_use[0][1] != 1.0:
    sys.exit("hostModeChange needs a host with one display in use, at a scale of 100 %; this host has " +
             ", ".join("%s at %d %%" % (name, round(scale * 100)) for name, scale in in_use))
x, y, scale, transform, primary, assigned, _l = logicals[0]
connector = assigned[0][0]
modes = next(m for m in monitors if m[0][0] == connector)[1]
current = next(m for m in modes if m[6].get("is-current"))
print(current[1], current[2], current[3])
wanted = tuple(int(a) for a in sys.argv[1:3])
if wanted:
    rate = float(sys.argv[3]) if len(sys.argv) > 3 else current[3]
    fit = sorted((m for m in modes if (m[1], m[2]) == wanted), key=lambda m: abs(m[3] - rate))
    if not fit:
        sys.exit("%s offers no %dx%d" % ((connector,) + wanted))
    call("ApplyMonitorsConfig", GLib.Variant("(uua(iiduba(ssa{sv}))a{sv})",
        (serial, 1, [(x, y, scale, transform, primary, [(connector, fit[0][0], {})])], {})))
`;

const sizeArguments = size => (size ? ` ${Number(size.width)} ${Number(size.height)}` + (size.hz ? ` ${Number(size.hz)}` : '') : '');

const linux = {
  displayMode: size => LINUX_USER_SESSION + `python3 -c ${quote(LINUX_DISPLAY_MODE)}${sizeArguments(size)}`,

  watchKeys: `sudo -n python3 -u -c ${quote(LINUX_KEY_WATCHER)}`,

  // The type, the device and the /sys/block statistics of the Mini's device
  // that has a medium. The CD-ROM drive stays on the host when the Mini has
  // nothing mounted, so the medium is what shows a mount. Opening the device
  // asks it for its medium; udev's ID_CDROM_MEDIA can keep a disc that is
  // gone for minutes.
  watchMedia:
    `while :; do found=; for e in $(${LINUX_FIND_MEDIA}); do d=\${e#*:}; ` +
    's=$(sudo -n blockdev --getsize64 "$d" 2>/dev/null) || continue; [ "${s:-0}" -gt 0 ] || continue; ' +
    `echo "\${e%%:*} $d $(cat /sys/block/\${d##*/}/stat) errors=$(cat /sys/block/\${d##*/}/device/ioerr_cnt 2>/dev/null) ` +
    `blocks=$(tr ' ' , < ${BLOCKS_FILE} 2>/dev/null)"; found=1; done; ` +
    '[ -n "$found" ] || echo none; sleep 1; done',
  // /sys/block/<device>/stat: the third number is the count of 512-byte sectors read.
  // errors=: the kernel's count of the commands that the device answered
  // with an error (ioerr_cnt, a hexadecimal number), which a read that the
  // Mini refuses is.
  medium: fields => {
    const errors = Number(fields.find(field => field.startsWith('errors='))?.slice('errors='.length) || NaN);
    return { type: fields[0], device: fields[1], bytesRead: Number(fields[4]) * 512,
      ...(Number.isNaN(errors) ? {} : { readErrors: errors }), ...blocksOf(fields) };
  },

  expectBlocks,
  // A device without a medium fails at once. Each pass goes through the
  // checker of its blocks.
  startReader: `python3 -c ${quote(LAUNCH_IN_NEW_SESSION)} ${quote(
    'dd=$(command -v gnudd || echo dd); end=$(($(date +%s) + 3600)); ' +
    'while [ "$(date +%s)" -lt "$end" ]; do read=; ' +
    `for e in $(${LINUX_FIND_MEDIA}); do ` +
    'sudo -n "$dd" if="${e#*:}" bs=64k iflag=direct 2>/dev/null | ' +
    `python3 -c ${quote(PYTHON_CHECK_INPUT)} ${BLOCKS_FILE} ${EXPECTED_FILE} ${READ_BLOCKS_FILE} && read=1; done; ` +
    '[ -n "$read" ] || sleep 1; done')} > ${READER_FILE}`,
  stopReader:
    `f=${READER_FILE}; [ -f "$f" ] || exit 0; g=$(cat "$f"); rm -f "$f"; ` +
    'case "$g" in ""|*[!0-9]*) exit 0;; esac; ' +
    // The external kill: the built-in kill of dash, Debian's sh, reads "--" as a
    // process group and fails.
    'env kill -TERM -- -"$g" 2>/dev/null; ' +
    'for i in $(seq 1 50); do pgrep -g "$g" >/dev/null || exit 0; sleep 0.2; done; ' +
    'sudo -n kill -KILL -- -"$g" 2>/dev/null; true',

  senderPackage: 'linux-x86_64-appimage',
  senderSuffix: '.AppImage',
  // Downloads the AppImage unless the file with this SHA-256 is already
  // there, approves the Mini's key, installs the USB access rule and the
  // capture helper, and registers the unit for this file. The sender's own
  // --install-service starts it, so the last step stops it again.
  installSender: ({ url, sha256, file, fingerprint }) =>
    'set -e; ' + SENDER_DIRECTORY +
    `f="$dir/${file}"; ` +
    `if ! echo "${sha256}  $f" | sha256sum -c --status 2>/dev/null; then ` +
    `curl -fsSL --retry 2 -o "$f.part" ${quote(url)}; echo "${sha256}  $f.part" | sha256sum -c --status; ` +
    'chmod 755 "$f.part"; mv "$f.part" "$f"; fi; ' +
    LINUX_USER_SESSION +
    // Each of the next three starts the AppImage, which needs a free mount.
    LINUX_REMOVE_SENDER_MOUNTS +
    `"$f" --approve-kvm ${quote(fingerprint)}; ` +
    'sudo -n "$f" --install-linux-access >/dev/null; ' +
    '"$f" --install-service; ' +
    `systemctl --user stop ${LINUX_SENDER_UNIT}; ` + LINUX_REMOVE_SENDER_MOUNTS + 'true',
  // A start first removes the mounts that an earlier stop left, by this
  // tool or by anything else that stopped the unit.
  startSender: LINUX_USER_SESSION + LINUX_REMOVE_SENDER_MOUNTS +
    `systemctl --user reset-failed ${LINUX_SENDER_UNIT} 2>/dev/null; systemctl --user start ${LINUX_SENDER_UNIT}`,
  // A host without the unit has no sender to stop. The stop leaves the
  // sender's mount, which goes with it.
  stopSender: LINUX_USER_SESSION + `systemctl --user stop ${LINUX_SENDER_UNIT} 2>/dev/null; ` +
    LINUX_REMOVE_SENDER_MOUNTS + 'true',

  checksumsAndRemove: paths => `sha256sum ${paths.map(quote).join(' ')}; rm -f ${paths.map(quote).join(' ')}`,
  countInTerminal: PERL_COUNT,
  answerInTerminal: PERL_ANSWER,

  // PipeWire gives an output that it sees for the first time a volume of
  // 40 %, which is 6 % of the level, and keeps an output's volume from then
  // on, also when the output leaves and comes back. A host without the
  // Mini's output has nothing to set; the target page then reports that.
  fullVolume: LINUX_USER_SESSION +
    `id=$(pw-dump | python3 -c ${quote(LINUX_MINI_SINK)}); [ -z "$id" ] || wpctl set-volume "$id" 1.0`,

  // GNOME: the screen saver is asked to show the desktop again, and
  // gnome-session-inhibit keeps the session from going idle, which is what
  // blanks and locks the screen, for as long as the browser runs. The
  // browser has a profile of its own, new for each start, so it is not a
  // window of the user's browser and restores nothing.
  openPage: url => LINUX_USER_SESSION + SENDER_DIRECTORY + LINUX_CLOSE_PAGE +
    'b=$(command -v google-chrome || command -v chromium || command -v chromium-browser) || ' +
    '{ echo "the host has no Google Chrome or Chromium to show the target page" >&2; exit 1; }; ' +
    'rm -rf "$dir/page-profile"; ' +
    'gdbus call --session --dest org.gnome.ScreenSaver --object-path /org/gnome/ScreenSaver ' +
    '--method org.gnome.ScreenSaver.SetActive false >/dev/null 2>&1; ' +
    `systemd-run --user --quiet --collect --unit=${LINUX_PAGE_UNIT} ` +
    'gnome-session-inhibit --inhibit idle --reason "JetKVM combined load" ' +
    `"$b" ${pageBrowserOptions(url).map(quote).join(' ')} --user-data-dir="$dir/page-profile"`,
  closePage: LINUX_USER_SESSION + LINUX_CLOSE_PAGE + 'true',
};

// --- macOS ---
//
// The host has python3 and swift (the Xcode command line tools) and curl.
// The SSH login's user is signed in at the screen and is in the group
// `operator`, which can read the raw disk devices. SSH sessions have the
// Input Monitoring permission, for the keys, and the Accessibility
// permission, for the dialog of an unreadable disk.

// The Mini's keyboard and mice through the HID manager, which also takes a
// device that returns after the Mini re-enumerated. 7 is the usage page of
// keyboards, where the keys of HID_KEYS have their usages; 1 is the generic
// desktop page and 0x30 its x axis, which the Mini's absolute mouse reports
// as the place and its relative mouse as a distance. A line that cannot be
// written, after the SSH session closed, ends the script (SIGPIPE). The HID
// manager gives each value the time at which the Mac got it, on the clock of
// the Mac's uptime; stamp() is such a time in ms.
const MACOS_KEY_WATCHER = `
import Foundation
import IOKit.hid
setvbuf(stdout, nil, _IOLBF, 0)
var timebase = mach_timebase_info_data_t()
mach_timebase_info(&timebase)
func stamp(_ uptime: UInt64) -> String {
    String(format: "%.3f", Double(uptime) * Double(timebase.numer) / Double(timebase.denom) / 1e6)
}
let manager = IOHIDManagerCreate(kCFAllocatorDefault, IOOptionBits(kIOHIDOptionsTypeNone))
IOHIDManagerSetDeviceMatching(manager, [kIOHIDManufacturerKey: "JetKVM", kIOHIDProductKey: "USB Emulation Device"] as CFDictionary)
IOHIDManagerRegisterInputValueCallback(manager, { _, _, _, value in
    let element = IOHIDValueGetElement(value)
    let page = IOHIDElementGetUsagePage(element)
    let usage = IOHIDElementGetUsage(element)
    let now = IOHIDValueGetIntegerValue(value)
    if page == 7 && [${HID_KEYS.map(key => key.usage).join(', ')}].contains(usage) && now == 1 { print("p\\(usage)") }
    if page == 1 && usage == 0x30 && !IOHIDElementIsRelative(element) { print("m\\(now) \\(stamp(IOHIDValueGetTimeStamp(value)))") }
}, nil)
IOHIDManagerScheduleWithRunLoop(manager, CFRunLoopGetMain(), CFRunLoopMode.defaultMode.rawValue)
guard IOHIDManagerOpen(manager, IOOptionBits(kIOHIDOptionsTypeNone)) == kIOReturnSuccess else {
    FileHandle.standardError.write("no access to the keyboard: the SSH session needs Input Monitoring\\n".data(using: .utf8)!)
    exit(1)
}
Timer.scheduledTimer(withTimeInterval: 1, repeats: true) { _ in print(".") }
RunLoop.main.run()
`;

// The Mini's media in the I/O Registry: under each block storage driver, the
// medium whose name starts with the Mini's vendor name. A CD is read through
// its data track, which is the first medium below the disc.
//
// The registry keeps a medium that the Mini has unmounted: a disc until
// something reads it, a disk until the Mini enumerates again. Only a read
// tells whether the medium is there. The read of a disc that is gone fails
// at once. For a disk that is gone, the Mini answers the read with "not
// ready, medium not present", and macOS tries it 21 times in 171 seconds
// before the read fails. The reading process waits in the kernel for that
// time and cannot be killed.
//
// So the watch takes a medium as there when the driver's count of bytes
// read has grown since the round before, which is the reader's work, or
// when a read of one block returns. It reads only when the count has not
// grown. Each read is a dd process of its own, of which at most one runs
// per medium, and the watch does not wait for it. The read of a medium
// takes about 10 ms, and up to 3 seconds beside an OS Services session; a
// medium that has been there stays one until its read has not returned for
// READ_SECONDS. The count that the watch prints is the driver's less the
// watch's own blocks, so it grows only with the reader's work.
//
// argv[1]: "watch" prints twice per second, for each medium, the type, the
// device and that count, or "none". "read" is the reader: it reads each
// medium from its start to its end, again and again, for an hour, with one
// dd process per medium, so it does not wait at a disk without a medium
// either. "disks" prints the whole disk of each disk in the registry,
// without a read.
const MACOS_MEDIA = PYTHON_BLOCKS + `
import plistlib, subprocess, sys, threading, time
READ_SECONDS = 5
COUNTS = "${BLOCKS_FILE}"
def check(pipe):
    blocks = Blocks(COUNTS, "${EXPECTED_FILE}", "${READ_BLOCKS_FILE}")
    while True:
        data = pipe.read(65536)
        if not data:
            break
        blocks.add(data)
def below(node):
    for child in node.get("IORegistryEntryChildren", []):
        yield child
        yield from below(child)
def media():
    out = subprocess.run(["ioreg", "-a", "-r", "-l", "-c", "IOBlockStorageDriver"], capture_output=True).stdout
    for driver in plistlib.loads(out) if out else []:
        for whole in driver.get("IORegistryEntryChildren", []):
            if not whole.get("IORegistryEntryName", "").startswith("JetKVM ") or "BSD Name" not in whole:
                continue
            rom = whole.get("IOObjectClass") == "IOCDMedia"
            part = next((m for m in below(whole) if "BSD Name" in m), whole) if rom else whole
            yield ("rom" if rom else "disk", "/dev/r" + part["BSD Name"], part.get("Preferred Block Size", 512),
                   whole.get("IORegistryEntryID"), driver.get("Statistics", {}).get("Bytes (Read)", 0), whole["BSD Name"])
def dd(*arguments, **streams):
    return subprocess.Popen(["dd"] + list(arguments), stdin=subprocess.DEVNULL, stderr=subprocess.DEVNULL, **streams), time.time()
if sys.argv[1] == "disks":
    for kind, _, _, _, _, whole in media():
        if kind == "disk":
            print(whole)
    sys.exit(0)
if sys.argv[1] == "read":
    # A pass that ended in less than a second, as at a medium that is gone,
    # starts again a second after its start.
    passes = {}
    looked = 0
    over = lambda entry: entry not in passes or (passes[entry][0].poll() is not None and time.time() - passes[entry][1] >= 1)
    end = time.time() + 3600
    while time.time() < end:
        if time.time() - looked >= 1 or any(over(entry) for entry in passes):
            looked = time.time()
            for _, device, _, entry, _, _ in media():
                if over(entry):
                    passes[entry] = dd("if=" + device, "bs=64k", stdout=subprocess.PIPE)
                    threading.Thread(target=check, args=(passes[entry][0].stdout,), daemon=True).start()
        time.sleep(0.1)
    sys.exit(0)
reads = {}
def returned(entry):
    return len(reads.pop(entry)[0].stdout.read())
own = {}
late = {}
seen = {}
there = set()
while True:
    start = time.time()
    # The driver's count at this round's look at the registry has the blocks
    # of the reads that have returned by now.
    got = {entry: returned(entry) for entry in [entry for entry in reads if reads[entry][0].poll() is not None]}
    for entry, count in list(late.items()) + list(got.items()):
        own[entry] = own.get(entry, 0) + count
    late = {}
    found = False
    now = list(media())
    for kind, device, block, entry, read, _ in now:
        count = read - own.get(entry, 0)
        # The count can have the block of a read that is under way.
        waits = entry in reads
        grew = count - seen.get(entry, count) > (block if waits else 0)
        if grew or not waits:
            seen[entry] = max(count, seen.get(entry, 0))
        result = got.get(entry)
        if not grew and not waits:
            reads[entry] = dd("if=" + device, "bs=%d" % block, "count=1", stdout=subprocess.PIPE)
            try:
                reads[entry][0].wait(timeout=0.2)
                result = late[entry] = returned(entry)
            except subprocess.TimeoutExpired:
                pass
        if grew or result:
            there.add(entry)
        elif result == 0 or (entry in reads and time.time() - reads[entry][1] >= READ_SECONDS):
            there.discard(entry)
        if entry in there:
            print(kind, device, seen[entry], shown(COUNTS), flush=True)
            found = True
    # The read of a disk that has left the registry ends by itself.
    for entry in set(reads) - {entry for _, _, _, entry, _, _ in now}:
        del reads[entry]
    if not found:
        print("none", flush=True)
    time.sleep(max(0, 0.5 - (time.time() - start)))
`;

// limit <seconds> <command>: runs the command without input or output and
// returns its status, or 1 when it has not ended after that time. A command
// that waits in the kernel cannot be killed; it stays behind, and the SSH
// session does not wait for it.
const MACOS_LIMIT =
  'limit() { s=$1; shift; "$@" </dev/null >/dev/null 2>&1 & c=$!; i=0; ' +
  'while kill -0 $c 2>/dev/null; do ' +
  'if [ $i -ge $((s * 10)) ]; then kill -KILL $c 2>/dev/null; return 1; fi; sleep 0.1; i=$((i + 1)); done; ' +
  'wait $c; }; ';

// The page's browser is a launchd agent of the user's screen session, like
// the sender. Unloading the agent ends the browser.
const MACOS_PAGE_LABEL = 'com.jetkvm.combined-load.page';
const MACOS_CLOSE_PAGE =
  `pt=gui/$(id -u)/${MACOS_PAGE_LABEL}; launchctl bootout $pt 2>/dev/null || true; i=0; ` +
  'while launchctl print $pt >/dev/null 2>&1 && [ $i -lt 100 ]; do sleep 0.1; i=$((i + 1)); done; ';
// The keys of hid and slowKeys go to the app that has the keyboard focus.
// An app that does not take them plays the alert sound for each key, on the
// system output, which is the Mini's: the Mini then sends that sound beside
// the page's tone. The page has no key handlers, so in the page's browser
// the keys type nothing and make no sound. MACOS_PAGE_PID prints the pid
// that launchd reports for the page's agent, which is the browser's main
// process (caffeinate runs the browser in its own process and stays beside
// it as a child), or nothing without the agent. MACOS_PAGE_TO_FRONT($pg)
// makes that process the frontmost app once it has a window, which gives
// it the keyboard focus, and fails when it has none after 15 s.
const MACOS_PAGE_PID =
  `launchctl print gui/$(id -u)/${MACOS_PAGE_LABEL} 2>/dev/null | ` +
  'sed -n "s/^[[:space:]]*pid = \\([0-9][0-9]*\\).*/\\1/p" | head -1';
const MACOS_PAGE_TO_FRONT =
  'osascript -e \'on run argv\' -e \'set pid to (item 1 of argv) as integer\' ' +
  '-e \'tell application "System Events"\' -e \'repeat 150 times\' ' +
  '-e \'set found to every process whose unix id is pid\' ' +
  '-e \'if found is not {} and (count windows of item 1 of found) > 0 then\' ' +
  '-e \'set frontmost of item 1 of found to true\' -e \'if frontmost of item 1 of found then return\' -e \'end if\' ' +
  '-e \'delay 0.1\' -e \'end repeat\' -e \'end tell\' -e \'error "not in front"\' -e \'end run\' "${pg:-0}"';

// macOS shows a dialog for each disk without a file system that appears, and
// keeps it after the disk is gone. This chooses Ignore in one such dialog at a
// time and reads the windows again after each, because a click changes the
// list of windows, until none is left or after 30. A host on which it cannot
// run, or does not answer, keeps the dialogs. Such a dialog takes the
// keyboard focus, and UserNotificationCenter, which shows it, can keep the
// focus after the click while it has another window, such as another app's
// request for a permission; the keys of hid then go there. So after a click,
// counted also when the click reports an error because the dialog went away,
// the page's browser, when it runs, is made the frontmost app again. Its
// window is there, so that takes one try; the limit of 3 s keeps the step
// within the time that the callers give it.
const MACOS_CLOSE_DISK_DIALOGS = MACOS_LIMIT +
  'if limit 15 osascript -e \'set clicked to 0\' ' +
  '-e \'tell application "System Events" to tell process "UserNotificationCenter"\' ' +
  '-e \'repeat 30 times\' -e \'set found to false\' ' +
  '-e \'repeat with w in (every window)\' ' +
  '-e \'if (exists button "Eject" of w) and (exists button "Ignore" of w) then\' ' +
  '-e \'try\' -e \'click button "Ignore" of w\' -e \'end try\' ' +
  '-e \'set clicked to clicked + 1\' -e \'set found to true\' -e \'exit repeat\' -e \'end if\' -e \'end repeat\' ' +
  '-e \'if not found then exit repeat\' -e \'delay 0.3\' -e \'end repeat\' -e \'end tell\' ' +
  '-e \'if clicked is 0 then error "no dialog"\'; then ' +
  `pg=$(${MACOS_PAGE_PID}); [ -z "$pg" ] || limit 3 ${MACOS_PAGE_TO_FRONT}; fi; `;

// The sender is a launchd agent of the user's screen session: the label, and
// the file that the sender's own --install-service writes.
export const MACOS_SENDER =
  't=gui/$(id -u)/com.jetkvm.os-services; p="$HOME/Library/LaunchAgents/com.jetkvm.os-services.plist"; ';
// Unloads the agent and waits until launchd has removed it.
const MACOS_STOP_SENDER = MACOS_SENDER +
  'launchctl bootout $t 2>/dev/null || true; i=0; ' +
  'while launchctl print $t >/dev/null 2>&1 && [ $i -lt 300 ]; do sleep 0.1; i=$((i + 1)); done';

const macos = {
  watchKeys: `swift -e ${quote(MACOS_KEY_WATCHER)}`,

  watchMedia: `python3 -u -c ${quote(MACOS_MEDIA)} watch`,
  medium: fields => ({ type: fields[0], device: fields[1], bytesRead: Number(fields[2]), ...blocksOf(fields) }),
  expectBlocks,
  canRead:
    'id -Gn | tr " " "\\n" | grep -qx operator || ' +
    '{ echo "$(id -un) is not in the group operator, which can read the raw disk devices ' +
    '(sudo dseditgroup -o edit -a $(id -un) -t user operator)" >&2; exit 1; }',

  // A raw device is read past the cache.
  startReader: `python3 -c ${quote(LAUNCH_IN_NEW_SESSION)} ${quote(
    `exec python3 -c ${quote(MACOS_MEDIA)} read`)} > ${READER_FILE}`,
  stopReader:
    MACOS_CLOSE_DISK_DIALOGS +
    `f=${READER_FILE}; [ -f "$f" ] || exit 0; g=$(cat "$f"); rm -f "$f"; ` +
    'case "$g" in ""|*[!0-9]*) exit 0;; esac; ' +
    'env kill -TERM -- -"$g" 2>/dev/null; ' +
    'for i in $(seq 1 50); do pgrep -g "$g" >/dev/null || exit 0; sleep 0.2; done; ' +
    'env kill -KILL -- -"$g" 2>/dev/null; true',
  // macOS keeps a disk that the Mini unmounts, and waits at each read of it
  // (MACOS_MEDIA). So the host ejects the disk first, which also unmounts the
  // image on the Mini and leaves the Mini's other USB devices as they are.
  // An eject takes about 0.3 s. A disk that the Mini has already unmounted
  // does not answer it; the command prints each such disk.
  releaseMedia: MACOS_LIMIT +
    `for d in $(python3 -c ${quote(MACOS_MEDIA)} disks); do limit 5 diskutil eject "$d" || echo "$d"; done`,
  closeDiskDialogs: MACOS_CLOSE_DISK_DIALOGS + 'true',

  senderPackage: 'macos-aarch64',
  senderSuffix: '.app.tar.gz',
  // The package is the app as a tar archive. It is downloaded and unpacked
  // unless the archive with this SHA-256 and its app are already there. The
  // sender's own --install-service writes the agent for this app and starts
  // it, so the last step stops it again. The system permissions of the
  // sender belong to its signature, so they hold for this copy too.
  installSender: ({ url, sha256, file, fingerprint }) =>
    'set -e; ' +
    // The agent runs in the session that has the screen. After a restart of
    // the Mac or of its window server, the screen is at the login window.
    '[ "$(stat -f %Su /dev/console)" = "$(id -un)" ] || ' +
    '{ echo "$(id -un) is not signed in at the screen of this Mac, where the sender runs" >&2; exit 1; }; ' +
    SENDER_DIRECTORY +
    `f="$dir/${file}"; app="$dir/${file.replace(/\.tar\.gz$/, '')}"; ` +
    `if ! echo "${sha256}  $f" | shasum -a 256 -c --status 2>/dev/null || [ ! -d "$app" ]; then ` +
    `curl -fsSL --retry 2 -o "$f.part" ${quote(url)}; echo "${sha256}  $f.part" | shasum -a 256 -c --status; ` +
    'rm -rf "$app.part"; mkdir "$app.part"; tar -xzf "$f.part" -C "$app.part"; ' +
    `${MACOS_STOP_SENDER}; rm -rf "$app"; mv "$app.part" "$app"; mv "$f.part" "$f"; fi; ` +
    'exe=$(echo "$app"/*.app/Contents/MacOS/ipkvm-os-services); ' +
    `"$exe" --approve-kvm ${quote(fingerprint)}; ` +
    '"$exe" --install-service; ' +
    MACOS_STOP_SENDER,
  // Loads the agent, or starts it again when it is loaded and has exited.
  startSender: MACOS_SENDER +
    'if launchctl print $t >/dev/null 2>&1; then launchctl kickstart $t; else launchctl bootstrap gui/$(id -u) "$p"; fi',
  stopSender: MACOS_STOP_SENDER,

  checksumsAndRemove: paths => `shasum -a 256 ${paths.map(quote).join(' ')}; rm -f ${paths.map(quote).join(' ')}`,
  // The seq of macOS writes each line to a terminal by itself, where GNU seq
  // writes blocks of 8192 bytes. The sender passes each read of the terminal
  // on to the Mini as one transfer, so line by line the Mini gets several
  // thousand small transfers per second. Perl, which macOS has, writes the
  // same numbers in blocks of 8192 bytes, as on Linux. It is one process:
  // after a terminal that ran seq through dd or cat is closed, the next
  // shell on that Mac takes 10 s to start.
  countInTerminal: PERL_COUNT,
  answerInTerminal: PERL_ANSWER,

  // The agent is caffeinate with the browser as its program: caffeinate
  // turns the display on (-u) and keeps the display and the Mac awake
  // (-d, -i) for as long as the browser runs. The browser has a profile of
  // its own, new for each start, so it is not a window of the user's
  // browser and restores nothing.
  openPage: url => SENDER_DIRECTORY + MACOS_CLOSE_PAGE +
    'b="/Applications/Google Chrome.app/Contents/MacOS/Google Chrome"; ' +
    '[ -x "$b" ] || { echo "the host has no Google Chrome to show the target page" >&2; exit 1; }; ' +
    'rm -rf "$dir/page-profile"; ' +
    'cat > "$dir/page.plist" <<JETKVM_END\n' +
    '<?xml version="1.0" encoding="UTF-8"?>\n<plist version="1.0"><dict>\n' +
    `<key>Label</key><string>${MACOS_PAGE_LABEL}</string>\n<key>RunAtLoad</key><true/>\n` +
    '<key>ProgramArguments</key><array>\n' +
    ['/usr/bin/caffeinate', '-diu', '$b', ...pageBrowserOptions(url), '--user-data-dir=$dir/page-profile']
      .map(argument => `<string>${argument.replace(/&/g, '&amp;').replace(/</g, '&lt;')}</string>\n`).join('') +
    '</array>\n</dict></plist>\nJETKVM_END\n' +
    'launchctl bootstrap gui/$(id -u) "$dir/page.plist"; ' +
    MACOS_LIMIT + `pg=; i=0; while [ -z "$pg" ] && [ $i -lt 150 ]; do pg=$(${MACOS_PAGE_PID}); [ -n "$pg" ] || sleep 0.1; i=$((i + 1)); done; ` +
    `limit 20 ${MACOS_PAGE_TO_FRONT} || ` +
    '{ echo "the browser of the target page did not come to the front, where the keys of hid go" >&2; exit 1; }',
  closePage: MACOS_CLOSE_PAGE + 'true',
};

// --- Windows ---
//
// The host has Git for Windows, whose sh is the shell of the SSH login, and
// Python as `python`. The SSH login's user is an administrator, which can
// read the raw devices, and is signed in at the screen.

// Git's sh rewrites an argument that starts with "/" to a Windows path, which
// breaks the options of the Windows programs. With that off, a program that
// is not Git's own gets its paths from cygpath. On its way into that sh, a
// command loses one of each two backslashes in a row, so the commands and
// scripts here have no two in a row: a device is //./D:, which Windows
// takes like the form with backslashes.
export const WINDOWS_SHELL = "export MSYS_NO_PATHCONV=1 MSYS2_ARG_CONV_EXCL='*'; ";

// Windows gives the keys and moves of a keyboard or mouse (raw input) only
// to a process of the screen session, which an SSH session is not. So the
// script has two roles. As "relay", in the SSH session, it starts itself as
// "watch" in the screen session, through a scheduled task that runs only
// there, and prints the lines that the watch sends to its TCP port. As
// "watch" it reports the devices with the Mini's USB IDs, with the keys of
// HID_KEYS by their virtual-key codes, and Windows gives the place of an
// absolute mouse in 65536 steps where the Mini sends 32768. The watch ends
// when the relay is gone, which is at most a second after the SSH session
// closed. Windows gives a move no time, so its time is the time at which
// the watch took it from its message queue: stamp() is the performance
// counter in ms, because the calendar clock of Python has steps of up to
// 16 ms there. Each line goes to the relay at once (TCP_NODELAY).
const WINDOWS_KEY_WATCHER = String.raw`
import ctypes, ctypes.wintypes as w, os, socket, struct, subprocess, sys, time
keys = ${usageByCode('windows')}
if sys.argv[1] == "relay":
    sys.stdout.reconfigure(newline="\n")
    server = socket.socket()
    server.bind(("127.0.0.1", 0))
    server.listen(1)
    server.settimeout(20)
    task = "jetkvm-combined-load-keys"
    watch = '"%s" "%s" watch %d' % (os.path.join(os.path.dirname(sys.executable), "pythonw.exe"),
                                    os.path.abspath(__file__), server.getsockname()[1])
    subprocess.run(["schtasks", "/create", "/f", "/tn", task, "/tr", watch, "/sc", "once", "/st", "00:00", "/it"],
                   check=True, capture_output=True)
    try:
        subprocess.run(["schtasks", "/run", "/tn", task], check=True, capture_output=True)
        for line in server.accept()[0].makefile():
            print(line.strip(), flush=True)
    finally:
        subprocess.run(["schtasks", "/delete", "/f", "/tn", task], capture_output=True)
    sys.exit(0)

relay = socket.create_connection(("127.0.0.1", int(sys.argv[2])))
relay.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
def stamp():
    return "%.3f" % (time.perf_counter() * 1000)
user32 = ctypes.WinDLL("user32")
kernel32 = ctypes.WinDLL("kernel32")
# A scheduled task runs below the normal priority, and the watch then waits
# for seconds while other programs keep the processors busy. 0x80 is the
# high priority class.
kernel32.GetCurrentProcess.restype = w.HANDLE
kernel32.SetPriorityClass.argtypes = [w.HANDLE, w.DWORD]
kernel32.SetPriorityClass(kernel32.GetCurrentProcess(), 0x80)
WNDPROC = ctypes.WINFUNCTYPE(ctypes.c_ssize_t, w.HWND, w.UINT, w.WPARAM, w.LPARAM)
user32.DefWindowProcW.restype = ctypes.c_ssize_t
user32.DefWindowProcW.argtypes = [w.HWND, w.UINT, w.WPARAM, w.LPARAM]
user32.CreateWindowExW.restype = w.HWND
user32.CreateWindowExW.argtypes = [w.DWORD, w.LPCWSTR, w.LPCWSTR, w.DWORD, ctypes.c_int, ctypes.c_int, ctypes.c_int,
                                   ctypes.c_int, w.HWND, w.HMENU, w.HINSTANCE, w.LPVOID]
user32.GetRawInputData.argtypes = [w.LPARAM, w.UINT, ctypes.c_void_p, ctypes.POINTER(w.UINT), w.UINT]
user32.GetRawInputDeviceInfoW.argtypes = [w.HANDLE, w.UINT, ctypes.c_void_p, ctypes.POINTER(w.UINT)]
user32.SetTimer.argtypes = [w.HWND, ctypes.c_size_t, w.UINT, ctypes.c_void_p]
kernel32.GetModuleHandleW.restype = w.HMODULE
kernel32.GetModuleHandleW.argtypes = [w.LPCWSTR]
class WNDCLASSW(ctypes.Structure):
    _fields_ = [("style", w.UINT), ("lpfnWndProc", WNDPROC), ("cbClsExtra", ctypes.c_int), ("cbWndExtra", ctypes.c_int),
                ("hInstance", w.HINSTANCE), ("hIcon", w.HANDLE), ("hCursor", w.HANDLE), ("hbrBackground", w.HANDLE),
                ("lpszMenuName", w.LPCWSTR), ("lpszClassName", w.LPCWSTR)]
class RAWINPUTDEVICE(ctypes.Structure):
    _fields_ = [("usUsagePage", w.USHORT), ("usUsage", w.USHORT), ("dwFlags", w.DWORD), ("hwndTarget", w.HWND)]
# RAWINPUTHEADER: the type, the size, the device and a parameter.
HEADER = 8 + 2 * ctypes.sizeof(ctypes.c_void_p)
def say(line):
    try:
        relay.sendall((line + "\n").encode())
    except OSError:
        os._exit(0)
minis = {}
def mini(device):
    if device not in minis:
        size = w.UINT(0)
        user32.GetRawInputDeviceInfoW(device, 0x20000007, None, ctypes.byref(size))
        name = ctypes.create_unicode_buffer(size.value + 1)
        user32.GetRawInputDeviceInfoW(device, 0x20000007, name, ctypes.byref(size))
        minis[device] = "VID_399B&PID_0001" in name.value.upper()
    return minis[device]
def on_message(window, message, wparam, lparam):
    if message == 0x0113:
        say(".")
    if message == 0x00FF:
        got = stamp()
        size = w.UINT(0)
        user32.GetRawInputData(lparam, 0x10000003, None, ctypes.byref(size), HEADER)
        data = ctypes.create_string_buffer(size.value)
        user32.GetRawInputData(lparam, 0x10000003, data, ctypes.byref(size), HEADER)
        kind = struct.unpack_from("<I", data, 0)[0]
        device = struct.unpack_from("<Q" if HEADER == 24 else "<I", data, 8)[0]
        if mini(device):
            if kind == 1:
                _, flags, _, key = struct.unpack_from("<HHHH", data, HEADER)
                if key in keys and not flags & 1:
                    say("p%d" % keys[key])
            if kind == 0:
                flags, _, _, x = struct.unpack_from("<HxxIIi", data, HEADER)
                if flags & 1:
                    say("m%d %s" % (round(x * 32767 / 65535), got))
    return user32.DefWindowProcW(window, message, wparam, lparam)
procedure = WNDPROC(on_message)
window_class = WNDCLASSW(lpfnWndProc=procedure, hInstance=kernel32.GetModuleHandleW(None),
                         lpszClassName="jetkvm-combined-load-keys")
user32.RegisterClassW(ctypes.byref(window_class))
window = user32.CreateWindowExW(0, window_class.lpszClassName, None, 0, 0, 0, 0, 0, w.HWND(-3), None,
                                window_class.hInstance, None)
devices = (RAWINPUTDEVICE * 2)(RAWINPUTDEVICE(1, 6, 0x100, window), RAWINPUTDEVICE(1, 2, 0x100, window))
if not user32.RegisterRawInputDevices(devices, 2, ctypes.sizeof(RAWINPUTDEVICE)):
    os._exit(1)
# The first line at once, so that the tool has the watch without a wait.
say(".")
user32.SetTimer(window, 1, 1000, None)
message = w.MSG()
while user32.GetMessageW(ctypes.byref(message), None, 0, 0) > 0:
    user32.DispatchMessageW(ctypes.byref(message))
`;

// The Mini's media: its CD-ROM drive, which is a drive letter, and its disk,
// which is a physical drive, each when its vendor is the Mini's, it has a
// medium and the medium has a size. Windows counts the bytes read of a disk
// but not of a CD-ROM drive, so the reader counts what it reads, in the file
// in argv[2], and continues the count of the reader before. argv[1] "watch"
// prints each medium's type and device and that count once per second.
// "read" is the reader.
const WINDOWS_MEDIA = PYTHON_BLOCKS + String.raw`
import ctypes, ctypes.wintypes as w, struct, sys, time
kernel32 = ctypes.WinDLL("kernel32")
kernel32.CreateFileW.restype = w.HANDLE
kernel32.CreateFileW.argtypes = [w.LPCWSTR, w.DWORD, w.DWORD, ctypes.c_void_p, w.DWORD, w.DWORD, w.HANDLE]
kernel32.DeviceIoControl.argtypes = [w.HANDLE, w.DWORD, ctypes.c_char_p, w.DWORD, ctypes.c_char_p, w.DWORD,
                                     ctypes.POINTER(w.DWORD), ctypes.c_void_p]
kernel32.CloseHandle.argtypes = [w.HANDLE]
kernel32.GetDriveTypeW.argtypes = [w.LPCWSTR]
def ask(device, code, query=b"", size=0):
    answer = ctypes.create_string_buffer(size)
    got = w.DWORD()
    ok = kernel32.DeviceIoControl(device, code, query, len(query), answer, size, ctypes.byref(got), None)
    return answer.raw[:got.value] if ok else None
def media():
    letters = [c for c in "DEFGHIJKLMNOPQRSTUVWXYZ" if kernel32.GetDriveTypeW(c + ":/") == 5]
    paths = [("rom", "//./%s:" % c) for c in letters] + [("disk", "//./PhysicalDrive%d" % n) for n in range(16)]
    for kind, path in paths:
        device = kernel32.CreateFileW(path, 0x80000000, 3, None, 3, 0, None)
        if device in (None, w.HANDLE(-1).value):
            continue
        try:
            # IOCTL_STORAGE_QUERY_PROPERTY, IOCTL_STORAGE_CHECK_VERIFY, IOCTL_DISK_GET_LENGTH_INFO
            about = ask(device, 0x2D1400, bytes(12), 1024)
            vendor = struct.unpack_from("<I", about, 12)[0] if about and len(about) >= 16 else 0
            if not vendor or about[vendor:about.index(b"\0", vendor)].strip() != b"JetKVM":
                continue
            size = ask(device, 0x2D4800) is not None and ask(device, 0x7405C, b"", 8)
            if size and struct.unpack("<q", size)[0] > 0:
                yield kind, path
        finally:
            kernel32.CloseHandle(device)
def count():
    try:
        with open(sys.argv[2]) as f:
            return int(f.read())
    except (OSError, ValueError):
        return None
if sys.argv[1] == "watch":
    last = 0
    while True:
        last = count() or last
        found = [print(kind, path, last, shown(sys.argv[3]), flush=True) for kind, path in media()]
        if not found:
            print("none", flush=True)
        time.sleep(1)
total = count() or 0
blocks = Blocks(sys.argv[3], sys.argv[4], sys.argv[5])
block = bytearray(64 * 1024)
end = time.time() + 3600
with open(sys.argv[2], "w") as counter:
    while time.time() < end:
        before = total
        for kind, path in list(media()):
            try:
                with open(path, "rb", buffering=0) as medium:
                    blocks.begin()
                    while True:
                        got = medium.readinto(block)
                        if not got:
                            break
                        blocks.add(memoryview(block)[:got])
                        total += got
                        counter.seek(0)
                        counter.write("%020d" % total)
                        counter.flush()
            except OSError:
                pass
        if total == before:
            time.sleep(1)
`;
// Starts the reader apart from the SSH session and prints its process ID.
const WINDOWS_LAUNCH =
  'import subprocess, sys; ' +
  'print(subprocess.Popen([sys.executable, "-c", sys.argv[1], "read"] + sys.argv[2:], creationflags=0x208, ' +
  'stdin=subprocess.DEVNULL, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL).pid)';
const WINDOWS_READER_COUNT = '"$(cygpath -m /tmp)/jetkvm-combined-load-reader.bytes"';
// The reader's counts of blocks and what it is to read, as Windows paths.
const WINDOWS_BLOCKS = `"$(cygpath -m ${BLOCKS_FILE})" "$(cygpath -m ${EXPECTED_FILE})" "$(cygpath -m ${READ_BLOCKS_FILE})"`;

// The sender is a scheduled task of the user's screen session, which the
// sender's own --install-service makes: its name ends in the user's ID. The
// installer puts the sender into the user's programs.
export const WINDOWS_SENDER =
  'task="IP-KVM OS Services $("$SYSTEMROOT/System32/whoami.exe" /user /fo csv /nh | tr -d \'"\\r\' | cut -d, -f2)"; ' +
  'exe="$LOCALAPPDATA/IP-KVM OS Services/ipkvm-os-services.exe"; ';
// Asks the sender to stop: its --stop returns when the sender has closed its
// session with the Mini and has exited, or after 10 seconds. Only then are
// the task and the process ended, which does nothing to a sender that has
// stopped. A sender without --stop answers it with an error and keeps
// running, and no sender may be installed at all; ending the task, and the
// process when a task that is gone has left it, stops such a sender without
// the close, and the Mini keeps the session until its liveness limit.
const WINDOWS_STOP_SENDER = WINDOWS_SHELL + WINDOWS_SENDER +
  '"$exe" --stop >/dev/null 2>&1 || true; ' +
  'schtasks /end /tn "$task" >/dev/null 2>&1 || true; ' +
  'taskkill /f /im ipkvm-os-services.exe >/dev/null 2>&1 || true';

// The page's browser needs the screen session, like the watch of the keys.
// So this script has two roles too. As "start", in the SSH session, it makes
// a scheduled task that runs itself as "show" in the screen session. As
// "show" it turns the screen on with a mouse move of no distance, keeps the
// screen and the PC awake for as long as it runs (0x80000003:
// ES_CONTINUOUS, ES_SYSTEM_REQUIRED and ES_DISPLAY_REQUIRED), starts the
// browser with a profile of its own, new for each start, and notes the
// browser's process ID for closePage. Edge is on every Windows.
const WINDOWS_PAGE_TASK = 'jetkvm-combined-load-page';
const windowsPage = url => `
import ctypes, os, shutil, subprocess, sys
task = "${WINDOWS_PAGE_TASK}"
directory = os.path.dirname(os.path.abspath(__file__))
if sys.argv[1] == "start":
    show = '"%s" "%s" show' % (os.path.join(os.path.dirname(sys.executable), "pythonw.exe"), os.path.abspath(__file__))
    subprocess.run(["schtasks", "/create", "/f", "/tn", task, "/tr", show, "/sc", "once", "/st", "00:00", "/it"],
                   check=True, capture_output=True)
    subprocess.run(["schtasks", "/run", "/tn", task], check=True, capture_output=True)
    sys.exit(0)
kernel32 = ctypes.WinDLL("kernel32")
# A scheduled task runs below the normal priority, and a program that it
# starts would as well. 0x20 is the normal priority class.
kernel32.GetCurrentProcess.restype = ctypes.c_void_p
kernel32.SetPriorityClass.argtypes = [ctypes.c_void_p, ctypes.c_uint32]
kernel32.SetPriorityClass(kernel32.GetCurrentProcess(), 0x20)
ctypes.WinDLL("user32").mouse_event(1, 0, 0, 0, 0)
kernel32.SetThreadExecutionState(0x80000003)
programs = [os.path.join(os.environ.get(root, ""), path)
            for path in ("Microsoft/Edge/Application/msedge.exe", "Google/Chrome/Application/chrome.exe")
            for root in ("ProgramFiles(x86)", "ProgramFiles", "LOCALAPPDATA")]
profile = os.path.join(directory, "page-profile")
shutil.rmtree(profile, ignore_errors=True)
browser = subprocess.Popen([next(program for program in programs if os.path.exists(program))] +
                           ${JSON.stringify(pageBrowserOptions(url))} +
                           ["--user-data-dir=" + profile])
with open(os.path.join(directory, "page.pid"), "w") as note:
    note.write(str(browser.pid))
browser.wait()
`;
// Ends the task and the browser that it started. Only a browser with the
// noted process ID is stopped.
const WINDOWS_CLOSE_PAGE = WINDOWS_SHELL + SENDER_DIRECTORY +
  `schtasks /end /tn ${WINDOWS_PAGE_TASK} >/dev/null 2>&1; schtasks /delete /f /tn ${WINDOWS_PAGE_TASK} >/dev/null 2>&1; ` +
  'f="$dir/page.pid"; if [ -f "$f" ]; then g=$(tr -d "[:space:]" < "$f"); rm -f "$f"; ' +
  'case "$g" in ""|*[!0-9]*) ;; *) ' +
  'if tasklist /fi "PID eq $g" /fo csv /nh | grep -qiE \'^"(msedge|chrome)\'; then taskkill /f /t /pid "$g" >/dev/null 2>&1; fi;; ' +
  'esac; fi; ';

// Windows changes a display's mode only for a process of the screen session,
// so the script has the two roles of the key watch: "relay" in the SSH
// session starts "work" in the screen session and prints what it sends.
// The display is the primary one. ChangeDisplaySettingsExW with the flags 0
// (without CDS_UPDATEREGISTRY) changes the mode until the user signs out
// and does not store it. Without a rate only the width and the height are
// set (DM_PELSWIDTH and DM_PELSHEIGHT), so Windows keeps the rate when the
// new size has it; with a rate, that rate as well (DM_DISPLAYFREQUENCY).
const WINDOWS_DISPLAY_MODE = String.raw`
import ctypes, ctypes.wintypes as w, os, socket, subprocess, sys
if sys.argv[1] == "relay":
    sys.stdout.reconfigure(newline="\n")
    server = socket.socket()
    server.bind(("127.0.0.1", 0))
    server.listen(1)
    server.settimeout(20)
    task = "jetkvm-combined-load-display"
    work = '"%s" "%s" work %d %s' % (os.path.join(os.path.dirname(sys.executable), "pythonw.exe"),
                                     os.path.abspath(__file__), server.getsockname()[1], " ".join(sys.argv[2:]))
    subprocess.run(["schtasks", "/create", "/f", "/tn", task, "/tr", work, "/sc", "once", "/st", "00:00", "/it"],
                   check=True, capture_output=True)
    try:
        subprocess.run(["schtasks", "/run", "/tn", task], check=True, capture_output=True)
        lines = [line.strip() for line in server.accept()[0].makefile()]
    finally:
        subprocess.run(["schtasks", "/delete", "/f", "/tn", task], capture_output=True)
    failed = [line for line in lines if not line[:1].isdigit()]
    if failed or not lines:
        sys.exit("; ".join(failed) or "the screen session sent no display mode")
    print("\n".join(lines))
    sys.exit(0)

relay = socket.create_connection(("127.0.0.1", int(sys.argv[2]))).makefile("w")
class DEVMODE(ctypes.Structure):
    _fields_ = [("dmDeviceName", w.WCHAR * 32), ("dmSpecVersion", w.WORD), ("dmDriverVersion", w.WORD),
                ("dmSize", w.WORD), ("dmDriverExtra", w.WORD), ("dmFields", w.DWORD),
                ("dmPositionX", w.LONG), ("dmPositionY", w.LONG), ("dmDisplayOrientation", w.DWORD),
                ("dmDisplayFixedOutput", w.DWORD), ("dmColor", w.SHORT), ("dmDuplex", w.SHORT),
                ("dmYResolution", w.SHORT), ("dmTTOption", w.SHORT), ("dmCollate", w.SHORT),
                ("dmFormName", w.WCHAR * 32), ("dmLogPixels", w.WORD), ("dmBitsPerPel", w.DWORD),
                ("dmPelsWidth", w.DWORD), ("dmPelsHeight", w.DWORD), ("dmDisplayFlags", w.DWORD),
                ("dmDisplayFrequency", w.DWORD), ("dmICMMethod", w.DWORD), ("dmICMIntent", w.DWORD),
                ("dmMediaType", w.DWORD), ("dmDitherType", w.DWORD), ("dmReserved1", w.DWORD),
                ("dmReserved2", w.DWORD), ("dmPanningWidth", w.DWORD), ("dmPanningHeight", w.DWORD)]
try:
    user32 = ctypes.WinDLL("user32")
    mode = DEVMODE(dmSize=ctypes.sizeof(DEVMODE))
    user32.EnumDisplaySettingsW(None, -1, ctypes.byref(mode))
    relay.write("%d %d %d\n" % (mode.dmPelsWidth, mode.dmPelsHeight, mode.dmDisplayFrequency))
    if len(sys.argv) > 3:
        mode = DEVMODE(dmSize=ctypes.sizeof(DEVMODE), dmFields=0x80000 | 0x100000,
                       dmPelsWidth=int(sys.argv[3]), dmPelsHeight=int(sys.argv[4]))
        if len(sys.argv) > 5:
            mode.dmFields |= 0x400000
            mode.dmDisplayFrequency = round(float(sys.argv[5]))
        status = user32.ChangeDisplaySettingsExW(None, ctypes.byref(mode), None, 0, None)
        if status:
            relay.write("the display did not take %sx%s: status %d\n" % (sys.argv[3], sys.argv[4], status))
except Exception as error:
    relay.write("%r\n" % (error,))
relay.flush()
`;

const windows = {
  displayMode: size => WINDOWS_SHELL + SENDER_DIRECTORY +
    `cat > "$dir/set_display_mode.py" <<'JETKVM_END'\n${WINDOWS_DISPLAY_MODE}\nJETKVM_END\n` +
    `python -u "$(cygpath -m "$dir/set_display_mode.py")" relay${sizeArguments(size)}`,

  watchKeys:
    WINDOWS_SHELL + SENDER_DIRECTORY +
    `cat > "$dir/watch_keys.py" <<'JETKVM_END'\n${WINDOWS_KEY_WATCHER}\nJETKVM_END\n` +
    'python -u "$(cygpath -m "$dir/watch_keys.py")" relay',

  watchMedia: WINDOWS_SHELL + `python -u -c ${quote(WINDOWS_MEDIA)} watch ${WINDOWS_READER_COUNT} ${WINDOWS_BLOCKS}`,
  medium: fields => ({ type: fields[0], device: fields[1], bytesRead: Number(fields[2]), ...blocksOf(fields) }),
  expectBlocks,

  startReader: WINDOWS_SHELL +
    `python -c ${quote(WINDOWS_LAUNCH)} ${quote(WINDOWS_MEDIA)} ${WINDOWS_READER_COUNT} ${WINDOWS_BLOCKS} > ${READER_FILE}`,
  // The reader is one process. Only a Python process with that ID is stopped.
  stopReader: WINDOWS_SHELL +
    `f=${READER_FILE}; [ -f "$f" ] || exit 0; g=$(tr -d '\\r' < "$f"); rm -f "$f"; ` +
    'case "$g" in ""|*[!0-9]*) exit 0;; esac; ' +
    'tasklist /fi "PID eq $g" /fo csv /nh | grep -qi \'^"python\' || exit 0; ' +
    'taskkill /f /pid "$g" >/dev/null 2>&1; true',

  senderPackage: 'windows-x86_64',
  senderSuffix: '.exe',
  // The package is an installer. It is downloaded unless the file with this
  // SHA-256 is already there, and run without questions over what is
  // installed. The sender's own --install-service makes the task and starts
  // it, so the last step stops it again.
  installSender: ({ url, sha256, file, fingerprint }) =>
    'set -e; ' + SENDER_DIRECTORY +
    `f="$dir/${file}"; ` +
    `if ! echo "${sha256}  $f" | sha256sum -c --status 2>/dev/null; then ` +
    `curl -fsSL --retry 2 -o "$f.part" ${quote(url)}; echo "${sha256}  $f.part" | sha256sum -c --status; ` +
    'mv "$f.part" "$f"; fi; ' +
    `${WINDOWS_STOP_SENDER}; "$f" /S; ` +
    `"$exe" --approve-kvm ${quote(fingerprint)}; ` +
    '"$exe" --install-service; ' +
    WINDOWS_STOP_SENDER,
  startSender: WINDOWS_SHELL + WINDOWS_SENDER + 'schtasks /run /tn "$task" >/dev/null',
  stopSender: WINDOWS_STOP_SENDER,

  // A path has backslashes, with which sha256sum begins the file's line
  // with one.
  checksumsAndRemove: paths => `sha256sum ${paths.map(quote).join(' ')}; rm -f ${paths.map(quote).join(' ')}`,
  // The sender's terminal is PowerShell. Windows passes each write to the
  // console on as one message of the session, and takes about 5000 writes
  // per second, so the numbers go out 1000 per write, about 8 KiB, which is
  // also what seq writes at a time on Linux.
  countInTerminal: 'for($i=1;$i -lt 2000000000;$i+=1000){[Console]::Out.Write("t"+[DateTimeOffset]::UtcNow.ToUnixTimeMilliseconds()+"`r`n"+($i..($i+999) -join "`r`n")+"`r`n")}\r',
  // The answer of the light terminal: PowerShell reads each line from the
  // console and writes it back with its first "e" made an "r".
  answerInTerminal: '[Console]::Out.Write("ready`r`n");for(;;){$l=[Console]::In.ReadLine();if($null -eq $l){break};[Console]::Out.Write(($l -replace \'^e\',\'r\')+"`r`n")}\r',

  openPage: url => WINDOWS_CLOSE_PAGE +
    `cat > "$dir/show_page.py" <<'JETKVM_END'\n${windowsPage(url)}\nJETKVM_END\n` +
    'python "$(cygpath -m "$dir/show_page.py")" start',
  closePage: WINDOWS_CLOSE_PAGE + 'true',
};

const HOSTS = { linux, macos, windows };
/** The names of the hosts, as a recording's first row and a known failure's "host" have them. */
/**
 * The SHA-256 of the file at `path` in what a host's checksumsAndRemove
 * printed, or '' when the host had no such file. A line has a file's
 * checksum and ends with its path; a path with backslashes makes the line
 * begin with one. The file is found by its name, which the paths of one
 * command do not share.
 */
export function checksumIn(printed, path) {
  const name = path.split(/[\\/]/).pop();
  const line = printed.split('\n').find(one => one.trimEnd().endsWith(name)) ?? '';
  return /^\\?([0-9a-f]{64})\s/.exec(line)?.[1] ?? '';
}

export const HOST_NAMES = Object.keys(HOSTS);

/** The host for the output of `uname -s` there. */
export function hostFor(uname) {
  const name = uname.trim();
  // Git for Windows is MINGW64_NT-<version>; its base, MSYS2, is MSYS_NT-<version>.
  const system = name === 'Linux' ? 'linux' : name === 'Darwin' ? 'macos'
    : /^(MINGW|MSYS)\w*_NT/.test(name) ? 'windows' : null;
  if (!system) throw new Error(`the tool cannot drive a host whose uname -s is "${name}"`);
  return { system, ...HOSTS[system] };
}
