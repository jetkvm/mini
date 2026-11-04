# Tools

`run_all_tests.sh` runs every host check (see `docs/testing.md`). `run_host_tests_remote.sh` runs it on a remote BuildKit host.

Device suites, each against one Mini described by a bench file (copy `benches/example.env`):

- `e2e/`: the Playwright suite (`e2e/run.sh <bench>`), the BLE Wi-Fi setup test (`p4_wifi_ble_setup_e2e.py`) and their host-side helpers.
- `integration/`: one situation at a time over JSON-RPC. See its `README.md`.
- `combined_load/`: many features at once under load. See its `README.md`.

Build:

- `patches/`: adapts the esp_peer archive.
- `web_app/`: packs the built UI into the firmware.
- `edid/`: generates the EDID presets header.

Release:

- `release_r2.sh`: builds, signs and uploads a release (`--help`).
- `release_check.py`, `release_config.py`: its version and configuration checks.
- `third_party_notices.sh`: regenerates `THIRD_PARTY_NOTICES.md` from a build of both variants and the web UI (`third_party_notices.py`).
- `mini_image.py`: reads the version and SKU from a firmware image.

Other:

- `tests/`: unit tests for these tools.
- `upstream_delta.sh`: diff of the UI and e2e suite against upstream JetKVM.
- `package_c5_hosted_firmware.sh`: packages the ESP32-C5 network adapter firmware.
