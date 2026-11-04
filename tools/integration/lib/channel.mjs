// Extra data channels on the page's peer connection (OS Services terminal and
// file drops).
import { seededBytes } from '../../combined_load/runner/content.mjs';
import { until } from './mini.mjs';

// Runs in the page.
const open = label => {
  const channel = window.__kvmTestHooks._getPeerConnection().createDataChannel(label);
  channel.binaryType = 'arraybuffer';
  const decoder = new TextDecoder();
  const seen = { channel, output: '', binaryMessages: 0, binaryBytes: 0, binaryAfterText: 0, texts: [] };
  channel.addEventListener('message', event => {
    if (typeof event.data === 'string') {
      seen.texts.push(event.data);
      seen.binaryAfterText = 0;
      return;
    }
    seen.output += decoder.decode(event.data, { stream: true });
    seen.binaryMessages++;
    seen.binaryBytes += event.data.byteLength;
    seen.binaryAfterText++;
  });
  return (window.__integrationOpened ??= []).push(seen) - 1;
};

/**
 * { state, until, text, bytes, close }. state().output is the binary messages
 * decoded as UTF-8; binaryAfterText counts binary messages since the last
 * text message. bytes(text) sends text as a binary message.
 */
export async function openChannel(mini, label) {
  const id = await mini.page.evaluate(open, label);
  const state = () => mini.page.evaluate(index => {
    const { channel, ...seen } = window.__integrationOpened[index];
    return { readyState: channel.readyState, ...seen };
  }, id);
  const send = (message, binary) => mini.page.evaluate(([index, text, encode]) =>
    window.__integrationOpened[index].channel.send(encode ? new TextEncoder().encode(text) : text), [id, message, binary]);
  return {
    state,
    until: (check, seconds) => until(state, check, seconds),
    text: message => send(message, false),
    bytes: text => send(text, true),
    close: () => mini.page.evaluate(index => window.__integrationOpened[index].channel.close(), id),
  };
}

// Runs in the page, as the UI sends a drop: messages are a u32 LE transfer ID
// plus up to 32 KiB, within the Mini's credit; a file with cancelAfter is
// cancelled after that many bytes.
const send = ([label, files, content]) => {
  const bytesOf = (0, eval)(`(${content})`);
  const channel = window.__kvmTestHooks._getPeerConnection().createDataChannel(label);
  const seen = { channel, other: [], transfers: files.map(() => ({ credit: 0, creditMessages: 0, sent: 0 })) };
  const pumps = files.map((file, index) => {
    const bytes = bytesOf(file.seed, file.size);
    const transfer = seen.transfers[index];
    let credit = 0;
    let pumping = false;
    const pump = async () => {
      if (pumping) return;
      pumping = true;
      while (channel.readyState === 'open' && !transfer.status && !transfer.cancelled && credit > 0 && transfer.sent < file.size) {
        if (file.cancelAfter !== undefined && transfer.sent >= file.cancelAfter) {
          transfer.cancelled = true;
          channel.send(JSON.stringify({ type: 'cancel', transfer: file.transfer }));
          break;
        }
        if (channel.bufferedAmount > 96 * 1024) {
          await new Promise(resolve => setTimeout(resolve, 20));
          continue;
        }
        const n = Math.min(32 * 1024, credit, file.size - transfer.sent);
        const message = new Uint8Array(4 + n);
        new DataView(message.buffer).setUint32(0, file.transfer, true);
        message.set(bytes.subarray(transfer.sent, transfer.sent + n), 4);
        channel.send(message);
        transfer.sent += n;
        credit -= n;
      }
      pumping = false;
    };
    return { pump, credit: more => { credit += more; } };
  });
  // The Mini's first credit can arrive before the channel's open event.
  channel.addEventListener('open', () => pumps.forEach(one => void one.pump()));
  channel.addEventListener('message', event => {
    let message = {};
    try {
      message = JSON.parse(event.data);
    } catch {
      // kept in `other` below
    }
    const index = files.findIndex(file => file.transfer === message.transfer);
    const transfer = seen.transfers[index];
    if (transfer && message.type === 'credit') {
      transfer.credit += message.bytes;
      transfer.creditMessages++;
      pumps[index].credit(message.bytes);
      void pumps[index].pump();
    } else if (transfer && message.type === 'done') {
      transfer.status = message.status;
      transfer.path = message.path;
      if (seen.transfers.every(one => one.status)) channel.close();
    } else {
      seen.other.push(typeof event.data === 'string' ? event.data : '<binary>');
    }
  });
  return (window.__integrationOpened ??= []).push(seen) - 1;
};

/**
 * Sends files on the channel named in the Mini's guestFileDrop answer. Each
 * file is { transfer, size, seed, cancelAfter } with seededBytes content.
 * Returns state() → { readyState, transfers, other }.
 */
export async function sendFiles(mini, channel, files) {
  const id = await mini.page.evaluate(send, [channel, files, String(seededBytes)]);
  return () => mini.page.evaluate(index => {
    const { channel: opened, ...seen } = window.__integrationOpened[index];
    return { readyState: opened.readyState, ...seen };
  }, id);
}
