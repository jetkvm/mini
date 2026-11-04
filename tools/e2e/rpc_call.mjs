#!/usr/bin/env node
// Call one JSON-RPC method on a Mini through the shipped UI's WebRTC channel.
//   node tools/e2e/rpc_call.mjs http://<device>/ <method> ['<json params>']
// Uses the vendored UI's playwright-core and the local Chrome, like the e2e.
import { createRequire } from "node:module";
import path from "node:path";
import { fileURLToPath } from "node:url";

const root = path.resolve(path.dirname(fileURLToPath(import.meta.url)), "..", "..");
const require = createRequire(path.join(root, "frontend", "jetkvm-ui", "node_modules") + path.sep);
const { chromium } = require("playwright-core");

const [url, method, paramsJson = "{}"] = process.argv.slice(2);
if (!url || !method) {
  console.error("usage: rpc_call.mjs http://<device>/ <method> ['<json params>']");
  process.exit(2);
}
const params = JSON.parse(paramsJson);
const browser = await chromium.launch({ headless: true, channel: "chrome" });
const page = await browser.newPage();
let code = 0;
try {
  await page.goto(url, { waitUntil: "domcontentloaded" });
  await page.waitForFunction(
    () => window.__kvmTestHooks?.isWebRTCConnected?.() && window.__kvmTestHooks?.isHidRpcReady?.(),
    null,
    { timeout: 60000 },
  );
  await page.waitForTimeout(1500);
  const response = await page.evaluate(
    ([m, p]) => new Promise(resolve => window.__kvmTestHooks.sendJsonRpc(m, p, resolve, 15000)),
    [method, params],
  );
  console.log(JSON.stringify(response));
  if (response?.error) code = 1;
} catch (error) {
  console.error(String(error));
  code = 1;
} finally {
  await browser.close();
}
process.exit(code);
