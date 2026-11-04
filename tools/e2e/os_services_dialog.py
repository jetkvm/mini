"""Click a button of an open zenity dialog through AT-SPI, as a user would.

frontend/jetkvm-ui/e2e/mini/os-services-installer.ts runs it on the remote
host, in the desktop user's D-Bus session, for the dialogs the OS Services
sender opens on Linux ("Allow USB access?", "Allow this KVM?"):

    python3 - BUTTON TEXT...

BUTTON is the exact label of the button. The dialog is the zenity window that
shows every TEXT, in its title or in one of its labels. The script prints one
JSON line and exits with 0 when it clicked the button, 3 when no such dialog
is open (it lists the zenity windows it saw), and 2 without AT-SPI
(gir1.2-atspi-2.0 and python3-gi).
"""

import json
import sys

try:
    import gi

    gi.require_version("Atspi", "2.0")
    from gi.repository import Atspi
except (ImportError, ValueError) as error:
    print(json.dumps({"clicked": False, "error": f"AT-SPI is not available: {error}"}))
    sys.exit(2)

# A dialog's tree is a few levels deep; the limit only guards against a loop.
MAX_DEPTH = 32


def children(node):
    try:
        count = node.get_child_count()
    except Exception:  # noqa: BLE001 - a window can close while it is read
        return []
    found = []
    for index in range(count):
        try:
            child = node.get_child_at_index(index)
        except Exception:  # noqa: BLE001
            continue
        if child is not None:
            found.append(child)
    return found


def walk(node, depth=0):
    yield node
    if depth < MAX_DEPTH:
        for child in children(node):
            yield from walk(child, depth + 1)


def name(node):
    try:
        return node.get_name() or ""
    except Exception:  # noqa: BLE001
        return ""


def is_button(node):
    try:
        return node.get_role() == Atspi.Role.PUSH_BUTTON
    except Exception:  # noqa: BLE001
        return False


def main():
    button, texts = sys.argv[1], sys.argv[2:]
    seen = []
    for app in children(Atspi.get_desktop(0)):
        if name(app).lower() != "zenity":
            continue
        for window in children(app):
            nodes = list(walk(window))
            names = [name(node) for node in nodes]
            buttons = [node for node in nodes if is_button(node)]
            seen.append({"window": name(window), "buttons": [name(node) for node in buttons]})
            if not all(any(text in shown for shown in names) for text in texts):
                continue
            for node in buttons:
                if name(node) == button:
                    Atspi.Action.do_action(node, 0)
                    print(json.dumps({"clicked": True, "window": name(window)}))
                    return 0
    print(json.dumps({"clicked": False, "dialogs": seen}))
    return 3


if __name__ == "__main__":
    sys.exit(main())
