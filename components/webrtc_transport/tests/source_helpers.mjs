import fs from "node:fs";
import {fileURLToPath} from "node:url";

// Production transport code is split by responsibility across these files.
// Source contracts inspect them together as one text.
export const transportSource = [
  "webrtc_transport.c", "webrtc_sessions.c", "webrtc_data_channels.c",
  "webrtc_transport_rpc_adapters.c", "webrtc_transport_ctx_internal.h",
  "webrtc_transport_private.h",
].map(name => fs.readFileSync(fileURLToPath(new URL("../" + name, import.meta.url)), "utf8")).join("\n");
