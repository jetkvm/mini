// Exercise the real adoption loader, setup form and action on separate local
// origins. No device, Google account or production cloud is used.
import assert from "node:assert/strict";
import { readFileSync } from "node:fs";
import http from "node:http";
import { fileURLToPath } from "node:url";
import { rolldown } from "rolldown";
import { chromium } from "playwright";

const ui = fileURLToPath(new URL("../", import.meta.url));
const adopt = `${ui}src/routes/adopt.tsx`;
const setup = `${ui}src/routes/devices.$id.setup.tsx`;
const entry = `${ui}tests/adoption-routing-entry.tsx`;
const deviceId = "80f1b2d631d9";

const bundle = await rolldown({
  input: entry,
  transform: { define: { "process.env.NODE_ENV": '"production"' } },
  plugins: [
    {
      name: "adoption-fixture",
      resolveId(source) {
        if (source === entry) return entry;
        if (source === "@/api") return `${ui}src/api.ts`;
        if (source === "@/ui.config") return "\0config";
        if (source === "@/main") return "\0auth";
        if (source.startsWith("@components/")) return "\0components";
        if (source.startsWith("@localizations/")) return "\0messages";
      },
      load(id) {
        if (id === adopt && process.env.ADOPT_SOURCE)
          return readFileSync(process.env.ADOPT_SOURCE, "utf8");
        if (id === "\0config")
          return `export const DEVICE_API = ""; export const CLOUD_API = "/cloud-api";`;
        if (id === "\0auth")
          return `export async function checkAuth() { return { id: "test-user" }; }`;
        if (id === "\0messages")
          return `export const m = new Proxy({}, { get: (_, key) => () => String(key) });`;
        if (id === "\0components")
          return `
        import { createElement } from "react";
        export default function Wrapper({ children }) { return children; }
        export function InputFieldWithLabel({ label, error, ...props }) {
          return createElement("label", {}, label, createElement("input", props), error);
        }
        export function Button({ text, type = "submit" }) {
          return createElement("button", { type }, text);
        }
      `;
        if (id === entry)
          return `
        import { createElement } from "react";
        import { createRoot } from "react-dom/client";
        import { createBrowserRouter, RouterProvider } from "react-router";
        import Adopt from ${JSON.stringify(adopt)};
        import Setup from ${JSON.stringify(setup)};
        const routes = window.fixtureMode === "device"
          ? [{ id: "adopt", path: "/adopt", element: createElement(Adopt), loader: Adopt.loader }]
          : [{ id: "root", path: "/", children: [
              { id: "setup", path: "devices/:id/setup", element: createElement(Setup),
                loader: Setup.loader, action: Setup.action },
              { id: "device", path: "devices/:id", element: createElement("p", { id: "connected-route" }, "Device route") }
            ] }];
        const router = createBrowserRouter(routes, { basename: window.fixtureBase || "/" });
        createRoot(document.getElementById("root")).render(createElement(RouterProvider, { router }));
      `;
      },
    },
  ],
});
const { output } = await bundle.generate({ format: "esm" });
await bundle.close();
const script = output.find(item => item.type === "chunk").code;

const listen = server => new Promise(resolve => server.listen(0, "127.0.0.1", resolve));
const close = server =>
  new Promise(resolve => {
    server.closeAllConnections();
    server.close(resolve);
  });
const json = (res, status, value) => {
  res.writeHead(status, { "Content-Type": "application/json" });
  res.end(JSON.stringify(value));
};
const body = async req => {
  const chunks = [];
  for await (const chunk of req) chunks.push(chunk);
  return Buffer.concat(chunks).toString();
};
const html = (mode, base, asset) => `<!doctype html><div id="root"></div>
  <script>window.fixtureMode=${JSON.stringify(mode)};window.fixtureBase=${JSON.stringify(base)}</script>
  <script type="module" src="${asset}"></script>`;

