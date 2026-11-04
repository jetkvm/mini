/* The Noise msg2 frame of the control server's upgrade response, split at
 * every byte between the response's body and the socket. */
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define ESP_LOGI(tag, ...) ((void)(tag))
#define ESP_LOGE(tag, ...) ((void)(tag))

typedef struct microlink microlink_t;

static const char *TAG = "test_coord_msg2";
static const uint8_t *socket_data;
static size_t socket_len;
static size_t socket_read;

static void *ml_psram_malloc(size_t size) { return malloc(size); }

static int coord_recv(microlink_t *ml, uint8_t *buf, size_t len) {
    (void)ml;
    if (socket_read + len > socket_len) return -1;
    memcpy(buf, socket_data + socket_read, len);
    socket_read += len;
    return 0;
}

#include COORD_FUNCTIONS

/* msg2 with a 4-byte payload, then a proactive frame. */
static const uint8_t response[] = {0x02, 0x00, 0x04, 0xa1, 0xa2, 0xa3, 0xa4, 0x04, 0x00, 0x01, 0xb1};
#define MSG2_LEN 7

static uint8_t *read_split(const uint8_t *data, size_t len, size_t in_body, int *buf_len) {
    socket_data = data + in_body;
    socket_len = len - in_body;
    socket_read = 0;
    return coord_read_msg2(NULL, data, (int)in_body, buf_len);
}

int main(void) {
    for (size_t in_body = 0; in_body <= MSG2_LEN; ++in_body) {
        int len = -1;
        uint8_t *buf = read_split(response, MSG2_LEN, in_body, &len);
        assert(buf && len == MSG2_LEN && socket_read == MSG2_LEN - in_body);
        assert(!memcmp(buf, response, MSG2_LEN));
        free(buf);
    }
    puts("msg2 split at every byte between the body and the socket: PASS");

    int len = -1;
    uint8_t *buf = read_split(response, sizeof(response), sizeof(response), &len);
    assert(buf && len == (int)sizeof(response) && socket_read == 0);
    assert(!memcmp(buf, response, sizeof(response)));
    free(buf);
    puts("proactive frames after msg2 in the body stay in the buffer: PASS");

    for (size_t in_body = 0; in_body < MSG2_LEN; ++in_body) {
        assert(!read_split(response, in_body, in_body, &len));
    }
    const uint8_t transport[] = {0x04, 0x00, 0x00};
    assert(!read_split(transport, sizeof(transport), 1, &len));
    puts("a closed socket or a frame that is not msg2 fails the handshake: PASS");
}
