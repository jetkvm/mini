// takeover: a second tab opens the Mini's page, which ends the first tab's
// session (a Mini has one at most); the first tab takes it back with "Use
// Here". The tool only clicks and reads timings; it changes neither page.
import { sleep } from './driver.mjs';

// The page has no time limit for a socket that is still connecting.
export const SOCKET_MS = 10_000;
// How long a tab may take from its load, or from "Use Here", to its first
// decoded frame of video.
export const SESSION_MS = 20_000;
// How long the first tab may take to show "Use Here" after the second tab
// has video.
export const USE_HERE_SHOWN_MS = 5000;

/**
 * In a page: the time, on this machine's clock in ms, at which the page's
 * session decoded its first frame of video, or null when it has none after
 * timeoutMs. The page is asked every 10 ms, and Chrome renews its report of
 * a connection every 50 ms, so the time is late by up to 60 ms. A connection
 * that the page has closed, as after a takeover, does not count.
 *
 * The function uses nothing outside itself: the tool gives its source to
 * the page.
 */
export const firstFrame = timeoutMs => new Promise(resolve => {
  const end = performance.now() + timeoutMs;
  const look = async () => {
    const pc = window.__kvmTestHooks?._getPeerConnection?.();
    let frames = 0;
    if (pc && pc.connectionState !== 'closed') {
      const reports = await pc.getStats().catch(() => null);
      reports?.forEach(report => {
        if (report.type === 'inbound-rtp' && report.kind === 'video') frames = report.framesDecoded ?? 0;
      });
    }
    if (frames > 0) resolve(Date.now());
    else if (performance.now() >= end) resolve(null);
    else setTimeout(look, 10);
  };
  look();
});

/**
 * In a page: how many RTCPeerConnections the page has had, by the calls of
 * this function: each call counts the page's connection of that moment when
 * no call before saw it. A page that makes a new connection for each try
 * of a session has one more at each call that sees a new one.
 *
 * The function uses nothing outside itself: the tool gives its source to
 * the page.
 */
export const peerConnections = () => {
  const peers = window.__combinedLoadTakeoverPeers ??= { seen: new WeakSet(), count: 0 };
  const pc = window.__kvmTestHooks?._getPeerConnection?.();
  if (pc && !peers.seen.has(pc)) {
    peers.seen.add(pc);
    peers.count++;
  }
  return peers.count;
};

/**
 * What the browser did with a tab's signaling WebSockets, from the events
 * of the DevTools protocol (Network.webSocket...). event(name, params, at)
 * takes each event in, with the time at which it reached the tool. sockets()
 * has one entry per socket that the page made for the Mini's signaling path,
 * oldest first: createdAt, the page made the socket; requestAt, the browser
 * sent its request to the Mini; openAt, the Mini accepted it (HTTP 101);
 * answer, the HTTP status of another answer; closedAt; error, what the
 * browser reports for a socket that failed. A time is null for what did not
 * happen.
 */
export function signalingSockets() {
  const sockets = new Map();
  const isSignaling = address => {
    try {
      return new URL(address).pathname.startsWith('/webrtc/signaling');
    } catch {
      return false;
    }
  };
  return {
    event: (name, params, at) => {
      if (name === 'Network.webSocketCreated') {
        if (isSignaling(params.url)) {
          sockets.set(params.requestId, { createdAt: at, requestAt: null, openAt: null, answer: null, closedAt: null, error: null });
        }
        return;
      }
      const socket = sockets.get(params.requestId);
      if (!socket) return;
      if (name === 'Network.webSocketWillSendHandshakeRequest') socket.requestAt ??= at;
      else if (name === 'Network.webSocketHandshakeResponseReceived') {
        if (params.response?.status === 101) socket.openAt ??= at;
        else socket.answer = params.response?.status ?? null;
      } else if (name === 'Network.webSocketClosed') socket.closedAt ??= at;
      else if (name === 'Network.webSocketFrameError') socket.error = params.errorMessage ?? 'an error';
    },
    sockets: () => [...sockets.values()].map(socket => ({ ...socket })),
  };
}
export const SOCKET_EVENTS = ['Network.webSocketCreated', 'Network.webSocketWillSendHandshakeRequest',
  'Network.webSocketHandshakeResponseReceived', 'Network.webSocketClosed', 'Network.webSocketFrameError'];

