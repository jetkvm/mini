#include "websocket_origin.h"

#include <string.h>
#include <strings.h>

bool webrtc_websocket_origin_allowed(const char *origin, const char *host)
{
    if (!origin || !host || !*host) return false;
    const char *authority;
    if (strncmp(origin, "http://", 7) == 0) {
        authority = origin + 7;
    } else if (strncmp(origin, "https://", 8) == 0) {
        authority = origin + 8;
    } else {
        return false;
    }
    /* Reject URL suffixes, userinfo and whitespace, even in a malformed Host. */
    for (const unsigned char *p = (const unsigned char *)host; *p; ++p) {
        if (*p <= 0x20 || *p >= 0x7f || strchr("/\\?#@,", *p)) return false;
    }
    return strcasecmp(authority, host) == 0;
}
