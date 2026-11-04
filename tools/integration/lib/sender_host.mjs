// The OS Services sender on the bench's host: process, log, approvals,
// program, and the Mini's USB device.
import fs from 'node:fs';
import path from 'node:path';
import { fileURLToPath } from 'node:url';
import {
  LINUX_SENDER_UNIT, LINUX_USER_SESSION, MACOS_SENDER, WINDOWS_SENDER, WINDOWS_SHELL, quote,
} from '../../combined_load/runner/hosts.mjs';
import { connectHost } from './host.mjs';
import { until } from './mini.mjs';

const root = path.resolve(path.dirname(fileURLToPath(import.meta.url)), '..', '..', '..');
const tool = name => fs.readFileSync(path.join(root, 'tools', 'e2e', name), 'utf8');

// Class, subclass and protocol of the OS Services USB interface.
export const OS_SERVICES_TRIPLE = 'ff4a01';

export const FINGERPRINT = /^(?:[0-9A-F]{2}:){7}[0-9A-F]{2}$/;
const FINGERPRINT_TEXT = '((?:[0-9A-F]{2}:){7}[0-9A-F]{2})';

// Sender log lines: a KVM that is not approved, and the USB link ready.
export const SENDER_NOT_APPROVED = new RegExp(`the KVM ${FINGERPRINT_TEXT} is not approved`);
export const SENDER_SESSION_READY = /startup phase=usb_ready elapsed_ms=(\d+)/;
// The errors the sender logs when the KVM breaks the protocol.
// The Mini disappearing must not produce any of these.
export const SENDER_PROTOCOL_ERROR = new RegExp([
  'payload is too large',
  'payload has trailing bytes',
  'payload is truncated',
  'invalid (?:KVM USB reset status|clipboard|display resolution|encoded|file|frame rate|maximum frame rate|terminal|update|video)',
  'invalid \\w+ value',
  'reserved byte',
  'is not UTF-8',
  'text is too large',
  'without an? (?:update|transfer) id',
  'names no files',
  'has an invalid id',
  'must be non-zero',
  'needs non-zero',
  'carries no data',
  'must have an empty payload',
  'empty session token',
  'did not send session\\.config',
  'did not arrive within the search limit',
  // serde_json errors on session.config
  'at line \\d+ column \\d+',
].join('|'));
// What the sender logs when the Mini leaves the bus mid-session.
export const SENDER_DEVICE_GONE =
  /disconnected|cancelled|endpoint stall|reader stopped|write timed out|no such device|no OS Services USB interface found/i;

const NOW_MS = '$(($(date +%s%N) / 1000000))';
// macOS date has no %N.
const MACOS_NOW_MS = '$(perl -MTime::HiRes=time -e \'printf "%d", time * 1000\')';

// Logs without timestamps (macOS, Windows): each clock read appends
// "<ms> <log size>" to a marks file keyed by the log's inode, so log(since)
// can start at the log's size at `since`. Expects $log and $marks.
const markedClock = (now, size) => `now=${now}; echo "$now $(${size} 2>/dev/null || echo 0)" >> "$marks"; echo $now`;
const MARKED_LOG =
  'o=$(awk -v s=SINCE \'$1 <= s && $2 > o { o = $2 } END { print o + 0 }\' "$marks" 2>/dev/null); ' +
  'o=${o:-0}; echo $o; awk -v o=$o \'$2 >= o\' "$marks" 2>/dev/null; ' +
  'echo ---; tail -c +$((o + 1)) "$log"';
function markedEntries(output, since) {
  const split = output.indexOf('\n---\n');
  const [start, ...markLines] = output.slice(0, split).split('\n');
  const marks = markLines.map(line => line.split(' ').map(Number));
  const entries = [];
  let offset = Number(start);
  for (const line of output.slice(split + 5).split('\n')) {
    const at = Math.max(since, ...marks.filter(([, size]) => size <= offset).map(([time]) => time));
    const message = line.replace(/\r$/, '');
    if (message !== '') entries.push({ at, message });
    offset += Buffer.byteLength(line, 'utf8') + 1;
  }
  return entries;
}

