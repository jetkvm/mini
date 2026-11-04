# Embedded JetKVM web app pack

This component serves the JetKVM frontend from an asset pack linked into the application image, so the app and UI update and roll back together. There is no web partition or filesystem.

`webrtc_transport` registers the handlers on a server that uses `httpd_uri_match_wildcard`:

- `web_app_init()` validates the pack.
- `web_app_register_handlers()` serves `/static` and `/static/*`. Missing assets return 404.
- `web_app_register_spa_fallback()` adds the final `/*` route, which serves `index.html` to HTML navigation. Register it after every other route.
- `web_app_manifest_crc32()` identifies the exact pack content.

Assets are served directly from memory-mapped flash after a binary search of the URL table. `tools/web_app/pack_web_app.py` builds the pack from `frontend/jetkvm-ui/dist-p4`: text assets as deterministic gzip, compressed formats as is, identical payloads stored once.

The build fails without `frontend/jetkvm-ui/dist-p4/index.html`; build the frontend first with `npm run build:device -- --outDir dist-p4`. The pack is capped at 2.5 MiB so it fits an OTA slot with the app. If the pack fails validation at runtime, the server serves the diagnostic page instead.
