# Integration tests

Node tests (`node --test`) that drive a real Mini over its JSON-RPC and HTTP API and check its own status. No UI clicks or screenshots; anything that needs the page belongs in the e2e suite.

## Prerequisites

- A bench file `tools/benches/<name>.env` (see [docs/testing.md](../../docs/testing.md)).
- Docker buildx, Node 22, Google Chrome, and `playwright-core` in `frontend/jetkvm-ui/node_modules`.
- Host-side tests need the bench's host reachable over SSH; some need a Linux host with the remote agent and passwordless `sudo`. Tests skip themselves when their bench lacks something.

## Run

```sh
tools/integration/run.sh <bench> [test name pattern]
```

Builds this checkout, installs it on the Mini if it runs another build, then runs `tests/*.test.mjs` one at a time. Exit status: 0 passed, 1 a test failed, 2 nothing ran.

## Layout

- `lib/` helpers: `mini.mjs` (connect, RPC, `until`), `host.mjs` (the bench's host), `keys.mjs`, `video.mjs`, `usb.mjs`, `servers.mjs` (stand-in update server, release bucket, MQTT broker), `cloud.mjs`, `erase.mjs`, and the OS Services helpers.
- `tests/` one file per subject, `<subject>.test.mjs`.
- `fixtures/` static files served by stand-in servers.

## Writing a test

```js
import { test } from 'node:test';
import { connect, until } from '../lib/mini.mjs';

test('the situation, as a sentence', { timeout: 60_000 }, async t => {
  const mini = await connect();
  t.after(() => mini.close());
  await mini.rpc('setSomething', { enabled: true });
  await until(mini.status, status => status.uptimeSeconds > 0, 10);
});
```

- Give every test a `timeout` and every wait a limit (`until(read, check, seconds)`).
- Register `t.after(() => mini.close())` right after `connect()`.
- Leave the Mini and host as found. `close()` restores the settings in `SETTINGS` (`lib/mini.mjs`); add a setting there or restore it in your own `t.after()`. Helpers that change state return a `restore()`/`leave()`/ `forget()` to call at the end.
- Automatic updates stay off; a test that needs them turns them back off.
- Skip with `t.skip(reason)` when the bench lacks a precondition; host helpers such as `hostAgent(t)` skip for you and return null.
- Use file names unique to the run on the TF card and delete them.
