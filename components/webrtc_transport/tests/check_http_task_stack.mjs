import assert from "node:assert/strict";
import fs from "node:fs";

// The HTTP server stack must be in HP internal RAM. PSRAM fails the NVS
// writes of the password and registration handlers; LP (RTC) RAM is internal
// but slow, and MALLOC_CAP_DMA excludes it.
const source = fs.readFileSync(new URL("../webrtc_http.c", import.meta.url), "utf8")
  .replace(/\/\*[\s\S]*?\*\//g, "").replace(/\/\/.*$/gm, "");
const caps = [...source.matchAll(/task_caps\s*=\s*([^;]+);/g)].map(match => match[1]);
const configure = source.slice(source.indexOf("static void configure_http("));
const shared = configure.match(/config->task_caps\s*=\s*([^;]+);/);

assert.ok(shared, "configure_http() sets the HTTP server's task stack caps");
for (const cap of ["MALLOC_CAP_INTERNAL", "MALLOC_CAP_DMA"]) {
  assert.ok(shared[1].includes(cap), `HTTP task stack caps miss ${cap}: ${shared[1]}`);
}
for (const value of caps) {
  assert.ok(!value.includes("MALLOC_CAP_RTCRAM"), `HTTP task stack in LP RAM: ${value}`);
}

console.log("HTTP server task stack in HP internal RAM: OK");
