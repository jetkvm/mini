# Combined load

Runs several Mini features at once (video, audio, virtual media, uploads, OS Services, cloud, MQTT, Tailscale, an update, ...) and checks every second that each still meets its criteria.

## Running

```sh
tools/combined_load/run.sh <bench>
```

`<bench>` is a bench file in `tools/benches/` (see `example.env`). The script builds this checkout for the bench's board, installs it if the Mini runs another build, and runs every list in `lists.jsonc`.

With an existing image:

```sh
node tools/combined_load/runner/run.mjs <bench> --image=<file> [--install] [--list=<name>] [--without=<variable>,...] [--out=<dir>] [--plain-cloud]
```

`--without=ota` skips the update; `--without=upload,tfMountCdrom,tfMountDisk` suits a Mini without a TF card. Set `JETKVM_SENDER_URL` and `JETKVM_SENDER_SHA256` to test your own OS Services sender build.

Requirements: the Mini in no-password mode with a TF card (1.1 GiB free); a host reachable over SSH (`JETKVM_REMOTE_HOST` or `JETKVM_COMBINED_LOAD_HOST`) with a signed-in desktop, `curl`, `python3` and Chrome; on the machine running the tool, Chrome, `frontend/jetkvm-ui/node_modules`, `docker buildx` and OpenSSL 3. `runner/hosts.mjs` has the per-OS host details.

## Criteria and known failures

- `<board>/criteria.jsonc`: per-second limits (`everySecond`) and how long each change may take to settle (`changeTakesAtMost`).
- `<board>/known-failures.jsonc`: breaches tolerated while an open issue describes them. An entry with `"fails": true` still fails the run, and the report names its issue.

```jsonc
{ "issue": "https://github.com/jetkvm/kvm-mini/issues/<n>",
  "while": { "osTerminal": true, "osFiles": true },
  "measure": "framesPerSecond", "tolerateSeconds": 16, "recoverySeconds": 2 }
```

## Report

Each list writes `recording.jsonl` and `report.txt` under `.e2e-out/combined-load/`. Exit status: 0 all passed, 1 a list failed or is undecided, 2 a list did not run, 3 only known failures.

Re-judge a recording: `node tools/combined_load/runner/judge.mjs <recording.jsonl>`.

## Tests

```sh
node --test tools/combined_load/runner/combined_load.test.mjs
```
