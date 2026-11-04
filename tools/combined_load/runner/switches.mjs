// The variables that a list of states can name. Each variable is a switch:
//
//   on()      start it
//   off()     stop it; must work from any condition, because it is also the cleanup
//   isOn()    whether it is on, read from outside the device where possible;
//             null while the switch cannot tell, which agrees with neither
//   works(on) optional: whether a change to this value has taken effect. The
//             switch of a change is done when isOn() agrees and works() is
//             true; the driver then waits for the new state to settle.
//             Instead of false it can return a text that says what it saw,
//             which the driver records for a switch that is not done in time.
//   ends      optional: the variables that this one ends when it comes on.
//             The driver turns those that are on off when this switch is
//             done, so that the state names only what still runs.
//   sample()  optional: counters that neither the device nor the browser has
//   prepare() optional: what the bench needs before the variable can go on.
//             The run calls it once, before the list and outside every
//             measured change, for the variables the list names. It returns
//             facts about the bench for the recording's first row.
//
// VARIABLES is in dependency order: the driver turns variables on in this
// order and off in reverse. video is first because RPC rides its data
// channel. osServices, its parts and audio come before the mounts, which
// take the Mini off USB. Disturbances (packetLoss, usbEmulation,
// senderRestart, takeover) come after the loads they disturb. ota is last:
// it restarts the Mini.
import { execFile, spawn } from 'node:child_process';
import { createHash, generateKeyPairSync, randomBytes } from 'node:crypto';
import dgram from 'node:dgram';
import fs from 'node:fs';
import http from 'node:http';
import https from 'node:https';
import { promisify } from 'node:util';
import { beepClock, hearScript, levelBetween, pageSound } from './audio.mjs';
import { blockChecksums, imageBytes } from './content.mjs';
import { sleep, until } from './driver.mjs';
import { serverCertificate, startFakeCloud } from './fake_cloud.mjs';
import { keyAnswers, moveHand, parseReport, watchKeyStates } from './hid_times.mjs';
import { HID_KEYS, hostFor, quote } from './hosts.mjs';
import { KEY_PERIOD_MS } from './judge.mjs';
import { mqttSwitch } from './mqtt.mjs';
import { osServicesParts } from './os_services.mjs';
import { packetLossSwitch } from './packet_loss.mjs';
import { diagnostics as readDiagnostics, pageCall, pageRpc } from './page.mjs';
import { signDetached, testReleaseKey } from './release_signature.mjs';
import { cdromDrive, setDefaults } from './settings.mjs';
import { SOCKET_EVENTS, firstFrame, peerConnections, signalingSockets, takeoverSwitch } from './takeover.mjs';
import { tailscaleSwitch } from './tailscale.mjs';

export const VARIABLES = ['video', 'tailscale', 'mqtt', 'cloud', 'osServices', 'osVideo', 'osTerminal', 'osTerminalLight', 'osFiles', 'osFilesSmall', 'osClipboard', 'hid',
  'slowKeys', 'qualityChange', 'audio', 'noisePicture', 'httpMountCdrom', 'httpMountDisk', 'httpsMountDisk', 'tfMountCdrom', 'tfMountDisk', 'updateCheck',
  'upload', 'hostModeChange', 'usbEmulation', 'senderRestart', 'packetLoss', 'takeover', 'ota'];
/** The variables that mount an image. */
export const MOUNTS = ['httpMountCdrom', 'httpMountDisk', 'httpsMountDisk', 'tfMountCdrom', 'tfMountDisk'];

// The image that the Mini mounts from a URL. The tool serves it over HTTP on
// the LAN, so a mount and the host's reads depend on the Mini and not on the
// internet. For httpsMountDisk it serves the same image over HTTPS. It is
// 8 blocks of 1 MiB, each with content of its own (content.mjs), and the
// host's reader checks each block that it has read against IMAGE_SUMS. A
// CD-ROM has 2048-byte sectors.
const IMAGE = imageBytes(1, 8);
const IMAGE_SUMS = blockChecksums(IMAGE);

// The version that the tool's update server announces. The Mini installs an
// update only when the announced version is higher than its own; it does not
// read the version inside the image. So the image the Mini already runs can
// be installed again under this version.
const ANNOUNCED_VERSION = '99.0.0';
// The update server that the Mini uses outside a run (JETKVM_OTA_DEFAULT_API_URL).
const UPDATE_SERVER = 'https://api.jetkvm.com';
// The cloud that the Mini uses outside a run (JETKVM_DEFAULT_CLOUD_URL and its app).
const CLOUD = { apiUrl: 'https://api.jetkvm.com', appUrl: 'https://app.jetkvm.com' };
// esp_reset_reason(): a restart that the firmware asked for, as after an update.
const ESP_RST_SW = 3;

// A new Ed25519 key that belongs to no release key, for signDetached.
function otherKey() {
  const { privateKey } = generateKeyPairSync('ed25519');
  return {
    signingFingerprint: randomBytes(20).toString('hex'),
    // PKCS #8 of an Ed25519 key ends with its 32 octets.
    signingPrivateKey: privateKey.export({ format: 'der', type: 'pkcs8' }).subarray(-32).toString('hex'),
  };
}

/** The version inside a firmware image (esp_app_desc_t) and the image's SHA-256. */
export function readFirmware(file) {
  const bytes = fs.readFileSync(file);
  // The image header (24 bytes), the first segment's header (8), then the
  // description: 16 bytes, the version as 32 bytes of text, 96 bytes of
  // other text, and the SHA-256 of the build's ELF file.
  const version = bytes.subarray(48, 80).toString('utf8').replace(/\0.*$/s, '');
  const elfSha256 = bytes.subarray(176, 208).toString('hex');
  return { bytes, version, elfSha256, sha256: createHash('sha256').update(bytes).digest('hex') };
}

// What the host's screen shows and plays during a run: one file, which the
// tool serves and the host's browser opens.
const TARGET_PAGE = new URL('../target_page.html', import.meta.url);

// The tool's HTTP server, which the Mini reaches at `address`: IMAGE for the
// URL mounts, to range requests, which is how the Mini reads an image; with
// a firmware image, the update server's answer, which names the image at the
// address that the request came to, that image, its signature by
// the test release key, and that key as a keyserver serves it, which a bench
// build asks its update server for (release_signature.mjs); and, for the
// host's browser, the target page, which reports once per second what it
// shows. The answer to a report tells the page whether to play its sound,
// which is served.pageSound; the page is silent until that is true. The
// page also keeps a request to /target-page/sound open, which names the
// wish that the page has. wantSound(on) sets the wish and answers that
// request, so the page turns its sound on or off without a wait.
// served.onPageReport, when set, gets each report of the page.
// secure() starts a second server for the same requests, which speaks TLS
// with a certificate that the e2e test CA vouches for (fake_cloud.mjs), and
// returns its https address (base) and the image's https URL (url). A bench
// build trusts that CA.
//
// A web server ends a connection after a number of requests (nginx after
// 1000, Apache after 100), and the Mini, which reads a mounted image on one
// connection, then opens the next. The tool's server does that once per
// download of the firmware image: its answer to the first read of the
// mounted image during the download has "Connection: close". So the Mini
// has to open the image's connection again while the update downloads, in
// every run and not only when a count of requests falls into that time.
//
// offer changes what the update server announces, for the integration tests
// (tools/integration): version is the announced version, and signature is
// 'valid' (by the test release key), 'none' (the answer names no signature),
// 'empty' (a file without content), 'other key' (a valid signature by a key
// that is not the release key's) or 'other image' (the test release key's
// signature of another image). A request for a version by its number
// (systemVersion) is answered with that version, as the cloud answers it.
export function serve(address, firmware, offer = {}) {
  const { version = ANNOUNCED_VERSION, signature: signed = 'valid' } = offer;
  const served = { firmwareBytes: 0, firmwareDownloads: 0, pageReport: null, pageSound: false, pageNoise: false, onPageReport: null };
  const releaseKey = firmware && testReleaseKey();
  const signature = !firmware || signed === 'none' ? null
    : signed === 'empty' ? Buffer.alloc(0)
      : signed === 'other image' ? signDetached(Buffer.concat([firmware.bytes, Buffer.from([0])]), releaseKey)
        : signDetached(firmware.bytes, signed === 'other key' ? otherKey() : releaseKey);
  const page = fs.readFileSync(TARGET_PAGE);
  const soundWish = () => JSON.stringify({ sound: served.pageSound, noise: served.pageNoise });
  // The answers to the page's open requests for the wish.
  const waiting = new Set();
  const wantSound = on => {
    served.pageSound = Boolean(on);
    for (const answer of [...waiting]) answer();
  };
  // The firmware image is being downloaded, and no read of the mounted
  // image has been answered since the download began.
  let endImageConnection = false;
  const handle = (request, response) => {
    const { pathname, searchParams } = new URL(request.url, 'http://localhost');
    const base = `${request.socket.encrypted ? 'https' : 'http'}://${address}:${request.socket.localPort}`;
    if (pathname === '/target-page') {
      response.writeHead(200, { 'Content-Type': 'text/html; charset=utf-8', 'Cache-Control': 'no-store' });
      response.end(page);
      return;
    }
    if (pathname === '/target-page/report' && request.method === 'POST') {
      let body = '';
      request.setEncoding('utf8');
      request.on('data', chunk => { body += chunk; });
      request.on('end', () => {
        try {
          served.pageReport = { ...JSON.parse(body), at: Date.now() };
          served.onPageReport?.(served.pageReport);
          response.writeHead(200, { 'Content-Type': 'application/json' }).end(soundWish());
        } catch {
          response.writeHead(400).end();
        }
      });
      return;
    }
    if (pathname === '/target-page/sound') {
      // Answered when the wish is not the one that the page has, and after
      // 10 seconds, so that a page that is gone leaves no request.
      const answer = () => {
        clearTimeout(timer);
        waiting.delete(answer);
        if (!response.writableEnded) response.writeHead(200, { 'Content-Type': 'application/json' }).end(soundWish());
      };
      const timer = setTimeout(answer, 10_000);
      waiting.add(answer);
      response.on('close', () => {
        clearTimeout(timer);
        waiting.delete(answer);
      });
      if (searchParams.get('wanted') !== String(served.pageSound)) answer();
      return;
    }
    if (pathname === '/releases' && firmware) {
      response.writeHead(200, { 'Content-Type': 'application/json' });
      response.end(JSON.stringify({
        systemVersion: searchParams.get('systemVersion') ?? version,
        systemUrl: `${base}/jetkvm-mini.bin`, systemHash: firmware.sha256,
        ...(signature ? { systemSigUrl: `${base}/jetkvm-mini.bin.sig` } : {}),
      }));
      return;
    }
    if (pathname === '/jetkvm-mini.bin.sig' && signature) {
      response.writeHead(200, { 'Content-Type': 'application/pgp-signature', 'Content-Length': signature.length }).end(signature);
      return;
    }
    if (pathname.startsWith('/keyserver/')) {
      if (!releaseKey || pathname !== `/keyserver/${releaseKey.rootFingerprint}`) return void response.writeHead(404).end();
      response.writeHead(200, { 'Content-Type': 'application/pgp-keys', 'Content-Length': releaseKey.publicKey.length }).end(releaseKey.publicKey);
      return;
    }
    if (pathname === '/jetkvm-mini.bin' && firmware) {
      endImageConnection = true;
      served.firmwareDownloads++;
      response.on('close', () => {
        endImageConnection = false;
        served.firmwareDownloads--;
      });
      response.writeHead(200, { 'Content-Type': 'application/octet-stream', 'Content-Length': firmware.bytes.length });
      response.on('drain', () => { served.firmwareBytes = request.socket.bytesWritten; });
      response.end(firmware.bytes, () => { served.firmwareBytes = firmware.bytes.length; });
      return;
    }
    const range = /^bytes=(\d+)-(\d*)$/.exec(request.headers.range ?? '');
    const first = range ? Number(range[1]) : 0;
    const last = range?.[2] ? Math.min(Number(range[2]), IMAGE.length - 1) : IMAGE.length - 1;
    if (first > last) {
      response.writeHead(416, { 'Content-Range': `bytes */${IMAGE.length}` }).end();
      return;
    }
    response.writeHead(range ? 206 : 200, {
      'Content-Type': 'application/octet-stream', 'Accept-Ranges': 'bytes', 'Content-Length': last - first + 1,
      ...(range ? { 'Content-Range': `bytes ${first}-${last}/${IMAGE.length}` } : {}),
      ...(endImageConnection ? { Connection: 'close' } : {}),
    });
    endImageConnection = false;
    response.end(request.method === 'HEAD' ? undefined : IMAGE.subarray(first, last + 1));
  };
  const server = http.createServer(handle);
  // Started at its first use: the certificate needs the openssl command.
  let secure = null;
  const secureServer = () => new Promise((resolve, reject) => {
    const addresses = () => {
      const base = `https://${address}:${secure.address().port}`;
      return { base, url: `${base}/combined-load.iso` };
    };
    if (secure) return resolve(addresses());
    secure = https.createServer(serverCertificate(address), handle);
    secure.once('error', reject);
    secure.listen(0, () => resolve(addresses()));
  });
  return new Promise(resolve => server.listen(0, () => {
    const base = `http://${address}:${server.address().port}`;
    resolve({
      base, url: `${base}/combined-load.iso`, port: server.address().port, served, wantSound, secure: secureServer,
      close: () => {
        for (const answer of [...waiting]) answer();
        for (const one of [server, secure]) {
          one?.closeAllConnections();
          one?.close();
        }
      },
    });
  }));
}