// What the browser did with the sockets of a tab that has no open one, as a
// text, with each time in ms from `from`: the tab's load, or the click.
function describeSockets(sockets, from) {
  if (sockets.length === 0) return 'the page made no signaling socket';
  const after = at => `${at - from} ms`;
  return sockets.map((socket, index) => (sockets.length > 1 ? `socket ${index + 1}: ` : '') + [
    `the page made the socket after ${after(socket.createdAt)}`,
    socket.requestAt === null ? 'the browser sent no request for it' : `the browser sent its request after ${after(socket.requestAt)}`,
    socket.answer !== null ? `the Mini answered with HTTP ${socket.answer}` : 'no answer came',
    ...(socket.error ? [`the browser reports "${socket.error}"`] : []),
    ...(socket.closedAt === null ? [] : [`the socket closed after ${after(socket.closedAt)}`]),
  ].join(', ')).join('; ');
}

/**
 * The switch of takeover.
 *
 *   openTab()      opens the second tab, empty, and returns
 *                  { load(), sockets(), firstFrame(timeoutMs), close() }:
 *                  load() starts the load of the Mini's page and returns
 *                  when the tab has the page's first bytes; sockets() is
 *                  signalingSockets().sockets() for the tab; firstFrame()
 *                  is firstFrame in the tab; peers() is peerConnections in
 *                  the tab, or null when the tab does not answer
 *   first          the tool's page, the first tab:
 *                  { watch(), sockets(), useHereShown(timeoutMs), useHere(),
 *                  firstFrame(timeoutMs), has(), endWatch() }: watch()
 *                  begins to read the page's signaling sockets, sockets()
 *                  has those made since, and endWatch() ends the reading;
 *                  peers() is peerConnections in the page;
 *                  useHereShown() returns the time at which the
 *                  page shows the "Use Here" button, or null; useHere()
 *                  clicks it and returns the time at which the page got
 *                  the click; has() says what the page has now,
 *                  { video, useHere }
 *   sessionBack()  true when the first tab's session works again, or a text
 *                  that says what it does not do yet
 *
 * On is the whole way: the second tab loads the Mini's page, its signaling
 * socket opens, it decodes its first frame, the first tab shows "Use Here",
 * the tool clicks it, and the first tab decodes its first frame. The second
 * tab stays open, with its own "Use Here" page, as the tab of a user who
 * leaves both open, until the state ends. The change is finished when
 * sessionBack() holds. A step
 * that does not come in its time is an error of the change, with what the
 * browser saw, and the list ends there: the tool does not try again.
 *
 * sample() has the times of the newest takeover, for the recording, each
 * null until it has come: loadAt, the start of the second tab's load;
 * socketMadeAt, socketRequestAt and socketAt, for that tab's signaling
 * socket: the page made it, the browser sent its request, the Mini accepted
 * it; frameAt, the tab's first decoded frame; useHereShownAt, the first tab
 * shows the button; useHereAt, the page got the click; backSocketMadeAt,
 * backSocketRequestAt and backSocketAt, the same three for the first tab's
 * new signaling socket; backAt, the first tab's first decoded frame after
 * the click. socketsMade
 * and backSocketsMade count the signaling sockets that the second tab's
 * page made, and the first tab's page after the click: more than one means
 * that a socket failed and the page made another. newConnections counts
 * what either tab made after the first tab had its session back: signaling
 * sockets, and RTCPeerConnections that a sample saw. It is null until then,
 * and more than 0 means that the tabs take the session from each other.
 */