const MACOS_LOG = MACOS_SENDER +
  'log="$HOME/Library/Logs/IP-KVM OS Services/service.log"; ' +
  'marks="${TMPDIR:-/tmp}/jetkvm-integration-log-marks-$(stat -f %i "$log" 2>/dev/null || echo none)"; ';
const MACOS_PID = 'launchctl print $t 2>/dev/null | sed -n \'s/^\tpid = //p\' | head -1';
const WINDOWS_LOG = WINDOWS_SHELL + WINDOWS_SENDER +
  'log="$LOCALAPPDATA/IP-KVM OS Services/logs/service.log"; ' +
  'marks="${TMPDIR:-/tmp}/jetkvm-integration-log-marks-$(stat -c %i "$log" 2>/dev/null || echo none)"; ';

// Tab-separated device/interface/endpoint lines from sysfs for one vendor:product.
const LINUX_USB_LISTING = id =>
  'r() { cat "$1" 2>/dev/null | tr -d "\\t\\n"; }; ' +
  'for d in /sys/bus/usb/devices/*; do ' +
  '[ -f "$d/idVendor" ] || continue; ' +
  `[ "$(r "$d/idVendor"):$(r "$d/idProduct")" = ${quote(id)} ] || continue; ` +
  'n=${d##*/}; ' +
  'printf "device\\t%s\\t%s\\t%s\\t%s\\t%s\\t%s\\t%s\\t%s\\t%s\\n" "$n" "$(r "$d/busnum")" "$(r "$d/devnum")" ' +
  '"$(r "$d/speed")" "$(r "$d/version" | tr -d " ")" "$(r "$d/bcdDevice")" ' +
  '"$(r "$d/manufacturer")" "$(r "$d/product")" "$(r "$d/serial")"; ' +
  'for i in "$d/$n":*; do ' +
  '[ -f "$i/bInterfaceClass" ] || continue; ' +
  'drv=$(readlink "$i/driver" 2>/dev/null); drv=${drv##*/}; ' +
  'printf "interface\\t%s\\t%s\\t%s\\t%s\\t%s\\n" "${i##*/}" "$(r "$i/bInterfaceNumber")" ' +
  '"$(r "$i/bInterfaceClass")$(r "$i/bInterfaceSubClass")$(r "$i/bInterfaceProtocol")" ' +
  '"$(r "$i/bNumEndpoints")" "${drv:--}"; ' +
  'for e in "$i"/ep_*; do [ -d "$e" ] || continue; ' +
  'printf "endpoint\\t%s\\t%s\\t%s\\t%s\\n" "$(r "$e/bEndpointAddress")" "$(r "$e/type")" ' +
  '"$(r "$e/direction")" "$(r "$e/wMaxPacketSize")"; ' +
  'done; done; done';

// Prints "<ms> <id> <devnum> <interface state>" when the port's device changes,
// and "tick" once a second. Builtins only, so a 50 ms poll stays cheap.
const LINUX_PORT_WATCH = `
exec 2>/dev/null
p=$1; prev=x; n=0
while :; do
  d=/sys/bus/usb/devices/$p; s="- - -"
  if read -r v < "$d/idVendor" && read -r pr < "$d/idProduct" && read -r dn < "$d/devnum"; then
    o=none
    for i in "$d/$p":*; do
      read -r c < "$i/bInterfaceClass" || continue
      read -r sc < "$i/bInterfaceSubClass" || continue
      read -r pc < "$i/bInterfaceProtocol" || continue
      [ "$c$sc$pc" = ${OS_SERVICES_TRIPLE} ] || continue
      if [ -e "/sys/bus/usb/drivers/usbfs/\${i##*/}" ]; then o=usbfs
      elif [ -e "$i/driver" ]; then o=other
      else o=free; fi
    done
    s="$v:$pr $dn $o"
  fi
  if [ "$s" != "$prev" ]; then echo "${NOW_MS} $s"; prev=$s; n=0; fi
  n=$((n + 1)); [ "$n" -lt 20 ] || { echo tick; n=0; }
  sleep 0.05
done`;