/** The address this machine uses to reach `host`. */
export function addressTowards(host) {
  return new Promise((resolve, reject) => {
    const socket = dgram.createSocket('udp4');
    socket.once('error', reject);
    socket.connect(9, host, () => {
      const { address } = socket.address();
      socket.close();
      resolve(address);
    });
  });
}

export const SSH_OPTIONS = [
  '-o', 'UserKnownHostsFile=/dev/null', '-o', 'StrictHostKeyChecking=no', '-o', 'LogLevel=ERROR',
  '-o', 'BatchMode=yes', '-o', 'ConnectTimeout=30', '-o', 'ServerAliveInterval=5', '-o', 'ServerAliveCountMax=3',
];

// hid clicks the keys of HID_KEYS in turn, one every KEY_PERIOD_MS: a click
// is the key down, and the key up when the Mini's message lists the key as
// down, or KEY_HOLD_MS after the key down without one. A key that went up
// before the Mini read the keyboard's state would be in no message. At one
// key per KEY_PERIOD_MS at most 5 keys are down, below the 6 of the Mini's
// keyboard report.
const KEY_HOLD_MS = 500;
// qualityChange sets the preset Medium of the video settings, and sets the
// factor back to the one that the run's start has stored: 0, which is Auto.
const QUALITY_MEDIUM = 0.5;
const QUALITY_STANDING = 0;
// The share of the screen's width and height that the target page's window
// has at least when it shows the noise picture: the window's title bar and
// the desktop's bars take 5 to 11 % of the height on the bench hosts.
const NOISE_COVERS = { width: 0.95, height: 0.85 };
// Whether the target page's last report shows the noise picture: true when
// the page has drawn it in a window that covers the screen, false when the
// page shows no picture, and null when the window covers the screen only in
// part or when no report of the last 3 seconds tells.
export function noisePictureOn(report) {
  if (!report) return null;
  const { noise, width, height, screenWidth, screenHeight } = report;
  if (noise !== true) return false;
  return width >= NOISE_COVERS.width * screenWidth && height >= NOISE_COVERS.height * screenHeight ? true : null;
}
// usbEmulation leaves USB emulation off for this long.
const USB_OFF_MS = 2000;
// slowKeys clicks the keys of HID_KEYS in turn this often: each click is the
// page's first message to the Mini after more than a second without one.
export const SLOW_KEY_PERIOD_MS = 1500;
// The mouse of hid is a hand that moves for a second and rests for a second.
// While it moves, the page sends 30 moves per second, one every 33 ms. The
// Mini's own page sends one move per frame of the browser's screen, 60 per
// second on a normal screen; the walk sends half of that, which is less
// work for the machine that runs the tool and for the Mini. Each move has
// its own place: 1000 places in turn from left to right, 24 of the 32768
// steps of the screen's width apart, so a place comes again after 67
// seconds. The host reports the place of each move, so the tool knows which
// of its moves has arrived.
const MOVES_PER_SECOND = 30;
const MOVE_PERIOD_MS = 1000 / MOVES_PER_SECOND;
const HAND_MOVES_MS = 1000;
const HAND_RESTS_MS = 1000;
const MOUSE_PLACES = { first: 4000, step: 24, count: 1000 };
const MOUSE_Y = 16000;

// The file that upload sends to the Mini's TF card. The upload is stopped
// before it ends, so the size only has to outlast a hold.
const UPLOAD_BYTES = 1024 * 1024 * 1024;
const UPLOAD_PREFIX = 'combined-load-';
// The image on the TF card that tfMountCdrom and tfMountDisk mount: 64
// blocks of 1 MiB. A run of the tool whose list mounts it uploads it before
// the list, with content that no run before had, so that what the host
// reads from it is what this run's upload put on the card.
const TF_IMAGE = 'combined_load_blocks.img';
const TF_IMAGE_BLOCKS = 64;
const TF_IMAGE_BYTES = TF_IMAGE_BLOCKS * 1024 * 1024;

// The host's watch prints the Mini's media once per second. On a Windows
// host one pass of it takes up to a second while the host reads a medium, so
// two lines can be 2 s apart. A watch whose latest line is older than this is
// behind: it does not say what the host has now.
const WATCH_BEHIND_MS = 3000;
// A watch that is silent for this long leaves the host's drives unknown.
const WATCH_GONE_MS = 30_000;

/**
 * Whether the host has a medium of the Mini of this type, from the host's
 * watch. watch.medium: the medium of the watch's latest line, null for none.
 * watch.at: the time of that line. watch.ended: the watch's SSH session has
 * closed. Null while the watch is behind: a change that waits for the medium
 * is then not finished. Throws when the watch has ended or is silent for
 * WATCH_GONE_MS.
 */
export function mediumOnHost(watch, type, remoteHost) {
  const silentMs = Date.now() - watch.at;
  if (watch.ended || silentMs >= WATCH_GONE_MS) throw new Error(`${remoteHost} does not report the Mini's drives`);
  if (silentMs >= WATCH_BEHIND_MS) return null;
  return watch.medium !== null && watch.medium.type === type;
}

// How long the browser may take from the cloud's sign-in page to the name
// page. For the registration the Mini waits up to 10 s for its clock, and
// exchanges the token and checks the login with up to two TLS handshakes.
const ADOPTION_MS = 30_000;

/**
 * The adoption of the Mini, as a user's browser does it. page: the tool's
 * page, which is on the device page and leaves it. signInPage: the address
 * that the "Adopt" button of the Mini's settings page opens: the cloud's
 * sign-in page, with the address of the Mini's /adopt page to return to. The
 * cloud sends the browser to that page with a token and a login. The page,
 * which is the Mini's UI, asks the Mini for its cloud's address and gives
 * the Mini the token and the login (POST /cloud/register), and then goes to
 * the name page of the cloud's app. isNamePage(pathname) says whether a path
 * is that page's.
 *
 * Returns the times of what the browser saw: adoptAt, its request for the
 * /adopt page; registerAt, the page's request for the registration;
 * registeredAt, the Mini's answer to it; and namePageAt, the browser's
 * arrival at the name page. Throws when the Mini refuses the
 * registration, with the Mini's answer, and when the browser is not at the
 * name page after timeoutMs.
 */
