"""Compile the production peer manager paths against real host WireGuard.

Only platform services are stubbed. Keep the tested state transitions in the
production source so this regression also fails when the call site is removed.
"""
import pathlib
import re
import sys

root = pathlib.Path(__file__).resolve().parents[3]
mgr = pathlib.Path(sys.argv[1]).read_text()
include = root / "third_party/microlink/components/microlink/include"


def declaration(source, name):
    end = source.index("} " + name + ";") + len("} " + name + ";")
    start = source.rfind("typedef struct {", 0, end)
    return source[start:end]


def function(name):
    match = re.search(r"^static [^\n]+\b" + name + r"\([^;]*?\)\s*\{", mgr, re.M)
    if not match:
        raise ValueError("Missing production function: " + name)
    start = match.start()
    depth = 1
    end = match.end()
    while depth:
        depth += (mgr[end] == "{") - (mgr[end] == "}")
        end += 1
    return mgr[start:end]


types = [declaration((include / "microlink_internal.h").read_text(), "ml_peer_t"),
         declaration((include / "microlink.h").read_text(), "microlink_peer_info_t")]
pathlib.Path(sys.argv[2]).write_text("\n".join(types))
probe_start = mgr.index("typedef struct {", mgr.index("/* Pending DISCO probe tracking */"))
probe_end = mgr.index("/* ============================================================================", probe_start)
functions = ["key_to_base64", "find_peer_by_key", "find_peer_by_disco_key",
             "find_peer_by_id", "find_updated_peer", "add_peer", "remove_peer",
             "process_disco_pong", "process_disco_call_me_maybe"]
body = mgr[probe_start:probe_end]
body += "\nstatic void remove_peer(microlink_t *, const ml_peer_update_t *);\n"
body += "\n".join(function(name) for name in functions)
pathlib.Path(sys.argv[3]).write_text(body)
# The peer map's way into the slots, for the fill test.
fill = ["peer_session_up", "invalidate_peer_map", "select_peers", "apply_peer_update",
        "reclaim_unlisted", "process_peer_updates"]
pathlib.Path(sys.argv[4]).write_text("\n".join(function(name) for name in fill))
