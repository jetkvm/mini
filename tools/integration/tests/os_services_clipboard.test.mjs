import assert from 'node:assert/strict';
import { after, before, describe, test } from 'node:test';
import { hostClipboard } from '../lib/host_clipboard.mjs';
import { connect, until } from '../lib/mini.mjs';
import { connected, connectedWithClipboard, openSession, rpcAnswer, videoState } from '../lib/os_services.mjs';
import { senderHost } from '../lib/sender_host.mjs';

// The longest clipboard text the OS Services protocol allows, in bytes of UTF-8.
const MAX_BYTES = 65528;
const INTERNAL_ERROR = -32603;
const INVALID_PARAMS = -32602;
const SHARING_OFF = { code: INTERNAL_ERROR, data: 'clipboard sharing is turned off in the OS Services settings' };
// Characters of 2, 3 and 4 bytes, combining marks, right-to-left text, tabs and line ends.
const UNICODE_TEXT =
  'JetKVM ✓ Grüße — naïve café\tñ\n' +
  '日本語のテキスト 中文 한국어\n' +
  'emoji 🙂👍🏽🇩🇪 family 👨‍👩‍👧 math 𝔸𝕓 ∑∫\n' +
  'עברית العربية Ελληνικά Кириллица\n' +
  'é ä zero​width "quotes" \\backslash\\ <tag> & \'single\'';

const bytes = text => Buffer.byteLength(text, 'utf8');
// Exactly `count` bytes of ASCII in lines, other ones for each `seed`.
const asciiText = (count, seed) => {
  let text = '';
  for (let n = 0; text.length < count; n++) text += `${seed} ${String(n).padStart(5, '0')} ${'abcdefghijklmnopqrstuvwxyz0123456789'.repeat(2)}\n`;
  return text.slice(0, count);
};
// Fails with the sizes and the place of the first difference, not with two texts of 64 KiB.
const assertSameText = (actual, expected, what) => {
  if (actual === expected) return;
  let at = 0;
  while (at < Math.min(actual.length, expected.length) && actual[at] === expected[at]) at++;
  assert.equal(actual.slice(at, at + 40), expected.slice(at, at + 40),
    `${what}: ${bytes(actual)} bytes in place of ${bytes(expected)}, other from character ${at}`);
};

