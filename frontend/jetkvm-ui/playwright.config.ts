import { defineConfig } from "@playwright/test";

if (!process.env.JETKVM_URL) {
  throw new Error("JETKVM_URL environment variable is required");
}

export default defineConfig({
  testDir: "./e2e",
  globalSetup: "./e2e/global-setup.ts",
  globalTeardown: "./e2e/global-teardown.ts",
  timeout: 60000,
  workers: 1,
  reporter: [["list", { printSteps: true }]],
  use: {
    baseURL: process.env.JETKVM_URL,
    trace: "retain-on-failure",
    video: "retain-on-failure",
    screenshot: "only-on-failure",
  },
  projects: [
    {
      name: "ui",
      testIgnore: [/ota-.*/, /os-services.*\.spec\.ts/, /remote-agent\/.*/, /video-codec.*/],
    },
    // OS Services with the sender on the remote host (JETKVM_REMOTE_HOST);
    // tools/e2e/run.sh runs them in its second phase, with OS Services on.
    {
      name: "os-services",
      testMatch: /mini\/os-services.*\.spec\.ts/,
    },
    // The bench steps around and between run.sh's phases.
    {
      name: "os-services-bench",
      testMatch: /mini\/os-services-bench\.setup\.ts/,
    },
    {
      name: "video-codec",
      testMatch: /video-codec\.spec\.ts/,
    },
    {
      name: "remote-agent",
      testDir: "./e2e/remote-agent",
      testMatch: /ra-.*\.spec\.ts/,
    },
    // The OTA projects run in this order (tools/e2e/run.sh selects them in
    // it). Without a device shell each image is a full OTA install, so a
    // project starts where the one before it left the device. From the
    // candidate: ota-check installs nothing, and reboot-overlay pins the
    // baseline. What an update does without the page is tested in
    // tools/integration: the signature rule, the dev channel, a version asked
    // for by its number, the update check, an image of the other board, and an
    // update from the stable release with a config reset.
    {
      name: "ota-check",
      testMatch: /ota-check\.spec\.ts/,
    },
    {
      name: "ota-reboot-overlay",
      testMatch: /ota-reboot-overlay\.spec\.ts/,
    },
  ],
});