// Per-OS POSIX sh commands. `approvals` sets $f to the approved-kvms file
// in the user's local configuration directory.
const SYSTEMS = {
  linux: {
    now: `echo ${NOW_MS}`,
    state: LINUX_USER_SESSION + `systemctl --user show -p LoadState,ActiveState,MainPID ${LINUX_SENDER_UNIT}`,
    readState: text => {
      const unit = Object.fromEntries(text.trim().split('\n').map(line => line.split('=')));
      return { installed: unit.LoadState === 'loaded', running: unit.ActiveState === 'active', pid: unit.MainPID };
    },
    log: since => 'uid=$(id -u); sudo -n journalctl --no-pager -o json --output-fields=MESSAGE ' +
      `--since @${Math.floor(since / 1000) - 1} _SYSTEMD_USER_UNIT=${LINUX_SENDER_UNIT} _UID=$uid`,
    entries: (output, since) => output.split('\n').filter(line => line.trim() !== '').map(line => {
      const entry = JSON.parse(line);
      // journald returns non-UTF-8 messages as byte arrays.
      const message = typeof entry.MESSAGE === 'string' ? entry.MESSAGE
        : Buffer.from(Array.isArray(entry.MESSAGE) ? entry.MESSAGE : []).toString('utf8');
      return { at: Number(entry.__REALTIME_TIMESTAMP) / 1000, message };
    }).filter(entry => entry.at >= since),
    // The user manager's XDG_CONFIG_HOME, not the SSH session's.
    approvals: LINUX_USER_SESSION + 'cfg=$(systemctl --user show-environment | sed -n \'s/^XDG_CONFIG_HOME=//p\'); ' +
      'f="${cfg:-$HOME/.config}/ipkvm-os-services/approved-kvms"; ',
    programSum: LINUX_USER_SESSION + `prog=$(systemctl --user show -p ExecStart --value ${LINUX_SENDER_UNIT} | ` +
      'sed -n \'s/^{ path=\\(.*\\) ; argv\\[\\].*/\\1/p\'); sha256sum "$prog" | cut -d" " -f1',
    usbListing: id => ({ command: LINUX_USB_LISTING(id) }),
    portWatch: port => `timeout 7200 bash -c ${quote(LINUX_PORT_WATCH)} jetkvm-port-watch ${quote(port)}`,
  },
  macos: {
    now: MACOS_LOG + markedClock(MACOS_NOW_MS, 'stat -f %z "$log"'),
    state: MACOS_SENDER + `pid=$(${MACOS_PID}); ` +
      'if launchctl print $t >/dev/null 2>&1 || [ -f "$p" ]; then echo installed; fi; echo "pid $pid"',
    readState: text => {
      const pid = /^pid (\d+)$/m.exec(text)?.[1] ?? '0';
      return { installed: /^installed$/m.test(text), running: pid !== '0', pid };
    },
    log: since => MACOS_LOG + MARKED_LOG.replace('SINCE', since),
    entries: markedEntries,
    approvals: 'f="$HOME/Library/Application Support/ipkvm-os-services/approved-kvms"; ',
    programSum: MACOS_SENDER + 'e=$(plutil -extract ProgramArguments.0 raw "$p") && shasum -a 256 "$e" | cut -d" " -f1',
    // The lister imports the probe module from a directory.
    usbListing: id => ({
      command: 't=$(mktemp -d) && cat > "$t/os_services_usb_probe.py" && ' +
        `python3 -c ${quote(tool('os_services_usb_macos.py'))} "$t" ${quote(id)}; s=$?; rm -rf "$t"; exit $s`,
      input: tool('os_services_usb_probe.py'),
    }),
    // Also appends log-size marks as the log grows.
    portWatch: port => MACOS_LOG +
      `python3 -c ${quote(tool('os_services_usb_port_watch_macos.py'))} ${quote(port)} ${OS_SERVICES_TRIPLE} 7200 "$log" "$marks"`,
  },
  windows: {
    now: WINDOWS_LOG + markedClock(NOW_MS, 'stat -c %s "$log"'),
    state: WINDOWS_SHELL + WINDOWS_SENDER +
      'if [ -f "$exe" ] && schtasks /query /tn "$task" >/dev/null 2>&1; then echo installed; fi; ' +
      'tasklist /fi "imagename eq ipkvm-os-services.exe" /fo csv /nh | tr -d \'"\\r\' | ' +
      'awk -F, \'$1 == "ipkvm-os-services.exe" { print "pid " $2 }\'',
    readState: text => {
      const pids = [...text.matchAll(/^pid (\d+)$/gm)].map(match => Number(match[1])).sort((a, b) => a - b);
      return { installed: /^installed$/m.test(text), running: pids.length > 0, pid: pids.join(' ') || '0' };
    },
    log: since => WINDOWS_LOG + MARKED_LOG.replace('SINCE', since),
    entries: markedEntries,
    approvals: 'f="$LOCALAPPDATA/ipkvm-os-services/approved-kvms"; ',
    programSum: WINDOWS_SHELL + WINDOWS_SENDER + 'sha256sum < "$exe" | cut -d" " -f1',
  },
};

