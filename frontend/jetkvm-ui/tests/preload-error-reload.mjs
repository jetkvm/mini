// A failed chunk import reloads the page once; a second failure within the
// guard time shows the error instead of reloading in a loop.
// Run from frontend/jetkvm-ui: node tests/preload-error-reload.mjs
import assert from "node:assert/strict";
import { readFileSync } from "node:fs";
import { createRequire } from "node:module";
const require = createRequire(new URL("../package.json", import.meta.url));
const ts = require("typescript");

const source = readFileSync(new URL("../src/preloadErrorReload.ts", import.meta.url), "utf8");
const { outputText } = ts.transpileModule(source, {
  compilerOptions: { target: ts.ScriptTarget.ES2022, module: ts.ModuleKind.ES2022 },
});
const { reloadOnPreloadError, RELOAD_GUARD_MS } = await import(
  `data:text/javascript;base64,${Buffer.from(outputText).toString("base64")}`
);

// One browser tab: session storage survives location.reload(), the listener
// is added again by the next page load.
function tab({ storageThrows = false } = {}) {
  const stored = new Map();
  const state = { reloads: 0, time: 1_000_000 };
  const load = () => {
    const win = new EventTarget();
    Object.defineProperty(win, "sessionStorage", {
      get() {
        if (storageThrows) throw new DOMException("denied", "SecurityError");
        return {
          getItem: key => stored.get(key) ?? null,
          setItem: (key, value) => stored.set(key, String(value)),
        };
      },
    });
    win.location = { reload: () => state.reloads++ };
    reloadOnPreloadError(win, () => state.time);
    return win;
  };
  let win = load();
  return {
    state,
    failImport() {
      const reloadsBefore = state.reloads;
      win.dispatchEvent(new Event("vite:preloadError", { cancelable: true }));
      if (state.reloads > reloadsBefore) win = load();
    },
  };
}

{
  // A stale tab reloads once; the reloaded page loads current chunks.
  const t = tab();
  t.failImport();
  assert.equal(t.state.reloads, 1);
}

{
  // The chunk is missing from the current build too: no second reload.
  const t = tab();
  t.failImport();
  t.state.time += 2_000;
  t.failImport();
  t.state.time += RELOAD_GUARD_MS - 2_001;
  t.failImport();
  assert.equal(t.state.reloads, 1);
}

{
  // A later update, after the guard time, reloads the same tab again.
  const t = tab();
  t.failImport();
  t.state.time += RELOAD_GUARD_MS;
  t.failImport();
  assert.equal(t.state.reloads, 2);
}

{
  // Without session storage there is no guard, so there is no reload.
  const t = tab({ storageThrows: true });
  t.failImport();
  assert.equal(t.state.reloads, 0);
}

console.log("preload-error-reload: ok");
