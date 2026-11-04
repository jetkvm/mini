// The host desktop's clipboard, for OS Services clipboard tests.
import { LINUX_USER_SESSION, quote } from '../../combined_load/runner/hosts.mjs';

// Gtk 3 on the session's X11 display (GNOME syncs Wayland). "copy" forks a
// process that owns the clipboard until another program takes it, up to 120 s.
const GTK_CLIPBOARD = `
import os, sys, time
mode = sys.argv[1]
text = sys.stdin.buffer.read().decode("utf-8")
ready, owned = os.pipe()
if mode != "paste" and os.fork():
    os.close(owned)
    sys.exit(0 if os.read(ready, 2) == b"ok" else 1)
import gi
gi.require_version("Gdk", "3.0")
gi.require_version("Gtk", "3.0")
from gi.repository import Gdk, GLib, Gtk
board = Gtk.Clipboard.get(Gdk.SELECTION_CLIPBOARD)
if mode == "paste":
    sys.stdout.buffer.write((board.wait_for_text() or "").encode("utf-8"))
    sys.exit(0)
os.setsid()
null = os.open(os.devnull, os.O_RDWR)
for stream in (0, 1, 2):
    os.dup2(null, stream)
board.set_text(text, -1)
since = time.monotonic()
if mode == "clear":
    board.clear()
def announce():
    os.write(owned, b"ok")
    if mode == "clear":
        Gtk.main_quit()
    return False
def owner_changed(*_):
    if time.monotonic() - since > 0.5:
        Gtk.main_quit()
board.connect("owner-change", owner_changed)
GLib.idle_add(announce)
GLib.timeout_add_seconds(120, Gtk.main_quit)
Gtk.main()
`;
const GTK = mode => `${LINUX_USER_SESSION}GDK_BACKEND=x11 timeout 30 python3 -c ${quote(GTK_CLIPBOARD)} ${mode}`;
const COMMANDS = {
  linux: { copy: GTK('copy'), paste: GTK('paste'), clear: GTK('clear') },
  macos: {
    copy: 'LANG=en_US.UTF-8 pbcopy',
    paste: 'LANG=en_US.UTF-8 pbpaste',
    clear: `osascript -l JavaScript -e ${quote("ObjC.import('AppKit'); $.NSPasteboard.generalPasteboard.clearContents")}`,
  },
};


// { copy, paste, clear }, or null when unreadable (Windows, or Linux without
// python3 Gtk 3 or a logged-in desktop).
export async function hostClipboard(host) {
  const commands = COMMANDS[host.system];
  if (!commands) return null;
  const paste = () => host.exec(commands.paste, { timeoutMs: 60_000 });
  if ((await paste().catch(() => null)) === null) return null;
  return {
    copy: text => host.exec(commands.copy, { input: text, timeoutMs: 60_000 }),
    paste,
    clear: () => host.exec(commands.clear, { timeoutMs: 60_000 }),
  };
}
