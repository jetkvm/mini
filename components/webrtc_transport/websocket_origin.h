#pragma once

#include <stdbool.h>

/* Browser handshakes must come from the authority serving the device UI.
 * HTTP and HTTPS cover direct access and TLS-terminating reverse proxies. */
bool webrtc_websocket_origin_allowed(const char *origin, const char *host);