// The clipboard that a session of OS Services shares between the Mini's user
// and the host, and the OS Services settings that turn it off. The first
// test reads and writes the clipboard of the host's desktop, on a Linux or
// macOS host with a logged-in desktop.
describe('the OS Services clipboard', () => {
  let mini;
  let host;
  let session;

  before(async () => {
    mini = await connect();
    try {
      host = await senderHost(mini);
      session = await openSession(mini, host);
    } catch (error) {
      await mini.close().catch(() => undefined);
      throw error;
    }
  }, { timeout: 300_000 });

  after(async () => {
    try {
      await session?.restore();
    } finally {
      await mini.close();
    }
  }, { timeout: 300_000 });

  const settings = () => mini.rpc('getOsServicesSettings');
  const failure = async (method, params) => {
    const { error, result } = await rpcAnswer(mini, method, params);
    assert.ok(error, `${method} must fail; its answer was ${JSON.stringify(result)}`);
    return { code: error.code, data: error.data };
  };
  // A notification of `method` since `since` that passes check, within 10 s.
  const notified = (method, since, check) => until(() => mini.timedEvents(method),
    events => events.some(event => event.at >= since && check(event.params)), 10);

  test('text crosses both ways between the Mini and the host\'s clipboard, and clipboard sharing off withdraws it', { timeout: 300_000 }, async t => {
    const clipboard = await hostClipboard(host);
    if (!clipboard) return t.skip(`${host.login} cannot read its desktop's clipboard (a Linux host needs python3 with Gtk 3 and a logged-in desktop)`);
    const { copy, paste } = clipboard;
    const found = await paste();
    // The host's clipboard has the text again that it had.
    t.after(() => (found === '' ? clipboard.clear() : copy(found)));
    const offer = { read: true, write: true, os: host.system };
    const get = () => mini.rpc('guestClipboardGet', {}, 15_000);
    const set = text => mini.rpc('guestClipboardSet', { text }, 15_000);
    const roundTrip = async label => {
      const out = `${label}: to the host ${Date.now()} ✓`;
      assert.deepEqual(await set(out), { status: 'ok' });
      assert.equal(await paste(), out, `${label}: the host pastes the Mini's text`);
      const back = `${label}: from the host ${Date.now()} ✓`;
      await copy(back);
      assert.deepEqual(await get(), { status: 'ok', text: back, truncated: false }, `${label}: the Mini reads the host's text`);
    };

    assert.deepEqual((await until(() => videoState(mini), connectedWithClipboard, 30)).guestClipboard, offer, 'the sender offers reading and writing');

    // To the host.
    for (const [what, text] of [
      ['Unicode', UNICODE_TEXT],
      ['65,528 bytes of 4-byte characters', '🙂'.repeat(MAX_BYTES / 4)],
      ['65,528 bytes of ASCII', asciiText(MAX_BYTES, 'to-host')],
    ]) {
      // Another text on the host first, so that a set without effect fails.
      await copy(`before ${what} ${Date.now()}`);
      assert.deepEqual(await set(text), { status: 'ok' }, what);
      assertSameText(await paste(), text, `${what}, pasted on the host`);
    }
    let marker = `unchanged ${Date.now()}`;
    await copy(marker);
    const over = asciiText(MAX_BYTES + 1, 'over');
    assert.deepEqual(await failure('guestClipboardSet', { text: over }),
      { code: INTERNAL_ERROR, data: `clipboard text exceeds ${MAX_BYTES} bytes` }, 'one byte over the limit');
    assert.equal(await paste(), marker, 'the refused text did not reach the host');

    // From the host.
    for (const [what, text] of [['Unicode', UNICODE_TEXT], ['65,528 bytes of ASCII', asciiText(MAX_BYTES, 'from-host')]]) {
      await copy(text);
      const read = await get();
      assert.deepEqual({ status: read.status, truncated: read.truncated }, { status: 'ok', truncated: false }, what);
      assertSameText(read.text, text, `${what}, read through the Mini`);
    }
    // 21,843 characters of 3 bytes are 65,529 bytes; 21,842 of them fit.
    const fits = Math.floor(MAX_BYTES / 3);
    await copy('€'.repeat(fits + 1));
    const cut = await get();
    assert.deepEqual({ status: cut.status, truncated: cut.truncated }, { status: 'ok', truncated: true }, 'a longer text');
    assertSameText(cut.text, '€'.repeat(fits), 'the longer text, cut between two characters');
    await clipboard.clear();
    assert.deepEqual(await get(), { status: 'empty', text: '', truncated: false }, 'an empty clipboard');

    // Sharing off and on.
    const before = await settings();
    assert.equal(before.clipboard_enabled, true, 'sharing is on at first');
    marker = `kept on the host ${Date.now()}`;
    await copy(marker);
    const offAt = Date.now();
    // A field that is null counts as left out.
    const off = await mini.rpc('setOsServicesSettings', { settings: { clipboard_enabled: false, resolution: null } });
    assert.deepEqual(off, { ...before, clipboard_enabled: false }, 'only clipboard_enabled changed');
    assert.deepEqual(await settings(), off, 'the settings as stored');
    const withdrawn = await until(() => videoState(mini), state => connected(state) && state.guestClipboard === undefined, 15);
    assert.equal(withdrawn.osServices.clipboard, false);
    assert.deepEqual(await failure('guestClipboardGet'), SHARING_OFF);
    assert.deepEqual(await failure('guestClipboardSet', { text: 'must not reach the host' }), SHARING_OFF);
    assert.equal(await paste(), marker, 'the host\'s clipboard is as it was');
    await notified('osServicesSettings', offAt, params => JSON.stringify(params.settings) === JSON.stringify(off));
    await notified('videoInputState', offAt, params => params.osServices !== undefined && connected(params) && params.guestClipboard === undefined);

    const onAt = Date.now();
    assert.deepEqual(await mini.rpc('setOsServicesSettings', { settings: { clipboard_enabled: true } }), before);
    assert.deepEqual((await until(() => videoState(mini), connectedWithClipboard, 15)).guestClipboard, offer, 'the clipboard is offered again');
    await notified('videoInputState', onAt, params => params.osServices !== undefined && params.guestClipboard !== undefined);
    await roundTrip('sharing on again');
  });

  // A new resolution re-presents the host's display: the sender has no
  // scanout for 2 s or more, and the Mini gives up a clipboard request after
  // 2 s. A request sent while the display is re-presented is answered.
  test('a clipboard request while a new resolution re-presents the display is answered', { timeout: 300_000 }, async () => {
    // Every switch to service video counts against the Mini's limit of
    // switches, which video off and on clears.
    await mini.rpc('setOsServicesSettings', { settings: { video_enabled: false } });
    const registered = await until(() => videoState(mini), now => now.osServices.videoRegistered || now.osServices.videoError !== undefined, 15)
      .catch(() => videoState(mini));
    assert.equal(registered.osServices.videoError, undefined, 'the Mini\'s service video error');
    assert.equal(registered.osServices.videoRegistered, true, 'the sender offers service video');
    const shows = async (width, height) => {
      const state = await until(() => videoState(mini), now => now.osServices.videoError !== undefined ||
        (now.source === 'guest' && now.ready && now.width === width && now.height === height), 60);
      assert.equal(state.osServices.videoError, undefined, `the Mini's service video error at ${width}x${height}`);
    };
    await mini.rpc('setOsServicesSettings', { settings: { video_enabled: true, resolution: '1920x1080' } });
    await shows(1920, 1080);
    await until(() => videoState(mini), connectedWithClipboard, 30);
    for (const [width, height] of [[1280, 720], [1920, 1080]]) {
      await mini.rpc('setOsServicesSettings', { settings: { resolution: `${width}x${height}` } });
      // The sender loses the scanout within about 150 ms of the change; a
      // request sent before that is answered before the re-presentation.
      await new Promise(resolve => setTimeout(resolve, 150));
      assert.deepEqual(await mini.rpc('guestClipboardSet', { text: `during the change to ${width}x${height} ${Date.now()}` }, 15_000),
        { status: 'ok' }, `guestClipboardSet during the change to ${width}x${height}`);
      await shows(width, height);
    }
  });

  test('invalid OS Services settings are refused whole and change nothing', { timeout: 60_000 }, async () => {
    const before = await settings();
    for (const [changed, message] of [
      [{ resolution: '1024x768' }, /^unsupported OS Services resolution/],
      [{ display_mode: 'tile' }, /^unsupported OS Services display mode/],
      [{ adaptation: 'fixed:fast' }, /^invalid adaptation value/],
      [{ clipboard_enabled: 'yes' }, /^clipboard_enabled must be a boolean/],
      // Valid fields beside an invalid one are not stored either.
      [{ clipboard_enabled: !before.clipboard_enabled, terminal_enabled: !before.terminal_enabled, resolution: '800x600' },
        /^unsupported OS Services resolution/],
    ]) {
      const error = await failure('setOsServicesSettings', { settings: changed });
      assert.equal(error.code, INTERNAL_ERROR, JSON.stringify(changed));
      assert.match(error.data, message, JSON.stringify(changed));
      assert.deepEqual(await settings(), before, `the settings after ${JSON.stringify(changed)}`);
    }
    assert.equal((await failure('setOsServicesSettings', { settings: 'all' })).code, INVALID_PARAMS, 'settings that are no object');
    assert.equal(connectedWithClipboard(await videoState(mini)), true, 'the session goes on');
  });
});