export function takeoverSwitch({ openTab, first, sessionBack, now = Date.now, wait = sleep }) {
  let times = null;
  let tab = null;
  // The sockets and peer connections of both tabs when the session was
  // back, or null while that is unknown: before, or while a tab did not
  // answer.
  let returned = false;
  let settled = null;
  // Null when a tab does not answer for its peer connections.
  const connections = async () => {
    const mine = await first.peers();
    const other = tab ? await tab.peers() : 0;
    if (mine === null || other === null) return null;
    return { sockets: (await first.sockets()).length + (tab?.sockets().length ?? 0), peers: mine + other };
  };
  const closeTab = async () => {
    const open = tab;
    tab = null;
    await open?.close().catch(() => undefined);
  };
  return {
    on: async () => {
      await closeTab();
      returned = false;
      settled = null;
      times = { loadAt: null, socketMadeAt: null, socketRequestAt: null, socketAt: null, frameAt: null, useHereShownAt: null, useHereAt: null,
        backSocketMadeAt: null, backSocketRequestAt: null, backSocketAt: null, backAt: null, socketsMade: 0, backSocketsMade: 0 };
      await first.watch();
      tab = await openTab();
      const loadAt = times.loadAt = now();
      try {
        await tab.load();
      } catch (e) {
        throw new Error(`the second tab did not load the Mini's page: ${e.message.split('\n')[0]}`);
      }
      const opened = () => tab.sockets().find(socket => socket.openAt !== null);
      while (!opened() && now() - loadAt < SOCKET_MS) await wait(20);
      const socket = opened();
      times.socketsMade = tab.sockets().length;
      if (!socket) {
        const has = await first.has();
        throw new Error(`the second tab's signaling socket did not open in ${SOCKET_MS / 1000} s: ${describeSockets(tab.sockets(), loadAt)}; ` +
          `the first tab ${has.useHere ? 'shows "Use Here"' : has.video ? 'still has its session' : 'has no session and does not show "Use Here"'}`);
      }
      times.socketMadeAt = socket.createdAt;
      times.socketRequestAt = socket.requestAt;
      times.socketAt = socket.openAt;
      // The session's first frame comes some hundred ms after the socket:
      // the two sides exchange their descriptions over it first.
      times.frameAt = await tab.firstFrame(Math.max(1, SESSION_MS - (now() - loadAt)));
      if (times.frameAt === null) {
        throw new Error(`the second tab has no video ${SESSION_MS / 1000} s after its load began; its signaling socket opened after ${socket.openAt - loadAt} ms`);
      }
      times.useHereShownAt = await first.useHereShown(USE_HERE_SHOWN_MS);
      if (times.useHereShownAt === null) {
        throw new Error(`the first tab does not show "Use Here" ${USE_HERE_SHOWN_MS / 1000} s after the second tab had video`);
      }
      const useHereAt = times.useHereAt = await first.useHere();
      times.backAt = await first.firstFrame(SESSION_MS);
      // The page's sockets since the click: the one that opened, if one did.
      const since = (await first.sockets()).filter(made => made.createdAt >= useHereAt);
      const back = since.find(made => made.openAt !== null);
      times.backSocketMadeAt = back?.createdAt ?? null;
      times.backSocketRequestAt = back?.requestAt ?? null;
      times.backSocketAt = back?.openAt ?? null;
      times.backSocketsMade = since.length;
      if (times.backAt === null) {
        throw new Error(`the first tab has no video ${SESSION_MS / 1000} s after "Use Here": ` +
          (back ? `its signaling socket opened after ${back.openAt - useHereAt} ms` : describeSockets(since, useHereAt)));
      }
      returned = true;
      settled = await connections();
    },
    // Off is the end of the state: it closes the second tab.
    off: async () => {
      times = null;
      returned = false;
      settled = null;
      await closeTab();
      await first.endWatch();
    },
    isOn: () => times !== null && times.backAt !== null,
    works: on => !on || sessionBack(),
    sample: async () => {
      if (!times) return null;
      if (returned) settled ??= await connections();
      const now = settled && await connections();
      if (!now) return { ...times, newConnections: null };
      return { ...times, newConnections: now.sockets - settled.sockets + now.peers - settled.peers };
    },
  };
}
