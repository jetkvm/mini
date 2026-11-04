# Testing

| Suite | Needs a Mini | What it checks |
|---|---|---|
| Host tests | no | Component logic, patches and tools, on the build machine |
| End-to-end | yes | The web UI against a real Mini (Playwright) |
| Integration | yes | One situation at a time through JSON-RPC, without the page |
| Combined load | yes | Many features at once, judged every second against criteria |

## Host tests

```sh
idf.py reconfigure
tools/run_all_tests.sh
```

Needs a C compiler, Python, Node.js and the ESP RISC-V toolchain. CI runs this on every pull request, together with lint and build checks of the UI.

## Device setup

The device suites share one bench file describing the Mini under test:

1. Copy `tools/benches/example.env` to `tools/benches/<name>.env` and fill it in.
2. Insert a TF card in the Mini and put it in no-password mode.

Each runner builds the current source (with Docker, on `JETKVM_BUILD_HOST` if set), installs it on the Mini if it runs another build, and restores the Mini's settings afterwards. Tests that check the controlled computer build the remote agent (`frontend/e2e/remote-agent`, needs Go) and start it there over SSH.

## End-to-end

```sh
tools/e2e/run.sh <name>
tools/e2e/run.sh <name> --no-update-tests        # skip firmware update tests
tools/e2e/run.sh <name> --phase 2 -g <pattern>   # one phase, some specs
```

The Playwright suite in `frontend/jetkvm-ui/e2e` is mostly vendored from upstream; Mini-only specs live in `e2e/mini/`. Phase 1 runs everything except OS Services; phase 2 runs the OS Services specs with the sender started.

When writing specs: skip only for a missing precondition (`skipWithout…()` helpers), restore the settings you change, and never erase the Mini.

## Integration

```sh
tools/integration/run.sh <name> [test name pattern]
```

Node tests in `tools/integration/tests/` that call the Mini's JSON-RPC and check its own status: settings across a reboot, update rules, factory reset, cloud, USB. Anything that needs no page belongs here rather than in the e2e suite. See [tools/integration/README.md](../tools/integration/README.md).

## Combined load

```sh
tools/combined_load/run.sh <name>
```

Takes the Mini through the lists in `tools/combined_load/lists.jsonc` (video, audio, virtual media, uploads, OS Services, cloud, an update, ...) and checks every second against each board's `criteria.jsonc`. See [tools/combined_load/README.md](../tools/combined_load/README.md).

## Recovering a Mini

A Mini that won't boot or update can be written over its USB debugger:

```sh
node tools/combined_load/runner/build.mjs <name> .e2e-out/flash
tools/e2e/serial_flash.sh <name> .e2e-out/flash
```