// Log commands for callers with their own SSH session: { now, log(since),
// entries(output, since) → [{ at, message }] }.
export function senderLogCommands(system) {
  const { now, log, entries } = SYSTEMS[system];
  return { now, log, entries };
}

const hex4 = value => Number.parseInt(value, 16).toString(16).padStart(4, '0');

// A device's identity without bus and device numbers.
export function usbIdentity(device) {
  return {
    id: device.id,
    version: device.version,
    bcdDevice: device.bcdDevice,
    manufacturer: device.manufacturer,
    product: device.product,
    serial: device.serial,
    interfaces: device.interfaces.map(one => ({
      number: one.number,
      triple: one.triple,
      endpoints: one.endpoints.map(e => `${e.address} ${e.type} ${e.direction} ${e.maxPacketSize}`),
    })).sort((a, b) => a.number - b.number),
  };
}

// connectHost(mini) plus sender helpers; times are the host's clock in ms.
// usb, lsusb and watchPort need a Linux or macOS host.
export async function senderHost(mini) {
  const host = await connectHost(mini);
  const { login, system, exec } = host;
  const os = SYSTEMS[system];
  const needsUsb = what => {
    if (!os.usbListing) throw new Error(`${what} needs a Linux or macOS host, and ${login} runs ${system}`);
  };

  const now = async () => Number((await exec(os.now)).trim());
  const sender = async () => os.readState(await exec(os.state));
  const senderLog = async since => os.entries(await exec(os.log(since)), since);
  const waitForSenderLog = (since, pattern, seconds) => until(
    async () => (await senderLog(since)).find(entry => pattern.test(entry.message)), Boolean, seconds);
  const programSum = async () => (await exec(os.programSum, { timeoutMs: 60_000 })).trim();

  const approvals = async () => {
    const [state, ...encoded] = (await exec(os.approvals +
      'if [ -e "$f" ]; then echo present; base64 < "$f"; else echo absent; fi')).trim().split('\n');
    return { present: state === 'present', content: Buffer.from(encoded.join(''), 'base64').toString('utf8') };
  };
  const usbDevices = async id => {
    const { command, input } = os.usbListing(id);
    const devices = [];
    for (const line of (await exec(command, { input })).split('\n').filter(Boolean)) {
      const [kind, ...fields] = line.split('\t');
      if (kind === 'device') {
        devices.push({
          name: fields[0], id, busnum: Number(fields[1]), devnum: Number(fields[2]), speed: fields[3],
          version: fields[4], bcdDevice: fields[5], manufacturer: fields[6], product: fields[7], serial: fields[8],
          interfaces: [],
        });
      } else if (kind === 'interface') {
        devices.at(-1).interfaces.push({
          name: fields[0], number: Number.parseInt(fields[1], 16), triple: fields[2].toLowerCase(),
          endpointCount: Number.parseInt(fields[3], 16), driver: fields[4] === '-' ? null : fields[4], endpoints: [],
        });
      } else if (kind === 'endpoint') {
        devices.at(-1).interfaces.at(-1).endpoints.push({
          address: fields[0], type: fields[1], direction: fields[2], maxPacketSize: Number.parseInt(fields[3], 16),
        });
      }
    }
    return devices;
  };
  const usb = async mini => {
    needsUsb('the Mini\'s USB device');
    const config = await mini.rpc('getUsbConfig');
    const id = `${hex4(config.vendor_id)}:${hex4(config.product_id)}`;
    const devices = await until(() => usbDevices(id), found => found.length > 0, 30);
    if (devices.length !== 1) throw new Error(`${login} has ${devices.length} USB devices ${id}, and the Mini is one`);
    const [device] = devices;
    const transports = device.interfaces.filter(one => one.triple === OS_SERVICES_TRIPLE);
    if (transports.length > 1) throw new Error(`${id} has ${transports.length} interfaces ${OS_SERVICES_TRIPLE}`);
    return { device, osServices: transports[0] };
  };
  // Bus and device numbers stripped: they change on every enumeration.
  const lsusb = async id => {
    needsUsb('lsusb');
    if (!(await exec('command -v lsusb >/dev/null && echo yes || true')).trim()) return null;
    const strip = text => text.split('\n').map(line => line.replace(/^Bus \d+ Device \d+: /, ''))
      .filter(line => !/^\s*$/.test(line)).join('\n');
    return {
      line: strip(await exec(`lsusb -d ${quote(id)}`)),
      verbose: strip(await exec(`sudo -n lsusb -v -d ${quote(id)} 2>/dev/null`)),
    };
  };

  // samples: { at, present, id, devnum, osServices } per change; osServices is
  // 'none', 'free', 'usbfs' (claimed by the sender) or 'other'.
  const watchPort = async port => {
    needsUsb('the watch of a USB port');
    const samples = [];
    const { stop } = host.stream(os.portWatch(port), line => {
      const [at, id, devnum, osServices] = line.trim().split(' ');
      if (!at || at === 'tick' || devnum === undefined) return;
      const present = id !== '-';
      samples.push({
        at: Number(at), present, id: present ? id : null, devnum: present ? Number(devnum) : null,
        osServices: present ? osServices : null,
      });
    });
    try {
      await until(() => samples.length, Boolean, 20);
    } catch (error) {
      stop();
      throw new Error(`the watch of the USB port ${port} on ${login} reports nothing: ${error.message}`);
    }
    const stateAt = at => samples.filter(sample => sample.at <= at).at(-1);
    // A departure too short for the 50 ms poll shows as a new devnum, with
    // goneAt === backAt.
    const reenumeration = after => {
      let previous = stateAt(after);
      const times = { goneAt: undefined, backAt: undefined, claimedAt: undefined, departures: 0 };
      for (const sample of samples.filter(one => one.at > after)) {
        const left = previous?.present === true &&
          (!sample.present || (sample.devnum !== null && sample.devnum !== previous.devnum));
        if (left) {
          times.departures++;
          times.goneAt ??= sample.at;
        }
        if (times.goneAt !== undefined && times.backAt === undefined && sample.present) times.backAt = sample.at;
        if (times.backAt !== undefined && times.claimedAt === undefined && sample.osServices === 'usbfs') times.claimedAt = sample.at;
        previous = sample;
      }
      return times;
    };
    return { samples, stateAt, reenumeration, stop };
  };

  return { ...host, now, sender, senderLog, waitForSenderLog, programSum, approvals, usb, lsusb, watchPort };
}
