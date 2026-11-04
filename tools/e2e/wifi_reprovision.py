#!/usr/bin/env python3
"""Put a Wi-Fi Mini back on a network over BLE after one erase of its settings.

Started before the erase by tools/integration/lib/erase.mjs, on a host on the
Mini's network with a Bluetooth adapter. Needs a build with
JETKVM_WIFI_SETUP_LOG_CODE=1 (fixed setup code BENCH_SETUP_CODE). The
password comes from MINI_E2E_HOME_PASSWORD.

Once OFFLINE_MISSES connections in a row fail, it retries
ble_setup_client.py until the Mini has joined and answers HTTP again. Exit
status 0 on success, 1 when --limit seconds pass first.
"""

from __future__ import annotations

import argparse
import json
import os
from pathlib import Path
import socket
import subprocess
import sys
import time

# The fixed setup code of bench builds (C5_BENCH_POP in
# components/jetkvm_c5_wifi/jetkvm_c5_wifi.c).
BENCH_SETUP_CODE = "246810"
# One client run takes about 20 s; a run that hangs is stopped and repeated.
CLIENT_TIMEOUT_S = 60.0
JOIN_TIMEOUT_S = 30.0
POLL_S = 2.0
HTTP_JOIN_TIMEOUT_S = 15.0
# A Mini that writes an update to its flash can miss one connection of 2 s.
OFFLINE_MISSES = 3


def reachable(host: str) -> bool:
    """True when the Mini accepts a TCP connection on its HTTP port."""
    try:
        with socket.create_connection((host, 80), timeout=2.0):
            return True
    except OSError:
        return False


def send_network(args: argparse.Namespace, password: str) -> bool:
    """Run ble_setup_client.py once; True when the Mini reports the network as joined."""
    client = Path(__file__).with_name("ble_setup_client.py")
    try:
        result = subprocess.run(
            [args.prov_python, str(client), "--esp-prov-dir", args.esp_prov_dir, "--name", args.name,
             "--code", BENCH_SETUP_CODE, "--adapter", args.adapter, "--ssid", args.ssid,
             "--join-timeout", str(JOIN_TIMEOUT_S)],
            env={**os.environ, "IDF_PATH": args.idf_path, "MINI_E2E_BLE_PASSPHRASE": password},
            stdout=subprocess.PIPE, text=True, timeout=CLIENT_TIMEOUT_S, check=False)
    except subprocess.TimeoutExpired:
        print(f"BLE client did not finish within {CLIENT_TIMEOUT_S:.0f} s", file=sys.stderr, flush=True)
        return False
    print(result.stdout.strip(), flush=True)
    try:
        joined = json.loads(result.stdout).get("state") == "connected"
    except json.JSONDecodeError:
        joined = False
    return result.returncode == 0 and joined


def wait_offline(host: str, deadline: float) -> bool:
    """True when OFFLINE_MISSES connections in a row have failed; False at the deadline."""
    misses = 0
    while misses < OFFLINE_MISSES:
        if time.monotonic() >= deadline:
            return False
        misses = 0 if reachable(host) else misses + 1
        if misses < OFFLINE_MISSES:
            time.sleep(POLL_S)
    return True


def provision(args: argparse.Namespace, password: str, deadline: float) -> bool:
    """Put the Mini back after its erase; False when the deadline passes first."""
    if not wait_offline(args.host, deadline):
        print(f"{args.host} did not go offline", file=sys.stderr, flush=True)
        return False
    print(f"{args.host} offline; checking BLE setup", flush=True)
    joined = False
    while not (joined and reachable(args.host)):
        if time.monotonic() >= deadline:
            print(f"{args.host}: " + ("joined over BLE, but no answer on HTTP" if joined else "no BLE run sent the network"),
                  file=sys.stderr, flush=True)
            return False
        if send_network(args, password):
            joined = True
            # BLE joining and HTTP readiness are separate. Another reset (or
            # an address change) can happen between them; never stop retrying
            # just because the earlier BLE session reported connected.
            grace = time.monotonic() + HTTP_JOIN_TIMEOUT_S
            while not reachable(args.host) and time.monotonic() < grace:
                time.sleep(POLL_S)
        else:
            time.sleep(POLL_S)
    print(f"{args.host} HTTP reachable", flush=True)
    return True


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--host", required=True, help="the Mini's IP address")
    parser.add_argument("--name", required=True, help="the Mini's BLE name, for example JetKVM_E83AB8")
    parser.add_argument("--ssid", required=True, help="the network to put the Mini on")
    # The same defaults as p4_wifi_ble_setup_e2e.py.
    home = Path.home()
    parser.add_argument("--prov-python", default=str(home / ".prov-venv/bin/python"),
                        help="Python with esp_prov's dependencies and bleak")
    parser.add_argument("--esp-prov-dir", default=str(home / "jetkvm-prov/component/tool/esp_prov"))
    parser.add_argument("--idf-path", default=str(home / "jetkvm-prov/idf"))
    parser.add_argument("--adapter", default="hci0")
    parser.add_argument("--limit", type=float, default=900.0, help="seconds for the erase and the way back")
    args = parser.parse_args()
    password = os.environ.get("MINI_E2E_HOME_PASSWORD")
    if not password:
        print("MINI_E2E_HOME_PASSWORD is not set", file=sys.stderr)
        return 2
    print(f"watching {args.host} ({args.name})", flush=True)
    return 0 if provision(args, password, time.monotonic() + args.limit) else 1


if __name__ == "__main__":
    sys.exit(main())