export async function adopt(page, signInPage, isNamePage, timeoutMs = ADOPTION_MS) {
  const times = { adoptAt: null, registerAt: null, registeredAt: null, namePageAt: null };
  const pathOf = address => new URL(address).pathname;
  let refuse;
  const refused = new Promise((resolve, reject) => { refuse = reject; });
  const onRequest = request => {
    if (request.isNavigationRequest() && pathOf(request.url()) === '/adopt') times.adoptAt ??= Date.now();
    else if (pathOf(request.url()) === '/cloud/register') times.registerAt ??= Date.now();
  };
  const onResponse = async response => {
    if (pathOf(response.url()) !== '/cloud/register') return;
    times.registeredAt = Date.now();
    if (response.ok()) return;
    const body = await response.text().catch(() => '');
    refuse(new Error(`the Mini did not take the adoption: HTTP ${response.status()} ${body.slice(0, 200)}`.trimEnd()));
  };
  page.on('request', onRequest);
  page.on('response', onResponse);
  try {
    // One limit for the whole way: the wait for the name page has what the
    // way to the /adopt page left of it.
    const end = Date.now() + timeoutMs;
    const arrival = (async () => {
      await page.goto(signInPage, { waitUntil: 'commit', timeout: timeoutMs });
      await page.waitForURL(address => isNamePage(address.pathname), { waitUntil: 'commit', timeout: Math.max(1, end - Date.now()) });
      times.namePageAt = Date.now();
    })().catch(error => {
      throw new Error(`the browser did not get from the cloud's sign-in page to its name page; it is at ${page.url()}: ${error.message.split('\n')[0]}`);
    });
    // The one that loses the race has nobody else to take its error.
    arrival.catch(() => undefined);
    refused.catch(() => undefined);
    await Promise.race([arrival, refused]);
  } finally {
    page.off('request', onRequest);
    page.off('response', onResponse);
  }
  if (times.adoptAt === null || times.registeredAt === null) {
    throw new Error(`the browser is at the cloud's name page, ${page.url()}, without ` +
      (times.adoptAt === null ? 'a request for the Mini\'s /adopt page' : 'an answer of the Mini to the registration'));
  }
  return times;
}

// The level of the Mini's sound in the browser, 0 to 1, from which audio
// works (levelBetween() in audio.mjs). The target page's tone and beeps
// have about 0.15 at the full volume of the Mini's output on the host, and
// silence that the Mini sends has 0.
const AUDIO_HEARD_LEVEL = 0.001;

// The release bucket of OS Services, which the Mini also reads. releases.json
// lists the stable releases, oldest first, each with the path, size and
// SHA-256 of its packages.
export const RELEASES_URL = 'https://os-services.jetkvm.com';

/**
 * A sender package of one's own in place of the newest release, for a run
 * that measures a build of the sender: JETKVM_SENDER_URL is where the
 * host downloads it (http or https) and JETKVM_SENDER_SHA256 its SHA-256.
 * Both or neither. Returns null without them, else { url, sha256, file },
 * file being the last part of the URL's path. Throws when only one is set,
 * the SHA-256 is not 64 hex digits, the URL is not http(s) with a file, or
 * the file name has a character other than letters, digits and . _ + -.
 */
export function senderOverride(env) {
  const { JETKVM_SENDER_URL: url, JETKVM_SENDER_SHA256: sum } = env;
  if (!url && !sum) return null;
  if (!url || !sum) throw new Error('JETKVM_SENDER_URL and JETKVM_SENDER_SHA256 go together: set both for a sender of your own, or neither for the newest release');
  if (!/^[0-9a-f]{64}$/i.test(sum)) throw new Error(`JETKVM_SENDER_SHA256 must be the 64 hex digits of the package's SHA-256, not "${sum}"`);
  let parsed;
  try {
    parsed = new URL(url);
  } catch {
    throw new Error(`JETKVM_SENDER_URL is not a URL: "${url}"`);
  }
  const file = decodeURIComponent(parsed.pathname.split('/').pop());
  if (!/^https?:$/.test(parsed.protocol) || !file) throw new Error(`JETKVM_SENDER_URL must be an http or https URL of the package file, not "${url}"`);
  // The host's install command has the name inside double quotes.
  if (!/^[\w.+-]+$/.test(file)) throw new Error(`the file name in JETKVM_SENDER_URL may have only letters, digits and . _ + -, not "${file}"`);
  return { url, sha256: sum.toLowerCase(), file };
}

/**
 * Throws when the package of senderOverride() is not the type that the host
 * installs (senderSuffix in hosts.mjs). remoteHost names the host.
 */
export function checkSenderType(sender, host, remoteHost) {
  if (sender && !sender.file.endsWith(host.senderSuffix)) {
    throw new Error(`JETKVM_SENDER_URL names ${sender.file}, but ${remoteHost} installs a ${host.senderPackage} package, a file whose name ends in ${host.senderSuffix}`);
  }
}

/**
 * The package that osServices installs: the one of senderOverride() when
 * there is one, else the host's package of the newest release in
 * releases(), the parsed releases.json. Returns { url, sha256, file,
 * recorded }, recorded being what the recording's first row gets as
 * osServicesSender: the release's version, or "custom <first 12 digits of
 * the SHA-256> <url>", the URL without user, password, query or fragment,
 * which can hold credentials.
 */
export async function senderToInstall(sender, host, releases) {
  if (sender) {
    const { origin, pathname } = new URL(sender.url);
    return { ...sender, recorded: `custom ${sender.sha256.slice(0, 12)} ${origin}${pathname}` };
  }
  const release = (await releases()).releases.at(-1);
  const found = release.packages[host.senderPackage];
  if (!found) throw new Error(`release ${release.version} of the sender has no package ${host.senderPackage}`);
  return { url: `${RELEASES_URL}/${found.path}`, sha256: found.sha256, file: found.path.split('/').pop(), recorded: release.version };
}

// The time between the answer to an update check and the next check. A user
// checks once, when the settings page opens its update page, and again by
// hand; the Mini by itself checks once an hour. So a state with a mount has
// one check at its start and one more for each 15 s that it is held.
const UPDATE_CHECK_PAUSE_MS = 15_000;
// The same in a state without a mount, which is a series of checks for
// memoryLostBytes (judge.mjs): what one check loses is too little to see, so
// the state has many: 5 s after each answer gives 8 checks in 40 s.
const UPDATE_CHECK_SERIES_PAUSE_MS = 5000;

/**
 * The switch of updateCheck: update checks one after the other, each
 * pauseMs() after the answer to the one before. check() is one
 * check and rejects when the Mini refuses it or does not answer. On works
 * when the Mini has answered a check. The sample has the checks answered and
 * failed so far, the message of the newest failure, and as `checks` each
 * check that ended since the sample before, [startedAt, ms, error]: its
 * start, its time to the answer or to the failure, and the failure's
 * message, null for an answer. The judge counts the failed checks from them.
 */
export function updateCheckSwitch(check, pauseMs = () => UPDATE_CHECK_PAUSE_MS) {
  let checks = null;
  return {
    on: async () => {
      const current = { answered: 0, failed: 0, error: null, stopped: false, ended: [] };
      checks = current;
      (async () => {
        while (!current.stopped) {
          const startedAt = Date.now();
          await check().then(() => {
            current.answered++;
            current.ended.push([startedAt, Date.now() - startedAt, null]);
          }, error => {
            current.failed++;
            current.error = error.message;
            current.ended.push([startedAt, Date.now() - startedAt, error.message]);
          });
          // In short steps, so that a switch that went off ends its wait.
          const pause = pauseMs();
          for (const end = Date.now() + pause; !current.stopped && Date.now() < end;) await sleep(Math.min(250, pause));
        }
      })();
    },
    off: async () => {
      if (checks) checks.stopped = true;
      checks = null;
    },
    isOn: () => checks !== null,
    works: on => !on || checks.answered > 0 ||
      (checks.error ? `the Mini has answered no update check: ${checks.error}` : 'the Mini has answered no update check'),
    sample: () => checks && { answered: checks.answered, failed: checks.failed, error: checks.error, checks: checks.ended.splice(0) },
  };
}

// The size that hostModeChange sets. Every bench host offers it for the
// Mini's monitor, and the rules name its height.
const OTHER_SIZE = { width: 1280, height: 720 };

/** A host's displayMode output, "width height rate", as { width, height, hz }. */
export function parseSize(output) {
  const [width, height, hz] = output.trim().split(/\s+/).map(Number);
  if (!(width > 0 && height > 0 && hz > 0)) throw new Error(`the host did not print its display mode: "${output.trim()}"`);
  return { width, height, hz };
}

/**
 * What hostModeChange does on a host whose display has the mode `from`:
 * { from, to }. `to` has no rate, so the host takes the one nearest to its
 * own; `from` has its rate, so the way back ends in the mode before.
 */
export function otherMode(from) {
  if (from.width === OTHER_SIZE.width && from.height === OTHER_SIZE.height) {
    throw new Error(`the host's display is at ${from.width}x${from.height}, the mode that hostModeChange sets; give it another mode or run with --without=hostModeChange`);
  }
  return { from, to: OTHER_SIZE };
}

/**
 * page: a Playwright page that the tool owns. url: the Mini. remoteHost: the
 * SSH login of the host, the computer that the Mini's USB and HDMI connect
 * to; hosts.mjs has what the tool runs there.
 * firmwareFile: the firmware image that the Mini runs, for ota. cloudKeysUrl:
 * where the Mini's build fetches the fake cloud's keys, for cloud.
 * plainCloud: the fake cloud is http and ws, not https and wss.
 * sender: the package of senderOverride(), or null for the newest release;
 * a package that the host does not install is an error here.
 * tailscaleKeyFile: the file with a Tailscale auth key, for tailscale.
 */