const browser = await chromium.launch({
  ...(process.env.CHROME_BIN ? { executablePath: process.env.CHROME_BIN } : {}),
  headless: true,
});
try {
  for (const scenario of [
    { version: "0.5.9-dev202610021009-mini" },
    { version: "0.5.10-test+codelog-mini" },
    { legacy: true },
    { empty: true },
    { version: "../wrong", invalid: true },
    { version: 42, invalid: true },
    { version: "0.5.9-mini", rejected: true },
  ]) {
    const prefix =
      scenario.legacy || scenario.empty ? "" : `/v/${encodeURIComponent(scenario.version)}`;
    const setupPath = `${prefix}/devices/${deviceId}/setup`;
    const finalPath = `${prefix}/devices/${deviceId}`;
    const assetPath = `${prefix}/assets/adoption.js`;
    const cloudRequests = [];
    const registrations = [];
    const names = [];
    const cloudServer = http.createServer((req, res) => {
      void (async () => {
        const path = new URL(req.url, "http://fixture").pathname;
        cloudRequests.push(path);
        if (path === assetPath) {
          res.writeHead(200, { "Content-Type": "text/javascript" });
          return res.end(script);
        }
        if (path === setupPath || path === finalPath) {
          res.writeHead(200, { "Content-Type": "text/html" });
          return res.end(html("cloud", prefix, assetPath));
        }
        if (path === `/cloud-api/devices/${deviceId}`) {
          if (req.method === "PUT") names.push(JSON.parse(await body(req)).name);
          return json(res, 200, { device: { id: deviceId, version: scenario.version } });
        }
        // In the versioned case, either unversioned device path is an error.
        res.writeHead(404);
        res.end("Unexpected cloud route");
      })().catch(error => {
        res.writeHead(500);
        res.end(String(error));
      });
    });
    await listen(cloudServer);
    const cloudOrigin = `http://127.0.0.1:${cloudServer.address().port}`;
    const deviceServer = http.createServer((req, res) => {
      void (async () => {
        const path = new URL(req.url, "http://fixture").pathname;
        if (path === "/adopt") {
          res.writeHead(200, { "Content-Type": "text/html" });
          return res.end(html("device", "", "/adoption.js"));
        }
        if (path === "/adoption.js") {
          res.writeHead(200, { "Content-Type": "text/javascript" });
          return res.end(script);
        }
        if (path === "/cloud/state") return json(res, 200, { appUrl: cloudOrigin });
        if (path === "/cloud/register") {
          registrations.push(JSON.parse(await body(req)));
          if (scenario.rejected) return json(res, 503, { error: "Registration rejected" });
          if (scenario.empty) {
            res.writeHead(200);
            return res.end();
          }
          return json(res, 200, {
            message: "Registered",
            ...(!scenario.legacy && { cloudUiVersion: scenario.version }),
          });
        }
        res.writeHead(404);
        res.end();
      })().catch(error => {
        res.writeHead(500);
        res.end(String(error));
      });
    });
    await listen(deviceServer);
    const context = await browser.newContext();
    const page = await context.newPage();
    page.setDefaultTimeout(10_000);
    page.setDefaultNavigationTimeout(10_000);
    try {
      const origin = `http://127.0.0.1:${deviceServer.address().port}`;
      await page.goto(
        `${origin}/adopt?deviceId=${deviceId}&tempToken=test-token&oidcGoogle=test-id-token&clientId=test-client`,
      );
      if (scenario.invalid || scenario.rejected) {
        await page
          .getByText(
            scenario.rejected
              ? "Failed to register device"
              : "Invalid cloud UI version in registration response",
            { exact: true },
          )
          .waitFor();
        assert.equal(new URL(page.url()).origin, origin);
        assert.deepEqual(cloudRequests, []);
      } else {
        await page.waitForURL(cloudOrigin + setupPath);
        await page.locator('input[name="name"]').fill("E bench");
        await page.locator('button[type="submit"]').click();
        await page.waitForURL(cloudOrigin + finalPath);
        await page.locator("#connected-route").waitFor();
        assert.deepEqual(names, ["E bench"]);
        assert.ok(cloudRequests.includes(assetPath), "the matching cloud bundle loaded");
        if (prefix)
          assert.ok(
            cloudRequests.every(path => !path.startsWith("/devices/")),
            "no default device UI was requested",
          );
        // Also verify a direct reload resolves the final versioned route.
        await page.reload();
        await page.locator("#connected-route").waitFor();
        assert.equal(page.url(), cloudOrigin + finalPath);
      }
      assert.deepEqual(registrations, [
        { token: "test-token", oidcGoogle: "test-id-token", clientId: "test-client" },
      ]);
      console.log(`adoption routing: ${JSON.stringify(scenario)} OK`);
    } finally {
      await context.close();
      await Promise.all([close(deviceServer), close(cloudServer)]);
    }
  }
} finally {
  await browser.close();
}
