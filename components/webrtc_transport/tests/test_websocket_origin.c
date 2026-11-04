#include <assert.h>
#include <stddef.h>

#include "websocket_origin.h"

int main(void)
{
    assert(webrtc_websocket_origin_allowed("http://192.168.1.44", "192.168.1.44"));
    assert(webrtc_websocket_origin_allowed("https://mini.example:8443", "mini.example:8443"));
    assert(webrtc_websocket_origin_allowed("http://mini.local", "MINI.local"));
    assert(webrtc_websocket_origin_allowed("http://[::1]:8080", "[::1]:8080"));
    assert(!webrtc_websocket_origin_allowed(NULL, "mini.local"));
    assert(!webrtc_websocket_origin_allowed("", "mini.local"));
    assert(!webrtc_websocket_origin_allowed("null", "mini.local"));
    assert(!webrtc_websocket_origin_allowed("http://mini.local", NULL));
    assert(!webrtc_websocket_origin_allowed("http://", ""));
    assert(!webrtc_websocket_origin_allowed("http://evil.example", "mini.local"));
    assert(!webrtc_websocket_origin_allowed("http://mini.local.evil.example", "mini.local"));
    assert(!webrtc_websocket_origin_allowed("http://mini.local:8080", "mini.local"));
    assert(!webrtc_websocket_origin_allowed("http://mini.local", "mini.local:8080"));
    assert(!webrtc_websocket_origin_allowed("http://mini.local/", "mini.local"));
    assert(!webrtc_websocket_origin_allowed("http://mini.local@evil.example", "mini.local"));
    assert(!webrtc_websocket_origin_allowed("ws://mini.local", "mini.local"));
    assert(!webrtc_websocket_origin_allowed("http://mini.local/path", "mini.local/path"));
    assert(!webrtc_websocket_origin_allowed("http://mini.local ", "mini.local "));
    return 0;
}