export async function createSwitches({ page, url, remoteHost, firmwareFile, cloudKeysUrl, plainCloud = false, sender = null, tailscaleKeyFile }) {
  const firmware = firmwareFile ? readFirmware(firmwareFile) : null;
  const image = await serve(await addressTowards(new URL(url).hostname), firmware);
  const run = promisify(execFile);
  // The commands are POSIX sh; the login shell of the host can be another one.
  const onHost = command => [...SSH_OPTIONS, remoteHost, `sh -c ${quote(command)}`];
  // A failed command reports what it wrote to stderr, not the SSH command line.
  const hostExec = async (command, timeoutMs = 30_000) => {
    try {
      return (await run('ssh', onHost(command), { timeout: timeoutMs })).stdout;
    } catch (e) {
      throw new Error(`${remoteHost}: ${e.stderr?.trim() || e.message}`);
    }
  };
  // The host's operating system decides the commands.
  const host = hostFor(await hostExec('uname -s'));
  checkSenderType(sender, host, remoteHost);

  // The target page on the host's screen. The host's browser fetches it
  // from the address this machine has towards the host, which can be on
  // another network than the Mini. The page reports once per second; it is
  // shown when a report of the last 3 seconds has drawn frames, which a
  // browser does only for a window that is on the screen.
  const pageAddress = await addressTowards(remoteHost.split('@').pop()).catch(() => new URL(image.base).hostname);
  const pageUrl = `http://${pageAddress}:${image.port}/target-page`;
  const pageShown = () => {
    const report = image.served.pageReport;
    return report && Date.now() - report.at < 3000 && report.framesPerSecond > 0 ? report : null;
  };
  // Opens the page unless it is shown, and returns what it shows, for the
  // recording's first row: the size of its window in the screen's pixels,
  // the frames it drew in its last second, and the name of the Mini's audio
  // output in its browser, which is the only output it plays its sound on,
  // or null without one.
  const showPage = async () => {
    let report = pageShown();
    if (!report) {
      await hostExec(host.openPage(pageUrl), 60_000);
      // Only a report of the new page counts: the page before it is closed.
      image.served.pageReport = null;
      // Its first second has the browser's start in it.
      await until(() => (report = pageShown())?.seconds >= 2, 30_000,
        `${remoteHost} does not show the target page ${pageUrl} 30 s after its browser started`);
    }
    const { width, height, pictureWidth, pictureHeight, framesPerSecond, audioOutput } = report;
    if (width < pictureWidth || height < pictureHeight) {
      throw new Error(`the window of the target page is ${width}x${height} on ${remoteHost}, and its moving part is ${pictureWidth}x${pictureHeight}`);
    }
    return { targetPage: { width, height, framesPerSecond, audioOutput } };
  };

  // The page's sound, which audio turns on. A switch runs a change that
  // takes the Mini off USB through sound.offUsb(): the page has reported
  // its sound off before the change, and plays again after it.
  const pageNow = () => {
    const report = image.served.pageReport;
    return report && Date.now() - report.at < 3000 ? report : null;
  };
  const sound = pageSound({ want: image.wantSound, playing: () => pageNow()?.sound ?? null });

  const rpc = pageRpc(page);
  // The page keeps the Mini's messages of the keys that are down, for hid
  // and slowKeys, from each load on, and holds each key until the Mini
  // lists it.
  await page.addInitScript(watchKeyStates, KEY_HOLD_MS);

  // The Mini's stats, or null when the Mini does not answer in 3 seconds. A
  // busy Mini answers late, which the rules judge; the switch that asked
  // asks again.
  const stats = () => fetch(new URL('/webrtc/stats?bitrate=1', url), { signal: AbortSignal.timeout(3000) })
    .then(response => response.json()).catch(() => null);

  // The Mini's video source: "physical" (HDMI capture), "guest" (service
  // video from the sender) or a switch between them; null without an answer.
  // A firmware without OS Services reports none, which is HDMI capture.
  const videoSource = async () => {
    const now = await stats();
    return now && (now.guestVideo?.source ?? 'physical');
  };

  // The Mini's medium on the host, from the latest line of the host's watch, and
  // when the host's read count last grew. mounted: the variable that mounted it.
  let cdrom = null;
  let cdromAt = 0;
  let watchEnded = false;
  let readAt = 0;
  let mounted = null;
  let pending = '';
  const watch = spawn('ssh', onHost(host.watchMedia), { stdio: ['ignore', 'pipe', 'ignore'] });
  watch.stdout.setEncoding('utf8');
  watch.stdout.on('data', chunk => {
    const lines = (pending + chunk).split('\n');
    pending = lines.pop();
    for (const line of lines) {
      const fields = line.trim().split(/\s+/);
      const now = fields[0] === 'none' ? null : host.medium(fields);
      if (now && cdrom && now.device === cdrom.device && now.bytesRead > cdrom.bytesRead) readAt = Date.now();
      cdrom = now;
      cdromAt = Date.now();
    }
  });
  watch.on('close', () => { watchEnded = true; });
  const hostReports = () => Date.now() - cdromAt < WATCH_BEHIND_MS;

  // hid: the keys sent, the presses the host has seen, and of the keys that
  // the tool judged since the sample before, those without the Mini's answer
  // and the slowest answer (hid_times.mjs). For the mouse: the moves that the
  // page sent and the host's reports of a move. Which keys and moves the
  // host did not get, and the longest time between two moves on the host,
  // the judge takes from the events of the samples (hostInputAt() in
  // hid_times.mjs). tickLagMaxMs is how late the tool's own timer was at
  // most.
  const newKeys = () => ({ sent: 0, received: 0, unanswered: 0, latencyMaxMs: null,
    mouseSent: 0, mouseReceived: 0, tickLagMaxMs: null });
  const keys = newKeys();
  // The time of each key sent, of each judged key, of each move and of each
  // line of the host's watch since the sample before, for the judge. While
  // no sample takes them, the newest 2000 are kept.
  const sends = [];
  const keySends = [];
  const moveSends = [];
  const reports = [];
  const keep = (list, entry) => {
    list.push(entry);
    if (list.length > 2000) list.shift();
  };
  let tickAt = 0;
  let answers = keyAnswers();
  let keyTimer = null;
  // packetLoss: Chrome's network emulation for the page, through the page's
  // DevTools session, which the first use opens.
  let devTools = null;
  const loss = packetLossSwitch({
    emulate: async conditions => {
      devTools ??= await page.context().newCDPSession(page);
      await devTools.send('Network.emulateNetworkConditions', conditions);
    },
    video: () => videoCounts(),
  });
  let keyWatch = null;
  let keyWatchAt = 0;
  let keyReceivedAt = 0;
  let pressing = false;
  const watchKeys = () => {
    keyWatch = spawn('ssh', onHost(host.watchKeys), { stdio: ['ignore', 'pipe', 'ignore'] });
    keyWatch.stdout.setEncoding('utf8');
    let rest = '';
    keyWatch.stdout.on('data', chunk => {
      keyWatchAt = Date.now();
      const lines = (rest + chunk).split('\n');
      rest = lines.pop();
      for (const line of lines) {
        // A line is "p<usage>", "m<x> <ms>" or "."; <ms> is the host's
        // time of the move.
        const report = parseReport(line, keyWatchAt);
        if (!report) continue;
        keep(reports, report);
        const [what] = report;
        if (what.startsWith('p')) {
          keys.received++;
          keyReceivedAt = keyWatchAt;
        } else if (what.startsWith('m')) {
          keys.mouseReceived++;
        }
      }
    });
  };
  // Starts the hand in the page, which sends the moves by itself.
  const startMouse = () => page.evaluate(moveHand, [MOVE_PERIOD_MS, HAND_MOVES_MS, HAND_RESTS_MS, MOUSE_PLACES, MOUSE_Y]);
  // In the page, which keeps the Mini's messages of the keys that are down
  // (watchKeyStates in hid_times.mjs): presses the key, when there is one,
  // which the page releases when the Mini's message lists it or after
  // KEY_HOLD_MS, and returns the time of the press, null without one, the
  // messages since the call before, and with takeMoves the moves that the
  // page's hand has sent since the call before.
  const click = (usage, takeMoves) => page.evaluate(([usage, takeMoves]) => {
    const at = usage === null ? null : window.__combinedLoadKeys.press(usage);
    return { at, states: window.__combinedLoadKeyStates?.splice(0) ?? [], moved: (takeMoves && window.__combinedLoadMouse?.take()) || [] };
  }, [usage, takeMoves]).catch(() => ({ at: null, states: [], moved: [] }));
  const releaseKeys = () => page.evaluate(() => window.__combinedLoadKeys?.releaseAll()).catch(() => undefined);
  // slowKeys: its timer, its counts, and the keys that it judged since the
  // sample before.
  const slow = { timer: null, answers: keyAnswers(), clickedAt: 0, ticking: false, sent: 0, unanswered: 0, latencyMaxMs: null, answered: 0, judged: [] };
  // The Mini's messages go to the keys of hid and of slowKeys; each takes
  // those of its own keys.
  const toAnswers = states => {
    answers.states(states);
    slow.answers.states(states);
  };
  // Judges the keys of hid that are answered or old enough.
  const judgeKeys = now => {
    const judged = answers.judge(now);
    for (const key of judged) {
      keep(keySends, key);
      if (key[2] === null) keys.unanswered++;
      else keys.latencyMaxMs = Math.max(keys.latencyMaxMs ?? 0, key[2]);
    }
  };
  // Judges the keys of slowKeys that are answered or old enough.
  const judgeSlowKeys = now => {
    for (const key of slow.answers.judge(now)) {
      keep(slow.judged, key);
      const latencyMs = key[2];
      if (latencyMs === null) slow.unanswered++;
      else {
        slow.answered++;
        slow.latencyMaxMs = Math.max(slow.latencyMaxMs ?? 0, latencyMs);
      }
    }
  };
  // Takes the Mini's messages from the page, and clicks the next key when
  // SLOW_KEY_PERIOD_MS have passed since the click before.
  const slowTick = async () => {
    if (slow.ticking) return;
    slow.ticking = true;
    // A tick that comes a little early still clicks, so the keys are
    // SLOW_KEY_PERIOD_MS apart and not one tick more.
    const due = Date.now() - slow.clickedAt >= SLOW_KEY_PERIOD_MS - KEY_PERIOD_MS / 2;
    const usage = due ? HID_KEYS[slow.sent % HID_KEYS.length].usage : null;
    if (due) {
      slow.clickedAt = Date.now();
      slow.sent++;
    }
    const { at, states } = await click(usage, false);
    slow.ticking = false;
    if (due) slow.answers.send(usage, at ?? slow.clickedAt);
    toAnswers(states);
    judgeSlowKeys(Date.now());
  };
  const keyTick = async () => {
    const lag = Date.now() - tickAt - KEY_PERIOD_MS;
    if (tickAt) keys.tickLagMaxMs = Math.max(keys.tickLagMaxMs ?? 0, lag);
    tickAt = Date.now();
    if (pressing) return;
    pressing = true;
    const { usage } = HID_KEYS[keys.sent % HID_KEYS.length];
    keys.sent++;
    // The time before the click, so that the host's report cannot come first.
    keep(sends, [usage, tickAt]);
    const { at, states, moved } = await click(usage, true);
    pressing = false;
    // A click that the page did not take counts as a key without an answer.
    answers.send(usage, at ?? tickAt);
    toAnswers(states);
    judgeKeys(Date.now());
    keys.mouseSent += moved.length;
    for (const move of moved) keep(moveSends, move);
  };

  // upload: the request in progress.
  let upload = null;
  // The Wi-Fi Mini can take longer than an RPC's time to answer after an
  // upload was cut off, so the list is asked for up to three times.
  const removeUploads = async () => {
    const list = () => rpc('listStorageFiles');
    // A Mini without a TF card has no files to remove.
    const noCard = error => { if (/storage unavailable/.test(error.message)) return { files: [] }; throw error; };
    const { files } = await list().catch(list).catch(list).catch(noCard);
    for (const { filename } of files.filter(file => file.filename.startsWith(UPLOAD_PREFIX))) {
      await rpc('deleteStorageFile', { filename }).catch(() => undefined);
    }
  };

  // The page's counts for the Mini's video so far: the frames it has
  // decoded, the packets it counts as lost, and its requests to the Mini to
  // send packets again. null without a video stream.
  const videoCounts = () => page.evaluate(async () => {
    const pc = window.__kvmTestHooks?._getPeerConnection?.();
    let counts = null;
    (await pc?.getStats())?.forEach(report => {
      if (report.type === 'inbound-rtp' && report.kind === 'video') {
        counts = { framesDecoded: report.framesDecoded, packetsLost: report.packetsLost, nackCount: report.nackCount };
      }
    });
    return counts;
  }).catch(() => null);
  const framesDecoded = async () => (await videoCounts())?.framesDecoded ?? null;
  // The frame counts of the last second, one per call of video.works().
  let frameHistory = [];

  const switches = {
    video: {
      on: () => page.goto(url, { waitUntil: 'domcontentloaded' }),
      off: () => page.goto('about:blank'),
      isOn: () => page.evaluate(() => Boolean(window.__kvmTestHooks?.isWebRTCConnected?.() &&
        window.__kvmTestHooks?.isVideoStreamActive?.())).catch(() => false),
      // The browser decoded at least 25 frames in the last second: the
      // picture is back at its normal rate, not only at its first frame.
      works: async on => {
        if (!on) return true;
        const frames = await framesDecoded();
        const now = Date.now();
        frameHistory = frameHistory.filter(entry => now - entry.t < 1300);
        const before = frameHistory.find(entry => now - entry.t >= 1000);
        frameHistory.push({ t: now, frames });
        if (frames === null || before === undefined || before.frames === null) return 'the browser has no count of decoded frames for the last second';
        const rate = (frames - before.frames) / ((now - before.t) / 1000);
        return rate >= 25 || `the browser decoded ${rate.toFixed(0)} frames in the last second, and 25 make it work`;
      },
    },

    // An OS Services session: the sender on the host runs and is connected
    // to the Mini. Off is a stopped sender.
    osServices: {
      // The newest stable sender, or the package of senderOverride(),
      // installed for this run with the Mini's key approved, so every run
      // has the same sender and no dialog.
      prepare: async () => {
        const { recorded, ...install } = await senderToInstall(sender, host, async () =>
          (await fetch(`${RELEASES_URL}/releases.json`, { signal: AbortSignal.timeout(30_000) })).json());
        const fingerprint = (await rpc('getVideoState')).osServices?.fingerprint;
        if (!fingerprint) throw new Error('the Mini reports no OS Services key');
        await hostExec(host.installSender({ ...install, fingerprint }), 600_000);
        return { osServicesSender: recorded };
      },
      // The session without its video: the Mini keeps HDMI capture. The size
      // and the display mode of service video are the Mini's defaults, which
      // the run's start has set (settings.mjs).
      on: async () => {
        await rpc('setOsServicesSettings', { settings: {
          video_enabled: false, terminal_enabled: true, files_enabled: true, clipboard_enabled: true,
        } });
        await hostExec(host.startSender);
      },
      off: () => hostExec(host.stopSender),
      isOn: async () => (await rpc('getVideoState')).osServices?.connected === true,
      // The session offers the terminal and takes files.
      works: async on => {
        if (!on) return true;
        const state = await rpc('getVideoState');
        return state.guestTerminal === true && state.guestFile?.receive === true;
      },
    },

    // The sender's service video as the video source; off is HDMI capture.
    osVideo: {
      on: async () => {
        // Off and on clears the Mini's limit on switches to service video.
        await rpc('setOsServicesSettings', { settings: { video_enabled: false } });
        await rpc('setOsServicesSettings', { settings: { video_enabled: true } });
      },
      off: () => rpc('setOsServicesSettings', { settings: { video_enabled: false } }),
      // Neither on nor off during a switch between the sources, or without
      // an answer.
      isOn: async () => ({ guest: true, physical: false })[await videoSource()],
      // On or off, it is a switch of the video source: the browser decodes
      // frames from the new source.
      works: () => switches.video.works(true),
    },

    ...osServicesParts({ page, call: pageCall(page), hostExec, host }),

    // Keys and the mouse, through the Mini's HID: every 100 ms a click of
    // the next key of HID_KEYS, and a hand that moves the mouse for a
    // second, with 30 moves, and rests for a second.
    hid: {
      on: async () => {
        if (!keyWatch) watchKeys();
        // The watch's beat comes once per second.
        const start = Date.now();
        while (Date.now() - keyWatchAt > 2000) {
          if (Date.now() - start > 10_000) throw new Error(`${remoteHost} does not report its key presses`);
          await new Promise(resolve => setTimeout(resolve, 100));
        }
        Object.assign(keys, newKeys());
        sends.length = 0;
        keySends.length = 0;
        moveSends.length = 0;
        reports.length = 0;
        tickAt = 0;
        answers = keyAnswers();
        await startMouse();
        keyTimer = setInterval(keyTick, KEY_PERIOD_MS);
      },
      off: async () => {
        clearInterval(keyTimer);
        keyTimer = null;
        await page.evaluate(() => window.__combinedLoadMouse?.stop()).catch(() => undefined);
        await releaseKeys();
      },
      isOn: () => keyTimer !== null,
      // The host has seen a press in the last half second, and a move.
      works: on => !on || (Date.now() - keyReceivedAt < 500 && keys.mouseReceived > 0),
      // The keys that are answered or old enough are judged here as well,
      // once per sample. latencyMaxMs is the slowest answer of the Mini since
      // the sample before, and tickLagMaxMs the tool's latest tick. sends,
      // keys, moves and reports have the times of that second's sent keys,
      // judged keys and sent moves and of the lines of the host's watch, from
      // which the judge counts the keys and the moves that the host did not
      // get (hid_times.mjs).
      sample: () => {
        judgeKeys(Date.now());
        const sample = { ...keys, sends: sends.splice(0), keys: keySends.splice(0), moves: moveSends.splice(0), reports: reports.splice(0) };
        keys.latencyMaxMs = null;
        keys.tickLagMaxMs = null;
        return sample;
      },
    },

    // One key every 1.5 s, and nothing else from the page: a session in
    // which the user types now and then. Each key is the first message on
    // the session's data channels after more than a second, which is where
    // a connection that has gone quiet shows a late first message. The keys
    // are those of HID_KEYS in turn, clicked as by hid. The Mini's message that lists the key as down
    // is the answer, as for hid; the host's key watch is not used. The tool
    // takes the Mini's messages from the page every key period of hid,
    // which sends nothing to the Mini.
    slowKeys: {
      on: async () => {
        Object.assign(slow, { answers: keyAnswers(), clickedAt: 0, sent: 0, unanswered: 0, latencyMaxMs: null, answered: 0, judged: [] });
        slow.timer = setInterval(slowTick, KEY_PERIOD_MS);
        // The first key comes at once: the change is finished when the Mini has answered one.
        slowTick();
      },
      off: async () => {
        clearInterval(slow.timer);
        slow.timer = null;
        await releaseKeys();
      },
      isOn: () => slow.timer !== null,
      works: async on => {
        if (!on) return true;
        await slowTick();
        return slow.answered > 0;
      },
      // The keys so far, those without the Mini's answer, and the slowest
      // answer since the sample before, as in hid's sample, and the keys
      // that the tool judged since the sample before, from which the key
      // latency is judged.
      sample: () => {
        judgeSlowKeys(Date.now());
        const sample = { sent: slow.sent, unanswered: slow.unanswered, latencyMaxMs: slow.latencyMaxMs, keys: slow.judged.splice(0) };
        slow.latencyMaxMs = null;
        return sample;
      },
    },

  };

  // Sends `bytes` bytes of a file to the Mini's upload session `uploadId`:
  // those of `content`, or without it a text again and again. onRequest gets
  // the request, so that the caller can stop it.
  const postUpload = (uploadId, bytes, onRequest = () => {}, content = null) => new Promise((resolve, reject) => {
    const request = http.request(new URL(`/storage/upload?uploadId=${encodeURIComponent(uploadId)}`, url), {
      method: 'POST', headers: { 'Content-Type': 'application/octet-stream', 'Content-Length': bytes },
    });
    onRequest(request);
    request.on('error', reject);
    request.on('response', response => {
      response.resume();
      response.on('end', () => resolve(response.statusCode));
    });
    const chunk = Buffer.alloc(64 * 1024, 'combined-load ');
    let queued = 0;
    const write = () => {
      while (queued < bytes && !request.destroyed) {
        const length = Math.min(chunk.length, bytes - queued);
        const part = content ? content.subarray(queued, queued + length) : chunk.subarray(0, length);
        queued += part.length;
        if (!request.write(part)) {
          request.once('drain', write);
          return;
        }
      }
      if (queued >= bytes) request.end();
    };
    write();
  });

  // Before the Mini unmounts an image: the reader stops, and a host that
  // keeps a medium which the Mini unmounts gives the medium up. Returns the
  // media that such a host still has, as its releaseMedia prints them.
  const kept = out => out.trim().replace(/\s+/g, ', ');
  // then: a command for the host after the stop, in the same SSH session.
  const stopReading = async (then = 'true') => {
    // The stop can end its shell early, so it has a shell of its own.
    if (!host.releaseMedia) {
      await hostExec(`(${host.stopReader}); ${then}`);
      return '';
    }
    return kept(await hostExec(`(${host.stopReader}); ${then}; ${host.releaseMedia}`));
  };
  // The image on the TF card of this run: the checksums of its blocks, and
  // whether the host's reader is to forget which of them it has read, which
  // it is after an upload and after a restart of the Mini. A wrong block
  // then counts as wrong at its first read: what is on the card is not what
  // was sent, or did not stay through the restart.
  const tf = { sums: null, forget: true };
  // A medium that the host has kept goes when the Mini enumerates again,
  // which a mount makes it do. The host can give up the mounted image. The
  // kept medium can stay for some seconds beside the new one.
  const enumerateAgain = async before => {
    await rpc('mountWithHTTP', { url: image.url, mode: 'Disk' }, 60_000);
    await until(() => hostReports() && cdrom?.type === 'disk', 30_000,
      `${remoteHost} keeps ${before} of the Mini without a medium, and has no disk 30 s after a new mount`);
    let still = before;
    await until(async () => !(still = kept(await hostExec(host.releaseMedia))), 30_000,
      () => `${remoteHost} keeps ${still} of the Mini without a medium, also after a new mount`);
    await rpc('unmountImage', {}, 30_000).catch(() => undefined);
  };

  // A mounted image that the host reads. type: what the host sees, "rom" or
  // "disk". mount: the RPC that mounts it. prepare: what it needs on the Mini.
  // onCard: the image is the one on the TF card, not the one from the URL.
  // The host's reader checks each block that it reads. Its counts start at
  // zero with each mount, in the command that stops the reader before.
  const mountSwitch = (name, type, mount, prepare, onCard = false) => ({
    prepare: async () => {
      if (host.canRead) await hostExec(host.canRead);
      return prepare?.();
    },
    // A mount and an unmount can take the Mini off USB.
    on: async () => {
      const expect = onCard
        ? host.expectBlocks(tf.sums ?? [], { firstReads: true, forget: tf.forget })
        : host.expectBlocks(IMAGE_SUMS);
      if (onCard) tf.forget = false;
      await stopReading(expect);
      await sound.offUsb(async () => {
        await rpc('unmountImage').catch(() => undefined);
        await mount();
      });
      mounted = name;
      await hostExec(host.startReader);
    },
    off: async () => {
      const stillThere = await stopReading();
      await sound.offUsb(async () => {
        await rpc('unmountImage', {}, 30_000).catch(() => undefined);
        if (stillThere) await enumerateAgain(stillThere);
      });
      if (mounted === name) mounted = null;
    },
    // The host has a medium of this type. A medium that no variable of this
    // run mounted counts for each of them, so that the clean start waits
    // until it is gone. Null while the host's watch is behind.
    isOn: async () => {
      const there = mediumOnHost({ medium: cdrom, at: cdromAt, ended: watchEnded }, type, remoteHost);
      return there === null ? null : there && (mounted === name || mounted === null);
    },
    // The host has read from the medium in the last 3 seconds; a pass over
    // the image ends in a second without a read.
    works: on => !on || Date.now() - readAt < 3000,
    // The bytes that the host has read; the reader's counts since the
    // mount: the blocks that it checked, and those with a wrong checksum
    // at their first read and at a later read; and, on a Linux host, the
    // kernel's count of the commands that the medium answered with an error.
    sample: () => {
      if (!hostReports() || !cdrom) return null;
      const { bytesRead, blocksChecked, blocksWrongFirst, blocksWrongLater, readErrors } = cdrom;
      return { bytesRead, blocksChecked, blocksWrongFirst, blocksWrongLater, ...(readErrors === undefined ? {} : { readErrors }) };
    },
  });

  // The image on the TF card, uploaded once in a run of the tool, before the
  // list, in place of the image of the run before. Its blocks have seeds
  // from the time of the upload, in seconds. Returns how long the upload
  // took, for the recording's first row.
  let tfUpload = null;
  const tfImage = () => (tfUpload ??= (async () => {
    for (const filename of [TF_IMAGE, `${TF_IMAGE}.incomplete`]) await rpc('deleteStorageFile', { filename }).catch(() => undefined);
    const image = imageBytes(Math.floor(Date.now() / 1000), TF_IMAGE_BLOCKS);
    const start = Date.now();
    const { dataChannel } = await rpc('startStorageFileUpload', { filename: TF_IMAGE, size: TF_IMAGE_BYTES });
    const status = await postUpload(dataChannel, TF_IMAGE_BYTES, undefined, image);
    if (status !== 200) throw new Error(`the upload of ${TF_IMAGE} ended with HTTP ${status}`);
    tf.sums = blockChecksums(image);
    tf.forget = true;
    return { tfImageUploadSeconds: Math.round((Date.now() - start) / 100) / 10 };
  })());
  // The image from a URL, which the tool serves, as the CD-ROM and as a disk.
  switches.httpMountCdrom = mountSwitch('httpMountCdrom', 'rom',
    () => rpc('mountWithHTTP', { url: image.url, mode: 'CDROM' }, 60_000));
  switches.httpMountDisk = mountSwitch('httpMountDisk', 'disk',
    () => rpc('mountWithHTTP', { url: image.url, mode: 'Disk' }, 60_000));
  // The same image from the tool's HTTPS URL, as a disk. Each read of the
  // host is then a request on the Mini's TLS connection to the tool, which
  // the Mini admits or refuses by its free memory. The recorder has the
  // Mini's counts of those requests in every row (memoryCounters).
  let secureUrl = null;
  switches.httpsMountDisk = mountSwitch('httpsMountDisk', 'disk',
    () => rpc('mountWithHTTP', { url: secureUrl, mode: 'Disk' }, 60_000),
    async () => {
      secureUrl = (await image.secure()).url;
      return {};
    });
  // The image on the TF card, as the CD-ROM and as a disk.
  switches.tfMountCdrom = mountSwitch('tfMountCdrom', 'rom',
    () => rpc('mountWithStorage', { filename: TF_IMAGE, mode: 'CDROM' }, 60_000), tfImage, true);
  switches.tfMountDisk = mountSwitch('tfMountDisk', 'disk',
    () => rpc('mountWithStorage', { filename: TF_IMAGE, mode: 'Disk' }, 60_000), tfImage, true);

  // A file upload to the Mini's TF card, at the rate the Mini takes it.
  switches.upload = {
    prepare: async () => {
      const { bytesFree } = await rpc('getStorageSpace');
      if (!(bytesFree >= UPLOAD_BYTES)) throw new Error(`the TF card has ${bytesFree} bytes free; the upload needs ${UPLOAD_BYTES}`);
      return {};
    },
    on: async () => {
      await removeUploads();
      const filename = `${UPLOAD_PREFIX}${Date.now()}.img`;
      const { dataChannel } = await rpc('startStorageFileUpload', { filename, size: UPLOAD_BYTES });
      const current = { request: null, error: null };
      upload = current;
      postUpload(dataChannel, UPLOAD_BYTES, request => { current.request = request; }).then(
        status => { current.error = `the upload ended with HTTP ${status}`; },
        error => { current.error = error.message; });
    },
    off: async () => {
      upload?.request?.destroy();
      upload = null;
      await removeUploads();
    },
    isOn: () => upload !== null && upload.error === null,
    // On: the Mini has taken the first megabyte. Off: video is at its normal
    // rate, because a Mini that pauses video for an upload starts it again.
    works: on => (on ? (upload?.request?.socket?.bytesWritten ?? 0) > 1024 * 1024 : switches.video.works(true)),
    sample: () => (upload ? { bytesSent: upload.request?.socket?.bytesWritten ?? 0, error: upload.error } : null),
  };

  // Another display mode on the host, set as its display settings set it and
  // not stored: the Mini's capture loses its signal, finds the new mode and
  // is built again. On is the host in the other mode, off the host back in
  // the mode that it had before the list.
  let hostMode = null;      // { from, to }, from prepare()
  let hostModeSet = false;  // this run has set the other mode
  const setHostMode = size => hostExec(host.displayMode(size), 60_000);
  switches.hostModeChange = {
    prepare: async () => {
      if (!host.displayMode) {
        throw new Error(`the tool sets no display mode on a ${host.system} host; run with --without=hostModeChange`);
      }
      hostMode = otherMode(parseSize(await setHostMode()));
      return { hostMode };
    },
    on: async () => {
      hostModeSet = true;
      await setHostMode(hostMode.to);
    },
    // Also the cleanup: only a host that this run changed goes back.
    off: async () => {
      if (!hostModeSet) return;
      await setHostMode(hostMode.from);
      hostModeSet = false;
    },
    // What the Mini captures, as its page shows it (getVideoState): neither
    // on nor off while the capture has no signal or another size. While an
    // upload runs, the Mini's page shows the upload and no picture, so the
    // change is the host's alone: the host has taken the mode.
    isOn: async () => {
      if (!hostMode) return false;
      if (upload) return hostModeSet;
      const now = await rpc('getVideoState').catch(() => null);
      if (!now?.ready) return null;
      const has = size => now.width === size.width && now.height === size.height;
      return has(hostMode.to) ? true : has(hostMode.from) ? false : null;
    },
    // The picture is back: the browser decodes at the normal rate. No
    // picture is asked for while an upload runs; the end of the upload asks
    // for it (upload.works).
    works: () => (upload ? true : switches.video.works(true)),
    sample: () => (hostModeSet ? { ...hostMode.to } : null),
  };

  // An update check, again and again: getUpdateStatus, the call of "Check
  // for updates" on the settings page. The Mini asks its update server,
  // which is the real one outside ota, over a new TLS connection each time.
  // Beside a mount the checks come as a user's, and without one as a series.
  switches.updateCheck = updateCheckSwitch(() => rpc('getUpdateStatus', {}, 10_000),
    () => (mounted ? UPDATE_CHECK_PAUSE_MS : UPDATE_CHECK_SERIES_PAUSE_MS));

  // The stream quality changed as the video settings change it: on is the
  // preset Medium, off the walk's own value, to which the run's start has
  // set the Mini (settings.mjs). The Mini's encoder gets another bitrate
  // without a new capture or a new session, and the picture goes on.
  switches.qualityChange = {
    on: () => rpc('setStreamQualityFactor', { factor: QUALITY_MEDIUM }),
    off: () => rpc('setStreamQualityFactor', { factor: QUALITY_STANDING }),
    isOn: async () => (await rpc('getStreamQualityFactor')) === QUALITY_MEDIUM,
    works: () => switches.video.works(true),
  };

  // A still picture with the fine detail of a photo on the host's screen,
  // behind the target page's moving part (target_page.html): what the Mini
  // or the sender has to encode when the user's desktop is a photo. On when
  // the page reports that it shows the picture.
  switches.noisePicture = {
    on: async () => { image.served.pageNoise = true; },
    off: async () => { image.served.pageNoise = false; },
    // On: the page has drawn the picture for a window that has the screen's
    // width and, less the bars of the window and of the desktop, its
    // height. A window that the host does not let grow shows the picture on
    // a part of the screen only, and the change is then not finished. A page
    // without a recent report finishes neither change.
    isOn: () => noisePictureOn(pageNow()),
    // The picture has reached the browser: video is at its normal rate.
    works: () => switches.video.works(true),
    // The page's window and the host's screen, in the screen's pixels: the
    // window has the screen's size while it shows the picture.
    sample: () => {
      const { width, height, screenWidth, screenHeight } = pageNow() ?? {};
      return width === undefined ? null : { width, height, screenWidth, screenHeight };
    },
  };

  // USB emulation turned off and on again, as the switch "Enable USB
  // Emulation" of the settings page does it, with USB_OFF_MS between the
  // two. The Mini leaves the host's USB and comes back: the sender's session
  // ends and starts again, the host loses a mounted medium and gets it
  // again, and keys reach nobody in between. On is the whole way; the
  // change is finished when what was there before is back: the picture at
  // its normal rate, the sender's session, the host's reads of the mounted
  // image, and a key on the host.
  let usbCycle = null;
  switches.usbEmulation = {
    on: async () => {
      const cycle = usbCycle = { back: false, session: await switches.osServices.isOn().catch(() => false), mount: mounted, keys: keyTimer !== null };
      // The host's reader ends with its medium: it is stopped before and
      // started after, as around a mount.
      if (cycle.mount) await stopReading();
      await sound.offUsb(async () => {
        await rpc('setUsbEmulationState', { enabled: false });
        await sleep(USB_OFF_MS);
        await rpc('setUsbEmulationState', { enabled: true });
      });
      if (cycle.mount) {
        await until(async () => (await switches[cycle.mount].isOn()) === true, 30_000,
          `${remoteHost} does not have the Mini's medium again 30 s after USB emulation came back on`);
        await hostExec(host.startReader);
      }
      cycle.back = true;
    },
    // Also the cleanup: a Mini that a stopped run left off USB goes back on.
    off: async () => {
      usbCycle = null;
      if ((await rpc('getUsbEmulationState')) !== true) await rpc('setUsbEmulationState', { enabled: true });
    },
    isOn: () => usbCycle !== null && usbCycle.back,
    works: async on => {
      if (!on) return true;
      const video = await switches.video.works(true);
      if (video !== true) return video;
      if (usbCycle.session && !(await switches.osServices.isOn().catch(() => false))) return 'the sender has no session after USB emulation came back';
      if (usbCycle.mount && Date.now() - readAt >= 3000) return 'the host has not read the mounted image in the last 3 s';
      if (usbCycle.keys && Date.now() - keyReceivedAt >= 500) return 'the host has seen no key in the last half second';
      return true;
    },
  };

  // The sender on the host restarted, as when its user quits and starts the
  // program or the host's session starts again. Its session with the Mini
  // ends, and with it service video; the next session brings both back. On
  // is the restart; the change is finished when the session is connected
  // and, with service video on, the picture comes from the sender again at
  // its normal rate.
  let senderRestarted = false;
  switches.senderRestart = {
    on: async () => {
      senderRestarted = false;
      await hostExec(host.stopSender, 60_000);
      await hostExec(host.startSender, 60_000);
      senderRestarted = true;
    },
    off: async () => { senderRestarted = false; },
    isOn: async () => senderRestarted && await switches.osServices.isOn().catch(() => false),
    works: async on => {
      if (!on) return true;
      const wanted = (await rpc('getOsServicesSettings').catch(() => null))?.video_enabled;
      if (wanted && (await videoSource()) !== 'guest') return 'the picture does not come from the sender yet';
      return switches.video.works(true);
    },
  };

  // Every packet between the page and the Mini lost for 100 ms, once
  // (packet_loss.mjs).
  switches.packetLoss = loss;

  // The session taken by a second tab of the tool's browser and taken back
  // with the first tab's "Use Here" button (takeover.mjs). The second tab is
  // a page of the same browser context, as a user's second tab is. Its
  // signaling sockets are read from its DevTools session, which is attached
  // before the tab loads the Mini's page.
  const useHereButton = () => page.getByRole('button', { name: 'Use Here' });
  // The signaling sockets of a tab from now on.
  const watchSockets = async tab => {
    const signaling = signalingSockets();
    const session = await page.context().newCDPSession(tab);
    for (const name of SOCKET_EVENTS) session.on(name, params => signaling.event(name, params, Date.now()));
    await session.send('Network.enable');
    return { sockets: signaling.sockets, end: () => session.detach().catch(() => undefined) };
  };
  let firstTabSockets = null;
  switches.takeover = takeoverSwitch({
    openTab: async () => {
      const tab = await page.context().newPage();
      const signaling = await watchSockets(tab);
      return {
        load: () => tab.goto(url, { waitUntil: 'commit', timeout: 30_000 }),
        sockets: signaling.sockets,
        peers: () => tab.evaluate(peerConnections).catch(() => null),
        firstFrame: timeoutMs => tab.evaluate(firstFrame, timeoutMs),
        close: () => tab.close(),
      };
    },
    first: {
      // The page's sockets are read for the time of the state only.
      watch: async () => {
        await firstTabSockets?.end();
        firstTabSockets = await watchSockets(page);
      },
      endWatch: async () => {
        await firstTabSockets?.end();
        firstTabSockets = null;
      },
      sockets: () => firstTabSockets?.sockets() ?? [],
      peers: () => page.evaluate(peerConnections).catch(() => null),
      useHereShown: timeoutMs => useHereButton().waitFor({ state: 'visible', timeout: timeoutMs }).then(() => Date.now(), () => null),
      // The time of the click is the page's: the browser clicks a button
      // when it has stopped moving, which can be later than the call.
      useHere: async () => {
        await page.evaluate(() => {
          window.__combinedLoadClick = new Promise(resolve => {
            document.addEventListener('click', () => resolve(Date.now()), { capture: true, once: true });
          });
        });
        await useHereButton().click({ timeout: 5000 });
        return page.evaluate(() => window.__combinedLoadClick);
      },
      firstFrame: timeoutMs => page.evaluate(firstFrame, timeoutMs),
      has: async () => ({ video: await switches.video.isOn(), useHere: await useHereButton().isVisible().catch(() => false) }),
    },
    // The first tab's session is back when its video is at its normal rate
    // and, with audio on, the browser plays the Mini's sound again.
    sessionBack: async () => {
      const video = await switches.video.works(true);
      const sound = listening ? await switches.audio.works(true) : true;
      return video !== true ? video : sound;
    },
  });

  const diagnostics = () => readDiagnostics(url);

  // A firmware update while the rest runs: the Mini downloads the image it
  // already runs from the tool's update server, over HTTPS as from the real
  // one, installs it and restarts.
  // On is finished when the Mini is back with video, after a restart that
  // the firmware asked for, on the version of the image. The Mini has no
  // way to stop an update, so off only sets the update server's address: it
  // is always the real server after a run, also after one that was killed.
  // An update ends a mount, because the Mini restarts, and the terminal and
  // the file transfer, because the page loads again: the driver turns them
  // off when the update's switch is done.
  let update = null;
  let uptimeMs = null;
  switches.ota = {
    ends: ['osTerminal', 'osTerminalLight', 'osFiles', 'osFilesSmall', ...MOUNTS],
    prepare: async () => {
      if (!firmware) throw new Error('the list has ota; pass --image=<the firmware image that the Mini runs>');
      const running = (await diagnostics()).firmware;
      if (running.elfSha256 !== firmware.elfSha256) {
        throw new Error(`the Mini runs ${running.version}; ${firmwareFile} is ${firmware.version}` +
          (running.version === firmware.version ? ', another build of it' : ''));
      }
      return { firmware: running.version };
    },
    on: async () => {
      const current = { restarted: false, reloaded: false, checked: false, startedAt: Date.now(), refused: null };
      update = current;
      uptimeMs = null;
      // The Mini restarts: what the host reads from the card after that is
      // a first read again.
      tf.forget = true;
      // Over HTTPS, as from the real server.
      await rpc('setUpdateApiUrl', { url: (await image.secure()).base });
      // The restart takes the Mini off USB: the page's sound is off until
      // the update works.
      await sound.quiet();
      // The Mini can restart before its answer arrives, so the answer is not
      // waited for. An error counts only when no restart follows it.
      rpc('tryUpdate', {}, 30_000).catch(error => { current.refused = error.message; });
    },
    off: async () => {
      await rpc('setUpdateApiUrl', { url: UPDATE_SERVER });
      update = null;
      sound.resume();
    },
    // The Mini has restarted since the update started: its clock went back.
    isOn: async () => {
      if (!update) return false;
      const now = (await stats())?.nowMs ?? null;
      if (now !== null && uptimeMs !== null && now < uptimeMs) update.restarted = true;
      if (now !== null) uptimeMs = now;
      if (!update.restarted && update.refused && Date.now() - update.startedAt > 60_000) {
        throw new Error(`the Mini did not start the update: ${update.refused}`);
      }
      return update.restarted;
    },
    works: async on => {
      if (!on) return true;
      if (!update.reloaded) {
        // The page can start a load of its own when the Mini is back, which
        // interrupts this one; the next round loads it again.
        update.reloaded = await page.goto(url, { waitUntil: 'domcontentloaded' }).then(() => true, () => false);
        return false;
      }
      if (!(await switches.video.isOn())) return 'the page has no video after the restart';
      const video = await switches.video.works(true);
      if (video !== true) return video;
      if (!update.checked) {
        const now = await diagnostics();
        if (now.resetReason !== ESP_RST_SW) throw new Error(`the Mini restarted with reset reason ${now.resetReason}, not ${ESP_RST_SW} (a restart by the firmware)`);
        if (now.firmware.elfSha256 !== firmware.elfSha256) throw new Error(`the Mini runs ${now.firmware.version} after the update, and not the image, ${firmware.version}`);
        update.checked = true;
      }
      sound.resume();
      return true;
    },
    // downloading: the Mini has a connection for the firmware image open.
    // Over TLS the bytes are counted when they are encrypted, which is at
    // once, so they do not say how far the download is.
    sample: () => ({ bytesServed: image.served.firmwareBytes, downloading: image.served.firmwareDownloads > 0 }),
  };

  // The host's sound through the Mini to the browser. On: the Mini's audio
  // setting is on, the target page plays its tone and its beeps into the
  // Mini's USB audio output, and the tool's page plays the Mini's audio
  // track. The UI attaches that track when a page loads with audio enabled,
  // and its settings page loads the page again after the setting changed;
  // the tool attaches it itself (and mutes its own element while the UI
  // plays the track too), in the session that the other variables
  // use. Off: the page is silent and the setting is off, which is the
  // Mini's default.
  const pageClock = beepClock();
  image.served.onPageReport = pageClock.report;
  const hearNow = take => page.evaluate(hearScript(take)).catch(() => null);
  let beepsHeard = 0;
  // Whether the list has audio on, for takeover.
  let listening = false;
  // The browser's reports of the last second, one per call of audio.works().
  let heardHistory = [];
  switches.audio = {
    prepare: async () => {
      if ((await rpc('getUsbDevices')).audio === false) {
        throw new Error('the list has audio; the Mini\'s USB devices have no audio device (its setting "USB device")');
      }
      // Every host plays into the Mini's output at full volume, so the
      // level in the browser is the level of the target page's sound.
      if (host.fullVolume) await hostExec(host.fullVolume);
      return {};
    },
    on: async () => {
      await rpc('setAudioConfig', { params: { enabled: true } });
      beepsHeard = 0;
      heardHistory = [];
      sound.turn(true);
      listening = true;
    },
    off: async () => {
      sound.turn(false);
      listening = false;
      // The Mini refuses the setting while it changes its USB devices, as
      // during a mount.
      const end = Date.now() + 15_000;
      for (;;) {
        try {
          await rpc('setAudioConfig', { params: { enabled: false } });
          return;
        } catch (e) {
          if (!/in progress/.test(e.message) || Date.now() > end) throw e;
          await sleep(500);
        }
      }
    },
    isOn: async () => (await rpc('getAudioConfig')).enabled === true,
    // On: the page plays on the Mini's output, and the browser decoded sound
    // above silence in the last second. Off: the page has reported its
    // sound off.
    works: async on => {
      const shown = pageNow();
      if (!on) return shown?.sound === false || 'the target page has not reported its sound off';
      if (!shown?.audioOutput) return `the browser of the target page on ${remoteHost} has no audio output of the Mini`;
      if (!shown.sound) return 'the target page does not play its sound on the Mini\'s audio output';
      const heard = await hearNow(false);
      const now = Date.now();
      heardHistory = heardHistory.filter(entry => now - entry.t < 1300);
      const before = heardHistory.find(entry => now - entry.t >= 1000);
      heardHistory.push({ t: now, heard });
      const level = levelBetween(heard, before?.heard);
      if (level === undefined) return 'the browser has no report of the Mini\'s sound for the last second';
      return level >= AUDIO_HEARD_LEVEL ||
        `the browser decoded sound at a level of ${level.toFixed(4)} in the last second, and ${AUDIO_HEARD_LEVEL} makes it work`;
    },
    // The browser's report of the Mini's audio track; pageSound, whether
    // the target page plays; beeps, the beeps heard since audio came on;
    // beepsAt, the time at which each beep heard since the sample before
    // started, on this machine's clock; beepClock, the clocks of the target
    // page and of the host by which the judge takes each beep's delay from
    // the host's output to the browser (beepClock() in audio.mjs), null
    // before the page's first report with a clock; and tones, the strongest
    // frequency of each window of sound since the sample before.
    sample: async () => {
      const heard = await hearNow(true);
      if (!heard) return null;
      const { beeps, ...received } = heard;
      beepsHeard += beeps.length;
      return { ...received, pageSound: pageNow()?.sound ?? null, beeps: beepsHeard,
        beepsAt: beeps.map(at => Math.round(at * 10) / 10), beepClock: pageClock.now() };
    },
  };

  // The Mini adopted to a cloud and connected to it. The cloud is the tool's
  // own (fake_cloud.mjs), so that the tool can adopt the Mini without a
  // login. On is what a user does (adopt()): the cloud's address by RPC, as
  // the "Adopt" button of the settings page sets it; then the browser leaves
  // the device page, which ends its session, for the cloud's sign-in page,
  // comes to the Mini's /adopt page, which registers the Mini, and ends at
  // the cloud's name page. The tool then loads the device page again, and
  // the browser's session is local as before. The cloud is https and wss,
  // so the Mini does a TLS handshake for the keys (once in a run), for the
  // exchange of the token and for its WebSocket, and one more for the
  // WebSocket each time it connects again. Off is the "Deregister" button
  // of the settings page, the RPC deregisterDevice in the open session: it
  // removes the Mini from whatever cloud it is on, with one more handshake.
  // The tool then sets the real cloud's address: after a run the Mini is
  // not adopted.
  let fakeCloud = null;
  // The times of the newest adoption (adopt()), and where the page is after
  // it: at the cloud's name page, on the device page without its session
  // yet, or, with null, in its session.
  let adoption = null;
  let afterAdoption = null;
  // The device page that the tool loads again after an adoption.
  let returnUrl = url;
  switches.cloud = {
    prepare: async () => {
      if (!cloudKeysUrl) throw new Error('the list has cloud; the bench file must set JETKVM_MOCK_CLOUD_JWKS_URL');
      const { version } = (await diagnostics()).firmware;
      if (!version.includes('mockcloud')) throw new Error(`the Mini runs ${version}, which is not a build for the fake cloud (-DJETKVM_MOCK_CLOUD_JWKS_URL)`);
      fakeCloud ??= startFakeCloud(cloudKeysUrl, { tls: !plainCloud });
      return { cloud: fakeCloud.url };
    },
    on: async () => {
      await rpc('setCloudUrl', { apiUrl: fakeCloud.url, appUrl: fakeCloud.url });
      const deviceId = await rpc('getDeviceID');
      adoption = null;
      afterAdoption = null;
      // The device page at the host the page is on now: the tailnet address
      // while tailscale is on, else the LAN address.
      returnUrl = new URL(new URL(url).pathname, page.url()).href;
      try {
        adoption = { ...await adopt(page, fakeCloud.signInPage(deviceId, url), fakeCloud.isNamePage), socketAt: null };
      } catch (e) {
        throw new Error(e.message + (fakeCloud.problem() ? ` (${fakeCloud.problem()}; run.sh installs a build for this cloud)` : ''));
      }
      afterAdoption = 'name page';
    },
    off: async () => {
      // The cleanup has the device page in its session.
      afterAdoption = null;
      await rpc('deregisterDevice').catch(() => undefined);
      await rpc('setCloudUrl', CLOUD);
    },
    // What the Mini says; with the fake cloud's address set, that is the
    // only cloud it can be on. After an adoption the browser is at the
    // cloud's name page: the device page is loaded again first, as after an
    // update, and the Mini is asked when the page has its new session. Until
    // then the switch cannot tell, and the change is not finished.
    isOn: async () => {
      if (afterAdoption === 'name page' && await page.goto(returnUrl, { waitUntil: 'domcontentloaded' }).then(() => true, () => false)) {
        afterAdoption = 'device page';
      }
      if (afterAdoption === 'device page' && await switches.video.isOn()) afterAdoption = null;
      if (afterAdoption) return null;
      return (await rpc('getCloudState')).connected;
    },
    // On: the fake cloud has the Mini's WebSocket, and the browser's session
    // has video at its normal rate again.
    works: async on => {
      if (!on) return true;
      if (!fakeCloud.connected()) return 'the cloud does not have the Mini\'s WebSocket';
      if (!(await switches.video.isOn())) return 'the page has no video after the adoption';
      return switches.video.works(true);
    },
    // The cloud's view of the connection: the Mini's own answer is whether
    // it is adopted, which a lost connection does not change. With it, the
    // TLS handshakes of the Mini so far and how long the last one took, and
    // the times of the newest adoption: the browser's request for the
    // Mini's /adopt page (adoptAt), that page's request for the
    // registration (registerAt), the Mini's answer to it (registeredAt), the
    // browser's arrival at the name page (namePageAt), and the cloud's
    // acceptance of the Mini's WebSocket (socketAt).
    // socketAt is the first acceptance after that request.
    sample: () => {
      if (adoption && adoption.socketAt === null && fakeCloud.socketAt() >= adoption.adoptAt) adoption.socketAt = fakeCloud.socketAt();
      return { connected: fakeCloud?.connected() ?? false, ...fakeCloud?.handshakes(), ...adoption };
    },
  };

  // mqtt: the Mini's connection to the tool's MQTT broker, which the Mini
  // reaches at the address that serves the images (mqtt.mjs).
  switches.mqtt = mqttSwitch({ rpc, stats, address: new URL(image.base).hostname });

  // tailscale: the Mini on a tailnet with the auth key of tailscaleKeyFile,
  // and the page's session over it (tailscale.mjs).
  switches.tailscale = tailscaleSwitch({ page, url, rpc, keyFile: tailscaleKeyFile });

  // In the order of VARIABLES, which is the order the driver uses.
  const ordered = Object.fromEntries(VARIABLES.map(name => [name, switches[name]]));
  // The end of the tool's run: the page leaves the host's screen.
  const close = async () => {
    watch.kill();
    keyWatch?.kill();
    await hostExec(host.closePage).catch(error => console.error(`cleanup: ${error.message}`));
    image.close();
    fakeCloud?.close();
  };
  // The Mini's stored settings at their defaults, and, for a list that has a
  // mount, its drive a CD-ROM drive; what was found, for the recording's
  // first row. It needs RPC, so video is on. The drive comes last: a drive
  // that was a disk makes the Mini enumerate on USB again, before the list
  // starts. A list without a mount does not need the drive, and runs also
  // on a Mini whose USB configuration has no mass storage.
  const usbDrive = () => cdromDrive({
    enumerations: async () => (await fetch(new URL('/webrtc/stats?details=1', url), { signal: AbortSignal.timeout(10_000) })
      .then(response => response.json())).usbHid?.mountCount,
    mount: async () => {
      await switches.httpMountCdrom.prepare();
      await switches.httpMountCdrom.on();
      await until(switches.httpMountCdrom.isOn, 60_000, `${remoteHost} has no CD-ROM of the Mini 60 s after the mount before the list`);
    },
    unmount: async () => {
      await switches.httpMountCdrom.off();
      await until(async () => (await switches.httpMountCdrom.isOn()) === false, 60_000,
        `${remoteHost} keeps the Mini's CD-ROM 60 s after the unmount before the list`);
    },
  });
  const storedSettings = async ({ drive }) => ({ ...await setDefaults(rpc), ...(drive ? { usbDrive: await usbDrive() } : {}) });
  return { switches: ordered, host: host.system, hostExec, hostReports, showPage, storedSettings, close };
}
