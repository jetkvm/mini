# Upstreaming the Mini's UI changes

`jetkvm-ui/` changes the JetKVM web UI in a few places. This lists which of those changes `jetkvm/kvm` could take, and how to export them.

## Fixes any JetKVM could take

These have nothing Mini-specific in them:

- **Reload after a firmware update** (`src/preloadErrorReload.ts`, `src/main.tsx`). A tab opened before an update asks for route chunks that no longer exist; it now reloads once instead of showing an error.
- **Copy on plain HTTP** (`src/components/useCopyToClipBoard.tsx`). Without `navigator.clipboard`, copying used a temporary input that the video's focus trap took focus from, so nothing was copied.
- **Connection retries** (`src/webrtc/signaling/useWebSocketSignaling.ts`, `tests/signaling-lifecycle.mjs`). A failed attempt (no answer within 10 s, no data channel, failed peer) is retried up to three times on a new signaling socket before the page shows the failure.
- **Upload speed and time left** (`src/utils/uploadEta.ts`, `tests/upload-eta.mjs`, `src/routes/devices.$id.mount.tsx`). The rate is measured over the last 10 s, so it falls during a stall.
- **Clickable settings rows** (`src/components/SettingsItem.tsx`). Clicking a row's text no longer presses the row's button.
- **Component options** used by the features below: `SplitButton` sizes, themes and item descriptions, an optional description in `SettingsSectionHeader`, and a `VideoPausedOverlay` that takes its title and text.
- **Inter instead of Circular** (`src/assets/fonts/`, `src/index.css`, `index.html`, `MetricsChart.tsx`). Inter is under the OFL, so it can ship in an open-source build; Circular is a commercial font.

## Features that need firmware support

Each of these is behind a capability that the firmware reports, so the UI shows nothing on a JetKVM that doesn't report it. Upstream could take the UI first and turn each one on when its firmware supports it.

| Feature | Capability | Main files |
| --- | --- | --- |
| OS Services: settings page, guest clipboard, file drop, guest terminal, installer disc, status indicators | `os_services` | `devices.$id.settings.os-services.tsx`, `useGuestClipboard.ts`, `useGuestFileDrop.ts`, `useOsServicesInstaller.ts`, `Guest*StatusCard.tsx`, `InfoBar.tsx`, `Terminal.tsx`, `PasteModal.tsx` |
| Tailscale on its own settings page | `tailscale` | `devices.$id.settings.tailscale.tsx`, `TailscaleCard.tsx` |
| Extra host names the KVM answers under while it has no password | `extra_hostnames` | `devices.$id.settings.access._index.tsx`, `e2e/extra-hostnames.spec.ts` |
| Display rotation in quarter turns, display buttons | `display_quarter_turns`, `display_buttons` | `devices.$id.settings.hardware.tsx` |
| TF card space, image count and exFAT format | `removable_storage` | `devices.$id.mount.tsx` |
| Versioned cloud setup page after adoption | none; read from the registration response | `src/routes/adopt.tsx`, `tests/adoption-routing.mjs` |

## Mini-only changes

These should stay in the Mini:

- The Mini's USB vendor and product IDs in `UsbInfoSetting.tsx`.
- The MAC address source setting (`MacAddressSourceSetting.tsx`), which chooses between the factory address and the ESP32's.
- USB debugging in the advanced settings, which is the ESP32-P4's USB Serial/JTAG port.
- `e2e/mini/`, and the e2e changes that remove or shorten upstream specs for hardware or OTA paths the Mini doesn't have.

## Exporting a patch

`tools/upstream_delta.sh` lists every file that differs from upstream, and `tools/upstream_delta.sh e2e` does the same for the e2e specs. `tools/upstream_delta.sh --patch` prints one diff of everything outside `e2e/mini/`, which applies in a `jetkvm/kvm` checkout with `git apply --directory=ui`. To send one change, cut its files out of that diff.

The comparison base is [`jetkvm/kvm@77fc7cd`](https://github.com/jetkvm/kvm/tree/77fc7cd/ui).

## Build

The firmware packs `jetkvm-ui/dist-p4/`. Use Node 22:

```sh
cd frontend/jetkvm-ui
HUSKY=0 npm ci
npm run build:device -- --outDir dist-p4
```
