#!/usr/bin/env python3
"""One BLE Wi-Fi setup session with a Mini, as the setup page runs it.

Runs under the Python environment that has ESP-IDF's ``esp_prov`` tool and
``bleak``; ``p4_wifi_ble_setup_e2e.py`` starts it as a subprocess so the test
itself needs only the standard library. Connects to the device by its BLE
name, opens a Security 1 session with the setup code, and optionally sends a
network and polls the join state. Prints one JSON object on stdout; the
``esp_prov`` progress output goes to stderr.

The network password comes from the ``MINI_E2E_BLE_PASSPHRASE`` environment
variable so it stays out of the process list.

    IDF_PATH=<idf> python3 ble_setup_client.py --esp-prov-dir <dir> \\
        --name JetKVM_XXXXXX --code 123456 [--ssid NAME]

``--probe`` only imports esp_prov and its dependencies and exits, so the
test can find a broken environment before it takes the Mini offline.
"""

from __future__ import annotations

import argparse
import asyncio
import contextlib
import json
import os
from pathlib import Path
import sys
import time
from typing import Any

# esp_prov reports these join states; the others mean "still joining".
FINAL_STATES = ("connected", "failed")
# network_provisioning's default service UUID (scheme_ble.c, bytes stored LSB
# first); the Mini does not set its own. esp_prov's fallback, 021a9004-...,
# is the older wifi_provisioning UUID and does not match.
PROV_SERVICE_UUID = "1775244d-6b43-439b-877c-060f2d9bed07"


def preset_service_uuid() -> None:
    """Work around esp_prov after an earlier connection to the same device.

    esp_prov takes the service UUID from the advertisement only when BlueZ
    lists exactly one UUID for the device. After a connection, BlueZ adds the
    GATT services to that list, the UUID stays None, and esp_prov indexes the
    service table with None before it tries its fallback (which is the wrong
    UUID for network_provisioning anyway). Preset the UUID; an advertisement
    with one UUID still replaces it.
    """
    from transport import ble_cli

    original = ble_cli.BLE_Bleak_Client.__init__

    def init(self: Any) -> None:
        original(self)
        self.srv_uuid_adv = PROV_SERVICE_UUID

    ble_cli.BLE_Bleak_Client.__init__ = init


async def run(args: argparse.Namespace) -> dict[str, Any]:
    import esp_prov
    import security

    preset_service_uuid()
    result: dict[str, Any] = {
        "name": args.name,
        "connected": False,
        "session": False,
        "sent": None,
        "applied": None,
        "state": None,
        "states": [],
    }
    started = time.monotonic()
    transport = await esp_prov.get_transport("ble", args.name, args.adapter)
    if transport is None:
        result["error"] = "BLE connection failed"
        return result
    result["connected"] = True
    try:
        session = security.Security1(args.code, False)
        # A wrong code makes the Security 1 handshake fail; esp_prov returns
        # None or False for that.
        result["session"] = bool(await esp_prov.establish_session(transport, session))
        if not result["session"] or args.ssid is None:
            return result
        passphrase = os.environ.get("MINI_E2E_BLE_PASSPHRASE", "")
        result["sent"] = bool(
            await esp_prov.send_wifi_config(transport, session, args.ssid, passphrase)
        )
        result["applied"] = bool(await esp_prov.apply_wifi_config(transport, session))
        if not result["applied"]:
            return result
        deadline = time.monotonic() + args.join_timeout
        while time.monotonic() < deadline:
            await asyncio.sleep(2.0)
            try:
                state = await esp_prov.get_wifi_config(transport, session)
            except Exception as exc:  # the device may end BLE setup after a join
                result["error"] = f"state read failed: {type(exc).__name__}: {exc}"
                break
            result["states"].append(state)
            if state in FINAL_STATES:
                result["state"] = state
                break
    except Exception as exc:
        # After a wrong code the Mini closes the link during the handshake,
        # which bleak reports as a GATT error; the link was up, so the
        # caller waits for the miss in the serial log.
        result["error"] = f"{type(exc).__name__}: {exc}"
    finally:
        with contextlib.suppress(Exception):
            await transport.disconnect()
        result["elapsedS"] = round(time.monotonic() - started, 3)
    return result


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--esp-prov-dir", type=Path, required=True,
                        help="directory that contains esp_prov.py")
    parser.add_argument("--probe", action="store_true",
                        help="import esp_prov, its security module and bleak, then exit")
    parser.add_argument("--name", help="BLE name, for example JetKVM_E83AB8")
    parser.add_argument("--code", help="six-digit setup code")
    parser.add_argument("--adapter", default="hci0")
    parser.add_argument("--ssid", help="network to send; omit to test the session only")
    parser.add_argument("--join-timeout", type=float, default=90.0)
    args = parser.parse_args()
    if not args.probe and (args.name is None or args.code is None):
        parser.error("--name and --code are required")

    sys.path.insert(0, str(args.esp_prov_dir.resolve()))
    if args.probe:
        error = None
        with contextlib.redirect_stdout(sys.stderr):
            try:
                import bleak  # noqa: F401
                import esp_prov  # noqa: F401
                import security

                security.Security1("000000", False)
            except Exception as exc:
                error = f"{type(exc).__name__}: {exc}"
        print(json.dumps({"probe": error is None, "error": error}))
        return 0 if error is None else 1
    with contextlib.redirect_stdout(sys.stderr):
        try:
            result = asyncio.run(run(args))
        except Exception as exc:
            result = {"name": args.name, "error": f"{type(exc).__name__}: {exc}"}
    print(json.dumps(result, sort_keys=True))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
