/**
 * @file ml_coord.c
 * @brief Coordination Task - Control Plane Client
 *
 * Owns ALL Noise protocol state (keys, nonces) exclusively.
 * Handles registration, MapResponse parsing, endpoint updates.
 * Runs as a state machine on Core 1.
 *
 * Protocol flow:
 *   1. DNS resolve controlplane.tailscale.com
 *   2. TCP connect to port 80 (NOT TLS - Noise provides encryption)
 *   3. HTTP/1.1 Upgrade with Noise msg1 in X-Tailscale-Handshake header
 *   4. Read Noise msg2 from upgrade response body
 *   5. Derive transport keys, send H2 preface (encrypted via Noise)
 *   6. Send RegisterRequest (Noise-encrypted H2 HEADERS+DATA)
 *   7. Parse RegisterResponse
 *   8. Send MapRequest, parse MapResponse (peers)
 *   9. Long-poll for updates
 *
 * Reference: tailscale/control/controlclient/auto.go
 *            tailscale/control/controlclient/direct.go
 */

#include "microlink_internal.h"
#include "ml_h2_message.h"
#include "freertos/idf_additions.h"
#include "x25519.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_random.h"
#include "esp_netif.h"
#include "esp_http_client.h"
#include "esp_crt_bundle.h"
#include "cJSON.h"
#include "lwip/sockets.h"
#include "lwip/netdb.h"
#include "mbedtls/base64.h"
#include "mbedtls/error.h"
#include "mbedtls/net_sockets.h"
#include <string.h>
#include <errno.h>

static const char *TAG = "ml_coord";

static uint32_t self_ipv4(const cJSON *addresses);

/*
 * Where the JSON body starts in a control-plane response.
 *
 * Some responses arrive as a 4-byte little-endian length followed by the JSON,
 * and some arrive as bare JSON. The length is what decides which, not the
 * position of the first '{': a length whose low byte is 0x7b *is* an ASCII '{',
 * so scanning for a brace finds the prefix itself and parses from there.
 *
 * That is not hypothetical, and it is not intermittent either. A 20863-byte
 * MapResponse carries the little-endian length 0x0000517b, whose first byte is
 * '{', so the scan reported offset 0 and cJSON was handed
 * "{Q\0\0{\"Node\"..." — failing at byte 1. Because a netmap's size is stable
 * for a given tailnet, the retry produces the same buffer and the same failure
 * indefinitely: the device never joins, and the control plane times out. The
 * same tailnet's 20861-byte response carries 0x00005179 and parsed fine minutes
 * earlier, which is what makes this look like flakiness across boots while
 * being perfectly deterministic within one.
 *
 * Returns 4 when the first four bytes declare exactly the rest of the buffer,
 * and 0 otherwise. No byte value can spoof that arithmetic.
 */
static size_t coord_json_offset(const uint8_t *buf, size_t total)
{
    if (buf == NULL || total <= 4) {
        return 0;
    }
    const uint32_t declared = (uint32_t)buf[0]
                            | ((uint32_t)buf[1] << 8)
                            | ((uint32_t)buf[2] << 16)
                            | ((uint32_t)buf[3] << 24);
    return (size_t)declared + 4 == total ? 4 : 0;
}

/* Effective control plane host: NVS override or compiled default */
#define CTRL_HOST(ml) ((ml)->ctrl_host[0] ? (ml)->ctrl_host : ML_CTRL_HOST)

/* NodeKeyChallenge from EarlyNoise (stored between handshake and register) */
static uint8_t s_node_key_challenge[32] = {0};
static bool s_has_node_key_challenge = false;

/* Coordination state machine */
typedef enum {
    COORD_IDLE,
    COORD_STUN_PROBE,
    COORD_DNS_RESOLVE,
    COORD_TCP_CONNECT,
    COORD_NOISE_HANDSHAKE,
    COORD_H2_PREFACE,
    COORD_REGISTER,
    COORD_FETCH_PEERS,
    COORD_LONG_POLL,
    COORD_RECONNECTING,
} coord_state_t;

/* ============================================================================
 * Helper: hex encoding for keys
 * ========================================================================== */

static void bytes_to_hex(const uint8_t *bytes, size_t len, char *hex) {
    static const char hextab[] = "0123456789abcdef";
    for (size_t i = 0; i < len; i++) {
        hex[i * 2] = hextab[bytes[i] >> 4];
        hex[i * 2 + 1] = hextab[bytes[i] & 0x0F];
    }
    hex[len * 2] = '\0';
}

static int hex_to_bytes(const char *hex, uint8_t *bytes, size_t max_len) {
    size_t hex_len = strlen(hex);
    if (hex_len % 2 != 0 || hex_len / 2 > max_len) return -1;
    for (size_t i = 0; i < hex_len / 2; i++) {
        unsigned int val;
        if (sscanf(hex + i * 2, "%2x", &val) != 1) return -1;
        bytes[i] = (uint8_t)val;
    }
    return hex_len / 2;
}

/* ============================================================================
 * TCP I/O helpers for coord socket (owned exclusively by this task)
 * ========================================================================== */

/* --- optional TLS wrapper for the coordination connection (issue #16) ------ *
 * When config.ctrl_tls is set the coord socket is wrapped in TLS, so the
 * control plane can sit behind an HTTPS reverse proxy or be the hosted Tailscale
 * service. The BIO and the setup mirror the DERP connection (see ml_derp.c). */

#define COORD_TLS_TIMEOUT_MS 10000

static int coord_bio_send(void *ctx, const unsigned char *buf, size_t len) {
    int fd = *(int *)ctx;
    if (fd < 0) return MBEDTLS_ERR_NET_INVALID_CONTEXT;
    int ret = (int)ml_write_sock(fd, buf, len);
    if (ret < 0) {
        if (errno == EAGAIN || errno == EWOULDBLOCK) return MBEDTLS_ERR_SSL_WANT_WRITE;
        if (errno == EPIPE || errno == ECONNRESET) return MBEDTLS_ERR_NET_CONN_RESET;
        if (errno == EINTR) return MBEDTLS_ERR_SSL_WANT_WRITE;
        return MBEDTLS_ERR_NET_SEND_FAILED;
    }
    return ret;
}

static int coord_bio_recv(void *ctx, unsigned char *buf, size_t len, uint32_t timeout) {
    int fd = *(int *)ctx;
    if (fd < 0) return MBEDTLS_ERR_NET_INVALID_CONTEXT;
    uint32_t t = timeout ? timeout : COORD_TLS_TIMEOUT_MS;
    struct timeval tv = { .tv_sec = t / 1000, .tv_usec = (t % 1000) * 1000 };
    ml_setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    int ret = (int)ml_read_sock(fd, buf, len);
    if (ret < 0) {
        if (errno == EAGAIN || errno == EWOULDBLOCK) return MBEDTLS_ERR_SSL_TIMEOUT;
        if (errno == EPIPE || errno == ECONNRESET) return MBEDTLS_ERR_NET_CONN_RESET;
        if (errno == EINTR) return MBEDTLS_ERR_SSL_WANT_READ;
        return MBEDTLS_ERR_NET_RECV_FAILED;
    }
    return ret;
}

/* Free the coord TLS context if one is up (before a reconnect, or on teardown). */
static void coord_tls_free(microlink_t *ml) {
    if (!ml->coord_tls_up) return;
    mbedtls_ssl_free(&ml->coord_ssl);
    mbedtls_ssl_config_free(&ml->coord_ssl_conf);
    ml->coord_tls_up = false;
}

static int coord_send(microlink_t *ml, const uint8_t *data, size_t len) {
    if (ml->config.ctrl_tls) {
        size_t sent = 0;
        while (sent < len) {
            int n = mbedtls_ssl_write(&ml->coord_ssl, data + sent, len - sent);
            if (n == MBEDTLS_ERR_SSL_WANT_READ || n == MBEDTLS_ERR_SSL_WANT_WRITE) continue;
            if (n <= 0) {
                ESP_LOGE(TAG, "coord_send (tls) failed: %d", n);
                return -1;
            }
            sent += n;
        }
        return 0;
    }
    /* Set send timeout to prevent indefinite blocking (v1 uses MSG_DONTWAIT) */
    struct timeval snd_tv = { .tv_sec = 5, .tv_usec = 0 };
    ml_setsockopt(ml->coord_sock, SOL_SOCKET, SO_SNDTIMEO, &snd_tv, sizeof(snd_tv));

    size_t sent = 0;
    while (sent < len) {
        int n = ml_send(ml->coord_sock, data + sent, len - sent, 0);
        if (n <= 0) {
            ESP_LOGE(TAG, "coord_send failed: sent=%d/%d n=%d errno=%d",
                     (int)sent, (int)len, n, errno);
            return -1;
        }
        sent += n;
    }
    return 0;
}

static int coord_recv(microlink_t *ml, uint8_t *buf, size_t len) {
    if (ml->config.ctrl_tls) {
        size_t recvd = 0;
        int retries = 0;
        while (recvd < len) {
            int n = mbedtls_ssl_read(&ml->coord_ssl, buf + recvd, len - recvd);
            if (n == MBEDTLS_ERR_SSL_WANT_READ || n == MBEDTLS_ERR_SSL_WANT_WRITE ||
                n == MBEDTLS_ERR_SSL_TIMEOUT) {
                if (recvd == 0) return -1;   /* nothing consumed yet: caller can retry */
                if (++retries > 300) {       /* ~3 s: must finish to keep frame alignment */
                    ESP_LOGE(TAG, "coord_recv (tls) partial timeout: %d/%d", (int)recvd, (int)len);
                    return -1;
                }
                vTaskDelay(pdMS_TO_TICKS(10));
                continue;
            }
            if (n <= 0) {
                ESP_LOGE(TAG, "coord_recv (tls) failed: %d", n);
                return -1;
            }
            recvd += n;
            retries = 0;
        }
        return 0;
    }
    size_t recvd = 0;
    int retries = 0;
    while (recvd < len) {
        int n = ml_recv(ml->coord_sock, buf + recvd, len - recvd, 0);
        if (n <= 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                if (recvd == 0) {
                    /* No data consumed yet — timeout is fine, caller can retry */
                    return -1;
                }
                /* Partial data consumed — we MUST finish this read or the
                 * Noise frame stream will be misaligned. Retry with backoff. */
                if (++retries > 300) {  /* ~3 seconds */
                    ESP_LOGE(TAG, "coord_recv partial timeout: %d/%d bytes",
                             (int)recvd, (int)len);
                    return -1;
                }
                vTaskDelay(pdMS_TO_TICKS(10));
                continue;
            }
            ESP_LOGE(TAG, "coord_recv failed: %d (errno %d, recvd %d/%d)",
                     n, errno, (int)recvd, (int)len);
            return -1;
        }
        recvd += n;
        retries = 0;  /* Reset on successful read */
    }
    return 0;
}

/* Read up to len bytes in one go (not a fixed-length read), TLS-aware. Used for
 * the HTTP upgrade response and the proactive frames that may trail it. Returns
 * the byte count, or <= 0 on error/timeout. Note: over TLS the read timeout is
 * the config's (COORD_TLS_TIMEOUT_MS), not any SO_RCVTIMEO set by the caller. */
static int coord_read_some(microlink_t *ml, uint8_t *buf, size_t len) {
    if (ml->config.ctrl_tls) {
        int n;
        do {
            n = mbedtls_ssl_read(&ml->coord_ssl, buf, len);
        } while (n == MBEDTLS_ERR_SSL_WANT_READ || n == MBEDTLS_ERR_SSL_WANT_WRITE);
        return n;
    }
    return (int)ml_recv(ml->coord_sock, buf, len, 0);
}

/* ============================================================================
 * Noise-encrypted transport I/O
 * ========================================================================== */

/* Send a Noise transport frame: [0x04][len_hi][len_lo][encrypted_data+MAC] */
static int noise_send(microlink_t *ml, ml_noise_state_t *noise,
                        const uint8_t *plaintext, size_t pt_len) {
    size_t ct_len = pt_len + 16;  /* ciphertext + 16-byte MAC */
    uint8_t *frame = ml_psram_malloc(3 + ct_len);
    if (!frame) return -1;

    frame[0] = 0x04;  /* Transport data frame type */
    frame[1] = (ct_len >> 8) & 0xFF;
    frame[2] = ct_len & 0xFF;

    if (ml_noise_encrypt(noise->tx_key, noise->tx_nonce,
                          NULL, 0,
                          plaintext, pt_len,
                          frame + 3) != ESP_OK) {
        free(frame);
        return -1;
    }
    noise->tx_nonce++;

    int ret = coord_send(ml, frame, 3 + ct_len);
    free(frame);
    return ret;
}

/* Receive and decrypt a Noise transport frame, returns plaintext length */
static int noise_recv(microlink_t *ml, ml_noise_state_t *noise,
                        uint8_t *plaintext, size_t max_len) {
    /* Read 3-byte frame header */
    uint8_t hdr[3];
    if (coord_recv(ml, hdr, 3) < 0) return -1;

    if (hdr[0] != 0x04) {
        ESP_LOGE(TAG, "Unexpected Noise frame type: 0x%02x", hdr[0]);
        return -1;
    }

    uint16_t ct_len = (hdr[1] << 8) | hdr[2];
    if (ct_len < 16) return -1;
    size_t pt_len = ct_len - 16;
    if (pt_len > max_len) {
        ESP_LOGE(TAG, "Noise frame too large: %d > %d", (int)pt_len, (int)max_len);
        return -1;
    }

    uint8_t *ciphertext = ml_psram_malloc(ct_len);
    if (!ciphertext) return -1;

    /* Header already consumed — payload read MUST complete or stream
     * alignment is permanently lost. Retry EAGAIN (coord_recv returns -1
     * with errno==EAGAIN if recvd==0 on first byte). */
    int payload_retries = 0;
    while (coord_recv(ml, ciphertext, ct_len) < 0) {
        if ((errno == EAGAIN || errno == EWOULDBLOCK) && ++payload_retries <= 300) {
            vTaskDelay(pdMS_TO_TICKS(10));
            continue;
        }
        ESP_LOGE(TAG, "noise_recv payload failed: ct_len=%d retries=%d errno=%d",
                 ct_len, payload_retries, errno);
        free(ciphertext);
        return -1;
    }

    if (ml_noise_decrypt(noise->rx_key, noise->rx_nonce,
                          NULL, 0,
                          ciphertext, ct_len,
                          plaintext) != ESP_OK) {
        ESP_LOGE(TAG, "Noise decrypt failed (nonce=%llu)", (unsigned long long)noise->rx_nonce);
        free(ciphertext);
        return -1;
    }
    noise->rx_nonce++;

    free(ciphertext);
    return (int)pt_len;
}

/* ============================================================================
 * State: DNS_RESOLVE + TCP_CONNECT
 * ========================================================================== */

static int do_tcp_connect(microlink_t *ml) {
    int64_t t_start = esp_timer_get_time();

    /* Drop any TLS context left from a previous connection before reconnecting. */
    coord_tls_free(ml);

    /* Control-plane port: explicit override, else 443 with TLS or 80 without. */
    int port = ml->config.ctrl_port ? ml->config.ctrl_port
                                     : (ml->config.ctrl_tls ? 443 : 80);
    char port_str[8];
    snprintf(port_str, sizeof(port_str), "%d", port);

    ESP_LOGI(TAG, "Resolving %s...", CTRL_HOST(ml));

    struct addrinfo hints = { .ai_family = AF_UNSPEC, .ai_socktype = SOCK_STREAM };
    struct addrinfo *res = NULL;

    if (ml_getaddrinfo(CTRL_HOST(ml), port_str, &hints, &res) != 0 || !res) {
        ESP_LOGE(TAG, "DNS resolve failed for %s", CTRL_HOST(ml));
        return -1;
    }

    int64_t t_dns = esp_timer_get_time();
    ESP_LOGI(TAG, "[TIMING] DNS resolve: %lld ms", (t_dns - t_start) / 1000);

    int sock = ml_socket(res->ai_family, SOCK_STREAM, IPPROTO_TCP);
    if (sock < 0) {
        ml_freeaddrinfo(res);
        ESP_LOGE(TAG, "Socket creation failed");
        return -1;
    }

    /* Socket timeouts */
    struct timeval tv = { .tv_sec = 10, .tv_usec = 0 };
    ml_setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    ml_setsockopt(sock, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));

    /* TCP keepalive (critical for NAT traversal) */
    int keepalive = 1;
    int keepidle = 25;
    int keepintvl = 10;
    int keepcnt = 3;
    ml_setsockopt(sock, SOL_SOCKET, SO_KEEPALIVE, &keepalive, sizeof(keepalive));
    ml_setsockopt(sock, IPPROTO_TCP, TCP_KEEPIDLE, &keepidle, sizeof(keepidle));
    ml_setsockopt(sock, IPPROTO_TCP, TCP_KEEPINTVL, &keepintvl, sizeof(keepintvl));
    ml_setsockopt(sock, IPPROTO_TCP, TCP_KEEPCNT, &keepcnt, sizeof(keepcnt));

    ESP_LOGI(TAG, "Connecting to %s:%d...", CTRL_HOST(ml), port);

    if (ml_connect(sock, res->ai_addr, res->ai_addrlen) < 0) {
        ESP_LOGE(TAG, "TCP connect failed: %d", errno);
        ml_close_sock(sock);
        ml_freeaddrinfo(res);
        return -1;
    }
    ml_freeaddrinfo(res);

    int64_t t_tcp = esp_timer_get_time();
    ESP_LOGI(TAG, "[TIMING] TCP connect: %lld ms (total: %lld ms)",
             (t_tcp - t_dns) / 1000, (t_tcp - t_start) / 1000);

    ml->coord_sock = sock;

    /* Verify the coordinator TLS endpoint as well as its Noise identity. */
    if (ml->config.ctrl_tls) {
        /* mbedTLS 4 (ESP-IDF 6): RNG comes from PSA, not ctr_drbg/entropy. */
        if (psa_crypto_init() != PSA_SUCCESS) {
            ESP_LOGE(TAG, "psa_crypto_init failed");
            ml_close_sock(sock);
            ml->coord_sock = -1;
            return ESP_FAIL;
        }
        mbedtls_ssl_init(&ml->coord_ssl);
        mbedtls_ssl_config_init(&ml->coord_ssl_conf);
        ml->coord_tls_up = true;  /* context now needs freeing on the next reconnect */

        if (mbedtls_ssl_config_defaults(&ml->coord_ssl_conf, MBEDTLS_SSL_IS_CLIENT,
                MBEDTLS_SSL_TRANSPORT_STREAM, MBEDTLS_SSL_PRESET_DEFAULT) != 0 ||
            esp_crt_bundle_attach(&ml->coord_ssl_conf) != ESP_OK) {
            ml_close_sock(sock);
            ml->coord_sock = -1;
            coord_tls_free(ml);
            return -1;
        }
        mbedtls_ssl_conf_authmode(&ml->coord_ssl_conf, MBEDTLS_SSL_VERIFY_REQUIRED);
        /* mbedTLS 4 removed mbedtls_ssl_conf_rng(); the SSL layer uses PSA RNG. */
        mbedtls_ssl_conf_read_timeout(&ml->coord_ssl_conf, COORD_TLS_TIMEOUT_MS);
        if (mbedtls_ssl_setup(&ml->coord_ssl, &ml->coord_ssl_conf) != 0 ||
            mbedtls_ssl_set_hostname(&ml->coord_ssl, CTRL_HOST(ml)) != 0) {
            ml_close_sock(sock);
            ml->coord_sock = -1;
            coord_tls_free(ml);
            return -1;
        }
        mbedtls_ssl_set_bio(&ml->coord_ssl, &ml->coord_sock, coord_bio_send, NULL, coord_bio_recv);

        int64_t t_tls_start = esp_timer_get_time();
        int ret;
        while ((ret = mbedtls_ssl_handshake(&ml->coord_ssl)) != 0) {
            if (ret == MBEDTLS_ERR_SSL_WANT_READ || ret == MBEDTLS_ERR_SSL_WANT_WRITE) continue;
            char eb[128];
            mbedtls_strerror(ret, eb, sizeof(eb));
            ESP_LOGE(TAG, "coord TLS handshake failed: %s", eb);
            ml_close_sock(sock);
            ml->coord_sock = -1;
            coord_tls_free(ml);
            return -1;
        }
        ESP_LOGI(TAG, "[TIMING] coord TLS handshake: %lld ms",
                 (esp_timer_get_time() - t_tls_start) / 1000);
        ESP_LOGI(TAG, "coord TLS established with %s", CTRL_HOST(ml));
    }

    return 0;
}

/* ============================================================================
 * State: NOISE_HANDSHAKE (HTTP/1.1 Upgrade with Noise msg1)
 * ========================================================================== */

/* Server proactive data (stored between handshake and H2 preface) */
static uint8_t *s_server_extra_data = NULL;
static int s_server_extra_data_len = 0;

/* The Noise msg2 frame: the body bytes after the upgrade response's header,
 * then the rest of the frame from the socket. The body can also hold proactive
 * frames after msg2, so *buf_len can be more than the frame. */
static uint8_t *coord_read_msg2(microlink_t *ml, const uint8_t *body, int body_len, int *buf_len) {
    /* A TLS record can end inside the 3-byte frame header. */
    uint8_t *buf = ml_psram_malloc(body_len > 3 ? body_len : 3);
    if (!buf) return NULL;
    if (body_len > 0) memcpy(buf, body, body_len);
    int len = body_len;

    if (len < 3) {
        ESP_LOGI(TAG, "Reading Noise msg2 header from socket (have %d bytes)...", len);
        if (coord_recv(ml, buf + len, 3 - len) < 0) {
            ESP_LOGE(TAG, "Failed to read Noise msg2 header");
            free(buf);
            return NULL;
        }
        len = 3;
    }

    uint8_t msg_type = buf[0];
    uint16_t payload_len = (buf[1] << 8) | buf[2];

    ESP_LOGI(TAG, "Noise frame: type=0x%02x, payload_len=%d", msg_type, payload_len);

    if (msg_type != 0x02) {
        ESP_LOGE(TAG, "Unexpected Noise msg type: 0x%02x (expected 0x02)", msg_type);
        free(buf);
        return NULL;
    }

    int msg2_total = 3 + payload_len;

    if (len < msg2_total) {
        ESP_LOGI(TAG, "Reading remaining msg2 payload: have %d, need %d", len, msg2_total);
        uint8_t *grown = ml_psram_malloc(msg2_total);
        if (!grown) { free(buf); return NULL; }
        memcpy(grown, buf, len);
        free(buf);
        buf = grown;

        if (coord_recv(ml, buf + len, msg2_total - len) < 0) {
            ESP_LOGE(TAG, "Failed to read Noise msg2 payload");
            free(buf);
            return NULL;
        }
        len = msg2_total;
    }

    ESP_LOGI(TAG, "Noise msg2 complete: %d bytes (payload=%d)", msg2_total, payload_len);
    *buf_len = len;
    return buf;
}

static int do_noise_handshake(microlink_t *ml, ml_noise_state_t *noise) {
    int64_t t_noise_start = esp_timer_get_time();

    /* Discover the coordinator's current key over the configured transport.
     * HTTPS certificate verification protects hosted/custom key discovery. */
    char url[192];
    unsigned port = ml->config.ctrl_port ? ml->config.ctrl_port : ml->config.ctrl_tls ? 443 : 80;
    snprintf(url, sizeof(url), "%s://%s:%u/key?v=%u", ml->config.ctrl_tls ? "https" : "http",
             CTRL_HOST(ml), port, ML_CTRL_PROTOCOL_VER);
    esp_http_client_config_t cfg = {
        .url = url, .timeout_ms = 10000, .crt_bundle_attach = esp_crt_bundle_attach,
        .buffer_size = 1024, .disable_auto_redirect = true,
    };
    esp_http_client_handle_t http = esp_http_client_init(&cfg);
    if (!http) return -1;
    char response[512];
    int length = -1;
    if (esp_http_client_open(http, 0) == ESP_OK && esp_http_client_fetch_headers(http) >= 0 &&
        esp_http_client_get_status_code(http) == 200)
        length = esp_http_client_read_response(http, response, sizeof(response) - 1);
    esp_http_client_cleanup(http);
    if (length <= 0 || length >= sizeof(response) - 1) return -1;
    response[length] = '\0';
    cJSON *keys = cJSON_Parse(response);
    const cJSON *key = cJSON_GetObjectItemCaseSensitive(keys, "publicKey");
    uint8_t remote_key[32];
    bool valid = cJSON_IsString(key) && strlen(key->valuestring) == 69 &&
        !strncmp(key->valuestring, "mkey:", 5);
    for (unsigned i = 0; valid && i < 32; ++i) {
        unsigned byte;
        char pair[3] = {key->valuestring[5 + i * 2], key->valuestring[6 + i * 2], '\0'};
        valid = strspn(pair, "0123456789abcdefABCDEF") == 2 && sscanf(pair, "%2x", &byte) == 1;
        remote_key[i] = (uint8_t)byte;
    }
    cJSON_Delete(keys);
    if (!valid) { ESP_LOGE(TAG, "Invalid coordination public key"); return -1; }
    /* Initialize Noise state with our machine key and the discovered server key. */
    ml_noise_init(noise,
                   ml->machine_private_key, ml->machine_public_key,
                   remote_key);

    /* Build Noise message 1 (101 bytes) */
    uint8_t msg1[128];
    size_t msg1_len = 0;
    if (ml_noise_write_msg1(noise, msg1, &msg1_len) != ESP_OK) {
        ESP_LOGE(TAG, "Failed to build Noise msg1");
        return -1;
    }

    /* Base64 encode msg1 for HTTP header */
    char msg1_b64[256];
    size_t b64_len = 0;
    mbedtls_base64_encode((unsigned char *)msg1_b64, sizeof(msg1_b64), &b64_len,
                           msg1, msg1_len);
    msg1_b64[b64_len] = '\0';

    /* Send HTTP/1.1 Upgrade request with Noise msg1 in header */
    char *http_req = ml_psram_malloc(512 + b64_len);
    if (!http_req) return -1;

    int req_len = snprintf(http_req, 512 + b64_len,
        "POST /ts2021 HTTP/1.1\r\n"
        "Host: %s\r\n"
        "Upgrade: tailscale-control-protocol\r\n"
        "Connection: Upgrade\r\n"
        "User-Agent: Tailscale\r\n"
        "X-Tailscale-Handshake: %s\r\n"
        "Content-Length: 0\r\n"
        "\r\n",
        CTRL_HOST(ml), msg1_b64);

    ESP_LOGI(TAG, "Sending Noise handshake (msg1=%d bytes, b64=%d chars)", (int)msg1_len, (int)b64_len);

    if (coord_send(ml, (uint8_t *)http_req, req_len) < 0) {
        free(http_req);
        return -1;
    }
    free(http_req);

    /* Read HTTP response - use large recv() because server often sends
     * HTTP 101 headers + Noise msg2 + proactive frames in the same TCP segment */
    uint8_t *resp = ml_psram_malloc(2048);
    if (!resp) return -1;

    /* A server or proxy may split the header over several TLS records:
     * read until its terminator is there. */
    int total = 0;
    while (total < 2047) {
        int n = coord_read_some(ml, resp + total, 2047 - total);
        if (n <= 0) {
            if (total > 0) break;
            ESP_LOGE(TAG, "Handshake recv failed: %d (errno=%d)", n, errno);
            free(resp);
            return -1;
        }
        total += n;
        resp[total] = '\0';
        if (strstr((char *)resp, "\r\n\r\n")) break;
    }
    resp[total] = '\0';

    ESP_LOGI(TAG, "HTTP response received: %d bytes", total);

    /* Verify 101 Switching Protocols */
    if (strstr((char *)resp, "101") == NULL) {
        ESP_LOGE(TAG, "Noise upgrade rejected: %.200s", resp);
        free(resp);
        return -1;
    }
    ESP_LOGI(TAG, "HTTP 101 received, looking for Noise msg2...");

    /* Find end of HTTP headers */
    uint8_t *body_start = (uint8_t *)strstr((char *)resp, "\r\n\r\n");
    if (!body_start) {
        ESP_LOGE(TAG, "No header terminator found");
        free(resp);
        return -1;
    }
    body_start += 4; /* Skip past \r\n\r\n */

    int body_len = total - (body_start - resp);
    ESP_LOGI(TAG, "Body after HTTP headers: %d bytes", body_len);

    /* The body may contain msg2 + extra frames */
    int body_buf_len = 0;
    uint8_t *body_buf = coord_read_msg2(ml, body_start, body_len, &body_buf_len);
    free(resp);
    if (!body_buf) return -1;
    uint16_t payload_len = (body_buf[1] << 8) | body_buf[2];
    int msg2_total = 3 + payload_len;  /* Total msg2 frame size */

    /* Strip 3-byte header, pass raw payload to noise processing */
    if (ml_noise_read_msg2(noise, body_buf + 3, payload_len) != ESP_OK) {
        ESP_LOGE(TAG, "Noise msg2 processing failed");
        free(body_buf);
        return -1;
    }

    int64_t t_noise_done = esp_timer_get_time();
    ESP_LOGI(TAG, "[TIMING] Noise handshake: %lld ms", (t_noise_done - t_noise_start) / 1000);
    ESP_LOGI(TAG, "Noise handshake complete, transport keys derived");

    /* Save extra data after msg2 (proactive server transport frames)
     * Server sends these immediately after handshake - each one increments
     * server's tx_nonce. We must process them to keep nonces in sync.
     * (Matches v1 g_server_extra_data logic)
     *
     * On fast connections (WiFi), the proactive frames often arrive in
     * the same TCP segment as the HTTP 101 + msg2. On slow connections
     * (cellular PPP), they arrive in separate TCP segments. We must
     * read from the socket if they weren't in the initial buffer. */
    int extra_len = body_buf_len - msg2_total;
    uint8_t *extra_data = NULL;

    if (extra_len > 0) {
        /* Fast path: proactive frames were in the initial recv buffer */
        ESP_LOGI(TAG, "Found %d bytes of extra data after msg2 (proactive frames)", extra_len);
        extra_data = ml_psram_malloc(extra_len);
        if (extra_data) {
            memcpy(extra_data, body_buf + msg2_total, extra_len);
        }
    } else {
        /* Slow path: proactive frames arrive in subsequent TCP segments.
         * Set a short recv timeout and read them from the socket.
         * Server sends EarlyNoise immediately after msg2, so they should
         * arrive within a few hundred ms even on slow cellular links. */
        ESP_LOGI(TAG, "No extra data in initial buffer, reading proactive frames from socket...");
        struct timeval short_tv = { .tv_sec = 2, .tv_usec = 0 };
        ml_setsockopt(ml->coord_sock, SOL_SOCKET, SO_RCVTIMEO, &short_tv, sizeof(short_tv));

        extra_data = ml_psram_malloc(1024);
        if (extra_data) {
            int n = coord_read_some(ml, extra_data, 1024);
            if (n > 0) {
                extra_len = n;
                ESP_LOGI(TAG, "Read %d bytes of proactive frames from socket", extra_len);
            } else {
                ESP_LOGI(TAG, "No proactive frames from server (n=%d errno=%d)", n, errno);
                free(extra_data);
                extra_data = NULL;
                extra_len = 0;
            }
        }

        /* Restore normal recv timeout */
        struct timeval normal_tv = { .tv_sec = 60, .tv_usec = 0 };
        ml_setsockopt(ml->coord_sock, SOL_SOCKET, SO_RCVTIMEO, &normal_tv, sizeof(normal_tv));
    }

    /* A read can end inside a proactive frame. Read the rest of that frame,
     * so that the frames and the server's nonce stay in step. */
    while (extra_data && extra_len > 0) {
        int offset = 0;
        while (offset + 3 <= extra_len &&
               offset + 3 + ((extra_data[offset + 1] << 8) | extra_data[offset + 2]) <= extra_len)
            offset += 3 + ((extra_data[offset + 1] << 8) | extra_data[offset + 2]);
        if (offset == extra_len) break;
        int need = offset + 3 <= extra_len ?
            offset + 3 + ((extra_data[offset + 1] << 8) | extra_data[offset + 2]) : offset + 3;
        uint8_t *grown = ml_psram_malloc(need);
        if (grown) memcpy(grown, extra_data, extra_len);
        if (!grown || coord_recv(ml, grown + extra_len, need - extra_len) < 0) {
            ESP_LOGE(TAG, "Could not read the rest of a proactive frame");
            free(grown);
            free(extra_data);
            free(body_buf);
            return -1;
        }
        free(extra_data);
        extra_data = grown;
        extra_len = need;
    }

    if (extra_data && extra_len > 0) {
        /* Count proactive transport frames */
        int offset = 0;
        int frame_count = 0;
        while (offset + 3 <= extra_len) {
            uint8_t ft = extra_data[offset];
            uint16_t fl = (extra_data[offset + 1] << 8) | extra_data[offset + 2];
            if (offset + 3 + fl > extra_len) {
                ESP_LOGW(TAG, "Incomplete proactive frame at offset %d", offset);
                break;
            }
            ESP_LOGI(TAG, "Proactive frame %d: type=0x%02x, len=%u", frame_count, ft, fl);
            frame_count++;
            offset += 3 + fl;
        }
        ESP_LOGI(TAG, "Server sent %d proactive transport frames", frame_count);

        /* Store for processing after handshake */
        if (s_server_extra_data) free(s_server_extra_data);
        s_server_extra_data = extra_data;
        s_server_extra_data_len = extra_len;
    } else {
        if (extra_data) free(extra_data);
    }

    free(body_buf);
    return 0;
}

/* ============================================================================
 * Process proactive server frames sent after Noise handshake
 * Copied from v1: decrypt EarlyNoise (frame 0) to get nodeKeyChallenge,
 * then count all frames and set rx_nonce = total count.
 * Frames 1-3 may not decrypt with any nonce/key combo (v1 observation).
 * ========================================================================== */

static void process_proactive_frames(microlink_t *ml, ml_noise_state_t *noise) {
    if (!s_server_extra_data || s_server_extra_data_len <= 0) {
        return;
    }

    ESP_LOGI(TAG, "Processing %d bytes of server proactive data", s_server_extra_data_len);
    s_has_node_key_challenge = false;

    /* Decrypt ALL proactive frames and accumulate plaintext.
     * EarlyNoise content (magic + length + JSON) is split across
     * multiple Noise transport frames:
     *   Frame 0: \xff\xff\xffTS (5 bytes magic)
     *   Frame 1: 4 bytes (big-endian JSON length)
     *   Frame 2: ~95 bytes (JSON with nodeKeyChallenge)
     *   Frame 3: H2 SETTINGS (optional) */
    uint8_t *combined = ml_psram_malloc(1024);
    size_t combined_len = 0;
    int offset = 0;
    int frame_count = 0;
    uint64_t nonce = 0;

    while (offset + 3 <= s_server_extra_data_len) {
        uint8_t ft = s_server_extra_data[offset];
        uint16_t fl = (s_server_extra_data[offset + 1] << 8) | s_server_extra_data[offset + 2];

        if (offset + 3 + fl > s_server_extra_data_len) break;

        ESP_LOGI(TAG, "Proactive frame %d: type=0x%02x, len=%u", frame_count, ft, fl);

        if (ft == 0x04 && fl >= ML_NOISE_MAC_LEN && combined) {
            size_t pt_len = fl - ML_NOISE_MAC_LEN;
            uint8_t *plaintext = ml_psram_malloc(pt_len + 1);

            if (plaintext) {
                esp_err_t ret = ml_noise_decrypt(noise->rx_key, nonce,
                                                  NULL, 0,
                                                  s_server_extra_data + offset + 3, fl,
                                                  plaintext);
                if (ret == ESP_OK) {
                    ESP_LOGI(TAG, "  Decrypted frame %d: %d bytes (nonce=%d)",
                             frame_count, (int)pt_len, (int)nonce);
                    if (combined_len + pt_len < 1024) {
                        memcpy(combined + combined_len, plaintext, pt_len);
                        combined_len += pt_len;
                    }
                } else {
                    ESP_LOGW(TAG, "  Frame %d decrypt failed (nonce=%d)", frame_count, (int)nonce);
                }
                free(plaintext);
            }
        }

        nonce++;
        frame_count++;
        offset += 3 + fl;
    }

    ESP_LOGI(TAG, "Decrypted %d bytes from %d proactive frames", (int)combined_len, frame_count);

    /* Search combined plaintext for nodeKeyChallenge JSON */
    if (combined && combined_len > 0) {
        combined[combined_len < 1024 ? combined_len : 1023] = '\0';

        /* Find JSON start (skip magic + length prefix) */
        char *json_start = NULL;
        for (int i = 0; i < (int)combined_len; i++) {
            if (combined[i] == '{') {
                json_start = (char *)combined + i;
                break;
            }
        }

        if (json_start) {
            ESP_LOGI(TAG, "EarlyNoise JSON: %.80s", json_start);
            cJSON *early_json = cJSON_Parse(json_start);
            if (early_json) {
                cJSON *challenge = cJSON_GetObjectItem(early_json, "nodeKeyChallenge");
                if (challenge && cJSON_IsString(challenge)) {
                    const char *chal_str = challenge->valuestring;
                    ESP_LOGI(TAG, "Found nodeKeyChallenge: %.40s...", chal_str);

                    /* Parse "chalpub:hex..." format (v1 lines 1402-1424) */
                    if (strncmp(chal_str, "chalpub:", 8) == 0) {
                        const char *hex = chal_str + 8;
                        if (strlen(hex) >= 64) {
                            hex_to_bytes(hex, s_node_key_challenge, 32);
                            s_has_node_key_challenge = true;
                            ESP_LOGI(TAG, "NodeKeyChallenge decoded (32 bytes)");
                        }
                    }
                }
                cJSON_Delete(early_json);
            }
        }
    }
    if (combined) free(combined);

    /* Set rx_nonce to skip all proactive frames */
    ESP_LOGI(TAG, "Setting rx_nonce=%d (skipping %d proactive frames)", frame_count, frame_count);
    noise->rx_nonce = frame_count;

    free(s_server_extra_data);
    s_server_extra_data = NULL;
    s_server_extra_data_len = 0;
}

/* ============================================================================
 * State: H2_PREFACE - Send HTTP/2 connection preface (Noise-encrypted)
 * ========================================================================== */

static int do_h2_preface(microlink_t *ml, ml_noise_state_t *noise) {
    int64_t t_h2_start = esp_timer_get_time();

    /* Build H2 preface (24+6=30) + SETTINGS with INITIAL_WINDOW_SIZE (9+6=15)
     * + SETTINGS_ACK (9) + connection-level WINDOW_UPDATE (13) = 67 bytes */
    uint8_t h2_init[128];
    int pos = 0;

    int preface_len = ml_h2_build_preface(h2_init, sizeof(h2_init));
    if (preface_len < 0) return -1;
    pos = preface_len;

    int ack_len = ml_h2_build_settings_ack(h2_init + pos, sizeof(h2_init) - pos);
    if (ack_len < 0) return -1;
    pos += ack_len;

    /* Connection-level WINDOW_UPDATE (stream 0) to expand the connection window
     * beyond the 65535 default. SETTINGS INITIAL_WINDOW_SIZE only sets per-stream
     * window; the connection-level window starts at 65535 and must be explicitly
     * expanded with WINDOW_UPDATE on stream 0. */
    uint32_t conn_window_delta = ML_H2_BUFFER_SIZE - 65535;
    if (conn_window_delta > 0) {
        int wu_len = ml_h2_build_window_update(h2_init + pos, sizeof(h2_init) - pos,
                                                0, conn_window_delta);
        if (wu_len > 0) pos += wu_len;
    }

    /* Encrypt and send as one Noise frame */
    if (noise_send(ml, noise, h2_init, pos) < 0) {
        ESP_LOGE(TAG, "Failed to send H2 preface");
        return -1;
    }

    ESP_LOGI(TAG, "H2 preface sent (%d bytes, conn window=%luKB)",
             pos, (unsigned long)(ML_H2_BUFFER_SIZE / 1024));

    /* Read and process server's response (SETTINGS, SETTINGS_ACK, WINDOW_UPDATE, etc.) */
    uint8_t recv_buf[4096];
    int recv_len = noise_recv(ml, noise, recv_buf, sizeof(recv_buf));
    if (recv_len < 0) {
        ESP_LOGW(TAG, "No immediate H2 response from server (may come later)");
        /* Not fatal - server may send its frames later */
    } else {
        ESP_LOGI(TAG, "Server H2 response: %d bytes", recv_len);
    }

    int64_t t_h2_done = esp_timer_get_time();
    ESP_LOGI(TAG, "[TIMING] H2 preface: %lld ms", (t_h2_done - t_h2_start) / 1000);

    return 0;
}

/* ============================================================================
 * State: REGISTER - Send RegisterRequest, parse RegisterResponse
 * ========================================================================== */

/* 0 registered, 2 waiting for the login at the returned URL, 1 refused
 * otherwise, -1 no answer. */
static int do_register(microlink_t *ml, ml_noise_state_t *noise) {
    int64_t t_reg_start = esp_timer_get_time();

    /* Build RegisterRequest JSON */
    cJSON *root = cJSON_CreateObject();
    if (!root) return -1;

    cJSON_AddNumberToObject(root, "Version", ML_CTRL_PROTOCOL_VER);

    /* NodeKey: "nodekey:" + hex(wg_public_key) */
    char key_hex[65];
    char key_str[80];
    bytes_to_hex(ml->wg_public_key, 32, key_hex);
    snprintf(key_str, sizeof(key_str), "nodekey:%s", key_hex);
    cJSON_AddStringToObject(root, "NodeKey", key_str);

    /* Auth - only include if auth_key is valid (matching v1 behavior) */
    if (!ml->registration_completed && ml->config.auth_key && strlen(ml->config.auth_key) > 0) {
        cJSON *auth = cJSON_CreateObject();
        cJSON_AddStringToObject(auth, "AuthKey", ml->config.auth_key);
        cJSON_AddItemToObject(root, "Auth", auth);
    }

    /* Hostinfo */
    cJSON *hostinfo = cJSON_CreateObject();
    const char *dev_name = (ml->config.device_name && ml->config.device_name[0]) ? ml->config.device_name : microlink_default_device_name();
    cJSON_AddStringToObject(hostinfo, "Hostname", dev_name);
    cJSON_AddStringToObject(hostinfo, "OS", "linux");
    cJSON_AddStringToObject(hostinfo, "OSVersion", "ESP-IDF");
    cJSON_AddStringToObject(hostinfo, "GoArch", "arm");

    /* NetInfo inside Hostinfo — control plane reads PreferredDERP from here
     * to populate Node.HomeDERP for other peers */
    {
        cJSON *netinfo = cJSON_CreateObject();
        if (netinfo) {
            cJSON_AddNumberToObject(netinfo, "PreferredDERP", ml->derp_home_region ? ml->derp_home_region : ML_DERP_REGION);
            cJSON_AddItemToObject(hostinfo, "NetInfo", netinfo);
        }
    }

    cJSON_AddItemToObject(root, "Hostinfo", hostinfo);

    /* NodeKeyChallengeResponse - prove we own the WireGuard private key
     * Server sends challenge public key in EarlyNoise; we respond with
     * X25519(wg_private_key, challenge_public_key). (v1 lines 1754-1785) */
    if (s_has_node_key_challenge) {
        ESP_LOGI(TAG, "Computing NodeKeyChallengeResponse...");

        /* X25519(wg_private_key, challenge_public_key) */
        uint8_t challenge_pub_copy[32];
        memcpy(challenge_pub_copy, s_node_key_challenge, 32);
        challenge_pub_copy[31] &= 0x7F;  /* Clear high bit per RFC 7748 */

        uint8_t challenge_response[32];
        x25519(challenge_response, ml->wg_private_key, challenge_pub_copy, 1);

        /* Encode as "chalresp:hex..." */
        char chalresp_hex[65];
        bytes_to_hex(challenge_response, 32, chalresp_hex);
        char chalresp_str[80];
        snprintf(chalresp_str, sizeof(chalresp_str), "chalresp:%s", chalresp_hex);
        cJSON_AddStringToObject(root, "NodeKeyChallengeResponse", chalresp_str);
        ESP_LOGI(TAG, "Added NodeKeyChallengeResponse");
    } else {
        ESP_LOGW(TAG, "No nodeKeyChallenge received - registration may fail!");
    }

    char *json_str = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (!json_str) return -1;

    size_t json_len = strlen(json_str);
    ESP_LOGI(TAG, "RegisterRequest: %d bytes", (int)json_len);

    /* Build HTTP/2 HEADERS + DATA frames */
    uint8_t *h2_buf = ml_psram_malloc(json_len + 512);
    if (!h2_buf) { free(json_str); return -1; }

    int h2_pos = 0;

    /* HEADERS frame (POST /machine/register) */
    int hdr_len = ml_h2_build_headers_frame(h2_buf + h2_pos, json_len + 512 - h2_pos,
                                              "POST", "/machine/register",
                                              CTRL_HOST(ml), "application/json",
                                              1, false);
    if (hdr_len < 0) { free(json_str); free(h2_buf); return -1; }
    h2_pos += hdr_len;

    /* DATA frame (JSON body, END_STREAM) */
    int data_len = ml_h2_build_data_frame(h2_buf + h2_pos, json_len + 512 - h2_pos,
                                            (uint8_t *)json_str, json_len,
                                            1, true);
    free(json_str);
    if (data_len < 0) { free(h2_buf); return -1; }
    h2_pos += data_len;

    /* Encrypt and send as one Noise frame */
    if (noise_send(ml, noise, h2_buf, h2_pos) < 0) {
        free(h2_buf);
        return -1;
    }
    free(h2_buf);

    int64_t t_reg_sent = esp_timer_get_time();
    ESP_LOGI(TAG, "RegisterRequest sent (%d H2 bytes) [TIMING] send: %lld ms",
             h2_pos, (t_reg_sent - t_reg_start) / 1000);

    /* Read RegisterResponse - accumulate all Noise frames into H2 buffer first,
     * then parse H2 frames (same pattern as MapResponse). */
    uint8_t *h2_resp = ml_psram_malloc(16384);
    if (!h2_resp) return -1;
    size_t h2_resp_len = 0;

    uint8_t *resp_buf = ml_psram_malloc(8192);
    if (!resp_buf) { free(h2_resp); return -1; }
    size_t resp_total = 0;
    /* Flow control counts the DATA payload with its padding. */
    size_t window_total = 0;

    /* Accumulate Noise frames into H2 buffer.
     * Scan each frame for H2 END_STREAM on stream 1 to break early
     * instead of blocking 60s waiting for more data that won't come. */
    bool got_register_end = false;
    for (int frame_count = 0; frame_count < 10 && !got_register_end; frame_count++) {
        uint8_t *frame_buf = ml_psram_malloc(4096);
        if (!frame_buf) break;

        int frame_len = noise_recv(ml, noise, frame_buf, 4096);
        if (frame_len <= 0) {
            free(frame_buf);
            break;
        }

        ESP_LOGI(TAG, "RegisterResponse Noise frame %d: %d bytes", frame_count, frame_len);

        if (h2_resp_len + frame_len < 16384) {
            memcpy(h2_resp + h2_resp_len, frame_buf, frame_len);
            h2_resp_len += frame_len;
        }
        free(frame_buf);

        /* Scan accumulated buffer for H2 END_STREAM on stream 1 */
        int scan = 0;
        while (scan + 9 <= (int)h2_resp_len) {
            uint32_t fl = (h2_resp[scan] << 16) | (h2_resp[scan+1] << 8) | h2_resp[scan+2];
            uint8_t ft = h2_resp[scan+3];
            uint8_t ff = h2_resp[scan+4];
            uint32_t fs = ((h2_resp[scan+5] & 0x7F) << 24) | (h2_resp[scan+6] << 16) |
                          (h2_resp[scan+7] << 8) | h2_resp[scan+8];
            if (fl > 1000000 || scan + 9 + (int)fl > (int)h2_resp_len) break;
            /* END_STREAM (0x01) on stream 1 in DATA(0x00) or HEADERS(0x01) frame */
            if (fs == 1 && (ft == 0x00 || ft == 0x01) && (ff & 0x01)) {
                got_register_end = true;
                break;
            }
            scan += 9 + fl;
        }
    }

    /* Parse H2 frames from accumulated buffer */
    bool got_end_stream = false;
    int fpos = 0;
    while (fpos + 9 <= (int)h2_resp_len) {
        uint32_t f_len = (h2_resp[fpos] << 16) | (h2_resp[fpos + 1] << 8) | h2_resp[fpos + 2];
        uint8_t f_type = h2_resp[fpos + 3];
        uint8_t f_flags = h2_resp[fpos + 4];
        uint32_t f_stream = ((h2_resp[fpos + 5] & 0x7F) << 24) | (h2_resp[fpos + 6] << 16) |
                             (h2_resp[fpos + 7] << 8) | h2_resp[fpos + 8];
        fpos += 9;

        ESP_LOGD(TAG, "  Register H2 frame: type=%d flags=0x%02x len=%lu stream=%lu",
                 f_type, f_flags, (unsigned long)f_len, (unsigned long)f_stream);

        if (f_len > 1000000 || fpos + (int)f_len > (int)h2_resp_len) {
            ESP_LOGW(TAG, "  Invalid H2 frame length %lu at pos %d, stopping", (unsigned long)f_len, fpos - 9);
            break;
        }

        /* Only process frames on stream 1 (our RegisterResponse).
         * Stream 0 = connection-level (SETTINGS, WINDOW_UPDATE, PING) */
        if (f_stream == 1) {
            if (f_type == 0x00 && f_len > 0) {  /* DATA frame */
                const uint8_t *data = h2_resp + fpos;
                size_t data_len = f_len;
                window_total += f_len;
                if (!ml_h2_data_payload(f_flags, &data, &data_len)) {
                    ESP_LOGW(TAG, "  Invalid H2 DATA padding at pos %d, stopping", fpos - 9);
                    break;
                }
                if (resp_total + data_len < 8192) {
                    memcpy(resp_buf + resp_total, data, data_len);
                    resp_total += data_len;
                }
                if (f_flags & 0x01) got_end_stream = true;
            }
            if (f_type == 0x01 && (f_flags & 0x01)) got_end_stream = true;
        }

        fpos += f_len;
    }
    free(h2_resp);

    /* Send connection-level WINDOW_UPDATE for RegisterResponse.
     * Stream 1 is closed (END_STREAM received), only update connection level. */
    if (window_total > 0) {
        uint8_t wu_buf[13];
        int wu_len = ml_h2_build_window_update(wu_buf, 13, 0, (uint32_t)window_total);
        noise_send(ml, noise, wu_buf, wu_len);
    }

    ESP_LOGI(TAG, "RegisterResponse: %d bytes total data", (int)resp_total);

    if (resp_total == 0) {
        ESP_LOGW(TAG, "No DATA frame in RegisterResponse");
        free(resp_buf);
        return -1;
    }

    uint8_t *json_data = resp_buf;
    size_t json_data_len = resp_total;

    /* Skip the length prefix if the length says there is one, the same way the
       MapResponse does. The registration reply has arrived as bare JSON in every
       capture so far, and the arithmetic says so rather than assuming it. */
    char *parse_start = (char *)json_data;
    size_t parse_len = json_data_len;
    const size_t json_offset = coord_json_offset(json_data, json_data_len);
    if (json_offset > 0) {
        ESP_LOGI(TAG, "RegisterResponse JSON starts at offset %d (declared length prefix)", (int)json_offset);
        parse_start += json_offset;
        parse_len -= json_offset;
    }
    if (parse_len == 0 || parse_start[0] != '{') {
        ESP_LOGW(TAG, "RegisterResponse does not open with '{' at offset %d", (int)json_offset);
        free(resp_buf);
        return -1;
    }

    /* Null-terminate for cJSON */
    char saved = parse_start[parse_len];
    parse_start[parse_len] = '\0';

    cJSON *resp_json = cJSON_Parse(parse_start);
    if (!resp_json) {
        ESP_LOGW(TAG, "Failed to parse RegisterResponse JSON (len=%d)", (int)parse_len);

        parse_start[parse_len] = saved;
        free(resp_buf);
        return -1;
    }
    parse_start[parse_len] = saved;
    free(resp_buf);

    const cJSON *auth_url = cJSON_GetObjectItemCaseSensitive(resp_json, "AuthURL");
    const cJSON *error = cJSON_GetObjectItemCaseSensitive(resp_json, "Error");
    const cJSON *authorized = cJSON_GetObjectItemCaseSensitive(resp_json, "MachineAuthorized");
    const cJSON *expired = cJSON_GetObjectItemCaseSensitive(resp_json, "NodeKeyExpired");
    const char *url = cJSON_IsString(auth_url) ? auth_url->valuestring : "";
    const char *reason = cJSON_IsString(error) ? error->valuestring : "";
    if (cJSON_IsTrue(expired) && !reason[0]) reason = "Node key expired";
    if (!cJSON_IsTrue(authorized) && !url[0] && !reason[0]) reason = "Registration not authorized";
    if (ml->config.auth_callback)
        ml->config.auth_callback(url, reason, ml->config.auth_callback_ctx);
    if (url[0] || reason[0] || !cJSON_IsTrue(authorized)) {
        int result = url[0] ? 2 : 1;
        ml->peer_map_valid = false;
        cJSON_Delete(resp_json);
        /* Never fetch peers or open a tunnel before authorization. */
        return result;
    }
    ml->registration_completed = true;

    /* Extract our VPN IP from Node.Addresses */
    cJSON *node = cJSON_GetObjectItem(resp_json, "Node");
    if (node) {
        uint32_t address = self_ipv4(cJSON_GetObjectItem(node, "Addresses"));
        if (address) {
            ml->vpn_ip = address;
            char ip_str[16];
            microlink_ip_to_str(ml->vpn_ip, ip_str);
            ESP_LOGI(TAG, "Our VPN IP: %s", ip_str);
        }
        /* Our MagicDNS FQDN (Node.Name), used e.g. to name a TLS certificate. */
        cJSON *self_nm = cJSON_GetObjectItem(node, "Name");
        if (self_nm && self_nm->valuestring && self_nm->valuestring[0]) {
            strncpy(ml->self_name, self_nm->valuestring, sizeof(ml->self_name) - 1);
            ml->self_name[sizeof(ml->self_name) - 1] = '\0';
            size_t nl = strlen(ml->self_name);
            if (nl > 0 && ml->self_name[nl - 1] == '.') {
                ml->self_name[nl - 1] = '\0';
            }
            ESP_LOGI(TAG, "Our tailnet name: %s", ml->self_name);
        }
        /* Parse self-node DERP region — try modern HomeDERP (int) first,
         * then fall back to legacy DERP string (format: "127.3.3.40:REGION") */
        cJSON *home_derp = cJSON_GetObjectItem(node, "HomeDERP");
        if (home_derp && cJSON_IsNumber(home_derp) && home_derp->valueint > 0) {
            ml->derp_home_region = (uint16_t)home_derp->valueint;
            ESP_LOGI(TAG, "Home DERP region: %d (from server, HomeDERP)", ml->derp_home_region);
        } else {
            cJSON *self_derp = cJSON_GetObjectItem(node, "DERP");
            if (self_derp && self_derp->valuestring) {
                ESP_LOGI(TAG, "Self-Node DERP: %s", self_derp->valuestring);
                const char *colon = strrchr(self_derp->valuestring, ':');
                if (colon) {
                    int region = atoi(colon + 1);
                    if (region > 0) {
                        ml->derp_home_region = (uint16_t)region;
                        ESP_LOGI(TAG, "Home DERP region: %d (from server, legacy DERP)", region);
                    }
                }
            }
        }
        /* Fallback: if server didn't assign a DERP region, use our configured default */
        if (ml->derp_home_region == 0) {
            ml->derp_home_region = ML_DERP_REGION;
            ESP_LOGI(TAG, "Home DERP region: %d (default)", ML_DERP_REGION);
        }
        cJSON *self_key = cJSON_GetObjectItem(node, "Key");
        if (self_key && self_key->valuestring) {
            ESP_LOGI(TAG, "Self-Node Key (from server): %s", self_key->valuestring);
            char our_key_hex[65];
            bytes_to_hex(ml->wg_public_key, 32, our_key_hex);
            ESP_LOGI(TAG, "Our WG pubkey (local):       nodekey:%s", our_key_hex);
        }
    }

    cJSON_Delete(resp_json);

    int64_t t_reg_done = esp_timer_get_time();
    ESP_LOGI(TAG, "[TIMING] Register total: %lld ms", (t_reg_done - t_reg_start) / 1000);

    return 0;
}

/* ============================================================================
 * State: FETCH_PEERS - Send MapRequest, parse MapResponse
 * ========================================================================== */

static bool collect_peer_update(const ml_peer_update_t *update, void *ctx)
{
    return ml_peer_batch_add(ctx, update);
}

/* The updates of one response go to the WireGuard task as one queue item:
 * before registration ends the task does not take any, and a map of any
 * size still fits. */
static bool send_peer_batch(microlink_t *ml, ml_peer_batch_t *batch)
{
    if (!batch) return true;
    for (unsigned i = 0; i < batch->count; ++i) batch->updates[i].generation = ml->peer_map_generation;
    if (xQueueSend(ml->peer_update_queue, &batch, pdMS_TO_TICKS(100)) == pdTRUE) return true;
    free(batch);
    return false;
}

/* The node's IPv4 tailnet address, wherever it is in Node.Addresses: a
 * dual-stack server may list the IPv6 address first. 0 without one. */
static uint32_t self_ipv4(const cJSON *addresses)
{
    const cJSON *item;
    cJSON_ArrayForEach(item, addresses) {
        unsigned a, b, c, d;
        if (cJSON_IsString(item) &&
            sscanf(item->valuestring, "%u.%u.%u.%u", &a, &b, &c, &d) == 4 &&
            a <= 255 && b <= 255 && c <= 255 && d <= 255)
            return (a << 24) | (b << 16) | (c << 8) | d;
    }
    return 0;
}

static bool parse_peers_from_map_response(microlink_t *ml, cJSON *root, bool initial)
{
    const cJSON *full = cJSON_GetObjectItemCaseSensitive(root, "Peers");
    /* A new complete map replaces the peers in place (ML_PEER_SYNC): the
     * peers in the map before keep their WireGuard sessions, and the tunnel
     * stays open while the new map is applied. */
    if (initial || (cJSON_IsArray(full) && cJSON_GetArraySize(full) > 0)) {
        ++ml->peer_map_generation;
    }
    xSemaphoreTake(ml->filter_mutex, portMAX_DELAY);
    bool expiry_valid = ml_key_expiry_update(&ml->key_lifetime, root, ml_get_time_ms());
    if (!expiry_valid)
        ESP_LOGE(TAG, "Invalid node expiry; tunnel traffic blocked");
    bool filter_valid = ml_packet_filter_update(&ml->packet_filters, root);
    if (!filter_valid)
        ESP_LOGE(TAG, "Invalid packet filter; incoming traffic blocked");
    xSemaphoreGive(ml->filter_mutex);
    ml_peer_batch_t *batch = NULL;
    bool peers_valid = ml_peer_map_apply(root, initial, collect_peer_update, &batch);
    /* The updates before an invalid one still apply. */
    peers_valid = send_peer_batch(ml, batch) && peers_valid;
    if (!peers_valid) {
        ml->peer_map_failed_generation = ml->peer_map_generation;
        ml->peer_map_valid = false;
        ESP_LOGE(TAG, "Invalid or incomplete peer update; tunnel traffic blocked");
    }
    return expiry_valid && filter_valid && peers_valid;
}

/* Add Endpoints + EndpointTypes arrays to a MapRequest JSON object.
 * Includes: WiFi LAN endpoint (type=Local), STUN IPv4 (type=STUN),
 * STUN IPv6 (type=STUN). Returns number of endpoints added. */
static int add_endpoints_to_json(microlink_t *ml, cJSON *root) {
    int count = 0;
    cJSON *ep_array = cJSON_AddArrayToObject(root, "Endpoints");
    cJSON *et_array = cJSON_AddArrayToObject(root, "EndpointTypes");
    if (!ep_array || !et_array) return 0;

    /* Advertise the active LAN interface on Ethernet and Wi-Fi boards. */
    esp_netif_t *sta_netif = esp_netif_get_default_netif();
    if (sta_netif) {
        esp_netif_ip_info_t ip_info;
        if (esp_netif_get_ip_info(sta_netif, &ip_info) == ESP_OK && ip_info.ip.addr != 0) {
            uint32_t local_ip = ntohl(ip_info.ip.addr);
            char ep_str[32];
            snprintf(ep_str, sizeof(ep_str), "%lu.%lu.%lu.%lu:%u",
                     (unsigned long)((local_ip >> 24) & 0xFF),
                     (unsigned long)((local_ip >> 16) & 0xFF),
                     (unsigned long)((local_ip >> 8) & 0xFF),
                     (unsigned long)(local_ip & 0xFF),
                     ml->disco_local_port);
            cJSON_AddItemToArray(ep_array, cJSON_CreateString(ep_str));
            cJSON_AddItemToArray(et_array, cJSON_CreateNumber(1));  /* EndpointLocal */
            count++;
        }
    }

    /* PPP endpoint (cellular) — use STUN-discovered public IP as our endpoint */
    /* (PPP netif doesn't have a useful local IP for peers — it's behind CGNAT) */

    /* STUN public endpoint (IPv4) */
    if (ml->stun_public_ip != 0) {
        uint32_t pub_ip = ml->stun_public_ip;
        uint16_t pub_port = ml->stun_public_port ? ml->stun_public_port : ml->disco_local_port;
        char ep_str[32];
        snprintf(ep_str, sizeof(ep_str), "%lu.%lu.%lu.%lu:%u",
                 (unsigned long)((pub_ip >> 24) & 0xFF),
                 (unsigned long)((pub_ip >> 16) & 0xFF),
                 (unsigned long)((pub_ip >> 8) & 0xFF),
                 (unsigned long)(pub_ip & 0xFF),
                 pub_port);
        cJSON_AddItemToArray(ep_array, cJSON_CreateString(ep_str));
        cJSON_AddItemToArray(et_array, cJSON_CreateNumber(2));  /* EndpointSTUN */
        count++;
    }

    /* STUN public endpoint (IPv6) — only include if it's a real IPv6 address,
     * not an IPv4-mapped IPv6 (::ffff:x.x.x.x) which is redundant with IPv4. */
    if (ml->stun_has_ipv6) {
        const uint8_t *a = ml->stun_public_ip6;
        /* Check for IPv4-mapped prefix: first 10 bytes zero, bytes 10-11 = 0xff */
        static const uint8_t v4mapped_prefix[12] = {0,0,0,0, 0,0,0,0, 0,0,0xff,0xff};
        bool is_v4mapped = (memcmp(a, v4mapped_prefix, 12) == 0);
        if (!is_v4mapped) {
            uint16_t port6 = ml->stun_public_port6 ? ml->stun_public_port6 : ml->disco_local_port;
            char ep6_str[64];
            snprintf(ep6_str, sizeof(ep6_str),
                     "[%02x%02x:%02x%02x:%02x%02x:%02x%02x:%02x%02x:%02x%02x:%02x%02x:%02x%02x]:%u",
                     a[0], a[1], a[2], a[3], a[4], a[5], a[6], a[7],
                     a[8], a[9], a[10], a[11], a[12], a[13], a[14], a[15],
                     port6);
            cJSON_AddItemToArray(ep_array, cJSON_CreateString(ep6_str));
            cJSON_AddItemToArray(et_array, cJSON_CreateNumber(2));  /* EndpointSTUN */
            count++;
        }
    }

    if (count > 0) {
        ESP_LOGI(TAG, "MapRequest includes %d endpoint(s)", count);
    }
    return count;
}

static int do_fetch_peers(microlink_t *ml, ml_noise_state_t *noise) {
    int64_t t_map_start = esp_timer_get_time();

    /* Build MapRequest JSON */
    cJSON *root = cJSON_CreateObject();
    if (!root) return -1;

    cJSON_AddNumberToObject(root, "Version", ML_CTRL_PROTOCOL_VER);

    char key_hex[65];
    char key_str[80];
    bytes_to_hex(ml->wg_public_key, 32, key_hex);
    snprintf(key_str, sizeof(key_str), "nodekey:%s", key_hex);
    cJSON_AddStringToObject(root, "NodeKey", key_str);

    /* DiscoKey */
    bytes_to_hex(ml->disco_public_key, 32, key_hex);
    snprintf(key_str, sizeof(key_str), "discokey:%s", key_hex);
    cJSON_AddStringToObject(root, "DiscoKey", key_str);

    /* Modern servers serve complete maps on streaming requests. */
    cJSON_AddBoolToObject(root, "Stream", true);
    cJSON_AddBoolToObject(root, "KeepAlive", true);
    cJSON_AddStringToObject(root, "Compress", "");  /* Disable compression */

    /* Hostinfo */
    cJSON *hostinfo = cJSON_CreateObject();
    const char *dev_name = (ml->config.device_name && ml->config.device_name[0]) ? ml->config.device_name : microlink_default_device_name();
    cJSON_AddStringToObject(hostinfo, "Hostname", dev_name);
    cJSON_AddStringToObject(hostinfo, "OS", "linux");
    cJSON_AddStringToObject(hostinfo, "OSVersion", "ESP-IDF");
    cJSON_AddStringToObject(hostinfo, "GoArch", "arm");
    cJSON_AddItemToObject(root, "Hostinfo", hostinfo);

    /* NetInfo: tell control plane our preferred DERP region and NAT type.
     * MUST be inside Hostinfo — the control plane reads Hostinfo.NetInfo.PreferredDERP
     * to populate Node.HomeDERP for other peers. */
    cJSON *netinfo = cJSON_CreateObject();
    if (netinfo) {
        cJSON_AddNumberToObject(netinfo, "PreferredDERP", ml->derp_home_region ? ml->derp_home_region : ML_DERP_REGION);
        if (ml->stun_nat_checked) {
            cJSON_AddBoolToObject(netinfo, "MappingVariesByDestIP", ml->nat_mapping_varies);
        }
        cJSON_AddItemToObject(hostinfo, "NetInfo", netinfo);
    }

    /* Include endpoints if STUN has already completed (Stream=false →
     * control plane processes these, unlike Stream=true with Version >= 68) */
    add_endpoints_to_json(ml, root);

    char *json_str = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (!json_str) return -1;

    size_t json_len = strlen(json_str);
    ESP_LOGI(TAG, "MapRequest: %d bytes (Stream=true, initial snapshot)", (int)json_len);

    /* Build H2 HEADERS + DATA, stream ID 3 (stream 1 was register) */
    uint8_t *h2_buf = ml_psram_malloc(json_len + 512);
    if (!h2_buf) { free(json_str); return -1; }

    int h2_pos = 0;

    int hdr_len = ml_h2_build_headers_frame(h2_buf + h2_pos, json_len + 512,
                                              "POST", "/machine/map",
                                              CTRL_HOST(ml), "application/json",
                                              3, false);
    if (hdr_len < 0) { free(json_str); free(h2_buf); return -1; }
    h2_pos += hdr_len;

    int data_len = ml_h2_build_data_frame(h2_buf + h2_pos, json_len + 512 - h2_pos,
                                            (uint8_t *)json_str, json_len,
                                            3, true);
    free(json_str);
    if (data_len < 0) { free(h2_buf); return -1; }
    h2_pos += data_len;

    if (noise_send(ml, noise, h2_buf, h2_pos) < 0) {
        free(h2_buf);
        return -1;
    }
    free(h2_buf);

    int64_t t_map_sent = esp_timer_get_time();
    ESP_LOGI(TAG, "[TIMING] MapRequest send: %lld ms", (t_map_sent - t_map_start) / 1000);

    /* Read the first length-prefixed map on stream 3. A streaming reply has
     * no END_STREAM after its initial map; waiting for it stalls startup. */
    uint8_t *h2_recv = ml_psram_malloc(ML_H2_BUFFER_SIZE);
    uint8_t *resp_buf = ml_psram_malloc(ML_JSON_BUFFER_SIZE);
    uint8_t *frame_buf = ml_psram_malloc(65536);
    if (!h2_recv || !resp_buf || !frame_buf) {
        free(h2_recv); free(resp_buf); free(frame_buf);
        return -1;
    }
    size_t h2_total = 0, json_total = 0, window_consumed = 0;
    struct timeval rcv_tv = { .tv_sec = 60, .tv_usec = 0 };
    ml_setsockopt(ml->coord_sock, SOL_SOCKET, SO_RCVTIMEO, &rcv_tv, sizeof(rcv_tv));
    for (int count = 0; count < 200; ++count) {
        int received = noise_recv(ml, noise, frame_buf, 65536);
        if (received <= 0 || (size_t)received > ML_H2_BUFFER_SIZE - h2_total) break;
        memcpy(h2_recv + h2_total, frame_buf, received);
        h2_total += received;
        window_consumed += received;
        int result = ml_h2_first_message(h2_recv, h2_total, 3, resp_buf,
                                        ML_JSON_BUFFER_SIZE, &json_total);
        if (result != 0) break;
        if (window_consumed >= 32768) {
            uint8_t wu[26];
            int size = ml_h2_build_window_update(wu, 13, 0, window_consumed);
            size += ml_h2_build_window_update(wu + size, 13, 3, window_consumed);
            if (noise_send(ml, noise, wu, size) < 0) break;
            window_consumed = 0;
        }
    }
    free(h2_recv);
    free(frame_buf);
    rcv_tv.tv_sec = 5;
    ml_setsockopt(ml->coord_sock, SOL_SOCKET, SO_RCVTIMEO, &rcv_tv, sizeof(rcv_tv));
    /* Cancel the initial stream before opening stream 5 for incremental maps.
     * RST_STREAM with CANCEL (RFC 7540 section 7) leaves the connection open. */
    const uint8_t cancel[] = {0, 0, 4, 3, 0, 0, 0, 0, 3, 0, 0, 0, 8};
    if (noise_send(ml, noise, cancel, sizeof(cancel)) < 0) {
        free(resp_buf);
        return -1;
    }
    if (window_consumed) {
        uint8_t wu[13];
        int size = ml_h2_build_window_update(wu, sizeof(wu), 0, window_consumed);
        noise_send(ml, noise, wu, size);
    }

    if (json_total == 0) {
        ESP_LOGW(TAG, "Empty MapResponse");
        free(resp_buf);
        return -1;
    }

    ESP_LOGI(TAG, "MapResponse JSON: %d bytes", (int)json_total);

    /* Hex dump first 32 bytes for debugging prefix issues */
    {
        int dump = json_total < 32 ? (int)json_total : 32;
        char hexbuf[97];
        for (int i = 0; i < dump; i++) {
            sprintf(hexbuf + i * 3, "%02x ", resp_buf[i]);
        }
        hexbuf[dump * 3] = '\0';
        ESP_LOGI(TAG, "MapResponse first %d bytes (hex): %s", dump, hexbuf);
    }

    /* Skip the length prefix if the length says there is one (Tailscale binary
       framing: 4-byte little-endian length before the JSON). Read rather than
       sniffed for; see coord_json_offset. */
    char *parse_start = (char *)resp_buf;
    size_t parse_len = json_total;

    const size_t json_offset = coord_json_offset(resp_buf, json_total);
    if (json_offset > 0) {
        ESP_LOGI(TAG, "JSON starts at offset %d (declared length prefix)", (int)json_offset);
        parse_start += json_offset;
        parse_len -= json_offset;
    }
    if (parse_len == 0 || parse_start[0] != '{') {
        ESP_LOGW(TAG, "MapResponse does not open with '{' at offset %d", (int)json_offset);
    }

    /* Null-terminate */
    char saved = parse_start[parse_len];
    parse_start[parse_len] = '\0';

    cJSON *map_json = cJSON_Parse(parse_start);
    parse_start[parse_len] = saved;

    if (!map_json) {
        const char *err = cJSON_GetErrorPtr();
        ESP_LOGE(TAG, "MapResponse JSON parse failed near: %.50s", err ? err : "unknown");
        free(resp_buf);
        return -1;
    }

    /* Debug: log all top-level fields in MapResponse (from v1 lines 3053-3072) */
    {
        ESP_LOGI(TAG, "MapResponse top-level fields:");
        cJSON *field = NULL;
        cJSON_ArrayForEach(field, map_json) {
            if (field->string) {
                if (cJSON_IsArray(field))
                    ESP_LOGI(TAG, "  - %s (array, size=%d)", field->string, cJSON_GetArraySize(field));
                else if (cJSON_IsObject(field))
                    ESP_LOGI(TAG, "  - %s (object)", field->string);
                else if (cJSON_IsString(field))
                    ESP_LOGI(TAG, "  - %s (string): %.40s", field->string, field->valuestring);
                else if (cJSON_IsNumber(field))
                    ESP_LOGI(TAG, "  - %s (number): %g", field->string, field->valuedouble);
                else if (cJSON_IsBool(field))
                    ESP_LOGI(TAG, "  - %s (bool): %s", field->string, cJSON_IsTrue(field) ? "true" : "false");
                else
                    ESP_LOGI(TAG, "  - %s (other)", field->string);
            }
        }
    }

    /* Extract self-node info */
    {
        cJSON *node = cJSON_GetObjectItem(map_json, "Node");
        if (node) {
            /* Extract VPN IP if not already set */
            if (ml->vpn_ip == 0) {
                uint32_t address = self_ipv4(cJSON_GetObjectItem(node, "Addresses"));
                if (address) {
                    ml->vpn_ip = address;
                    char ip_str[16];
                    microlink_ip_to_str(ml->vpn_ip, ip_str);
                    ESP_LOGI(TAG, "Our VPN IP: %s", ip_str);
                }
            }
            /* Our MagicDNS FQDN (Node.Name), used e.g. to name a TLS certificate. */
            cJSON *self_nm = cJSON_GetObjectItem(node, "Name");
            if (self_nm && self_nm->valuestring && self_nm->valuestring[0]) {
                strncpy(ml->self_name, self_nm->valuestring, sizeof(ml->self_name) - 1);
                ml->self_name[sizeof(ml->self_name) - 1] = '\0';
                size_t nl = strlen(ml->self_name);
                if (nl > 0 && ml->self_name[nl - 1] == '.') {
                    ml->self_name[nl - 1] = '\0';
                }
                ESP_LOGI(TAG, "Our tailnet name: %s", ml->self_name);
            }
            /* Parse self-node DERP region — try modern HomeDERP (int) first,
             * then fall back to legacy DERP string (format: "127.3.3.40:REGION") */
            cJSON *home_derp = cJSON_GetObjectItem(node, "HomeDERP");
            if (home_derp && cJSON_IsNumber(home_derp) && home_derp->valueint > 0) {
                ml->derp_home_region = (uint16_t)home_derp->valueint;
                ESP_LOGI(TAG, "Home DERP region: %d (from server, HomeDERP)", ml->derp_home_region);
            } else {
                cJSON *self_derp = cJSON_GetObjectItem(node, "DERP");
                if (self_derp && self_derp->valuestring) {
                    ESP_LOGI(TAG, "Self-Node DERP: %s", self_derp->valuestring);
                    const char *colon = strrchr(self_derp->valuestring, ':');
                    if (colon) {
                        int region = atoi(colon + 1);
                        if (region > 0) {
                            ml->derp_home_region = (uint16_t)region;
                            ESP_LOGI(TAG, "Home DERP region: %d (from server, legacy DERP)", region);
                        }
                    }
                }
            }
            /* Fallback: if server didn't assign a DERP region, use our configured default */
            if (ml->derp_home_region == 0) {
                ml->derp_home_region = ML_DERP_REGION;
                ESP_LOGI(TAG, "Home DERP region: %d (default)", ML_DERP_REGION);
            }
            cJSON *self_key = cJSON_GetObjectItem(node, "Key");
            if (self_key && self_key->valuestring) {
                ESP_LOGI(TAG, "Self-Node Key (server): %.40s...", self_key->valuestring);
                char our_hex[65];
                bytes_to_hex(ml->wg_public_key, 32, our_hex);
                ESP_LOGI(TAG, "Our WG pubkey (local):  nodekey:%.32s...", our_hex);
            }


        }
    }

    /* Parse peers */
    bool map_valid = parse_peers_from_map_response(ml, map_json, true);
    if (!map_valid) {
        cJSON_Delete(map_json);
        free(resp_buf);
        return -1;
    }

    /* Extract DERPMap if present — parse all regions and nodes */
    cJSON *derp_map = cJSON_GetObjectItem(map_json, "DERPMap");
    if (derp_map) {
        cJSON *regions = cJSON_GetObjectItem(derp_map, "Regions");
        if (regions) {
            ml->derp_region_count = 0;
            cJSON *region_obj;
            cJSON_ArrayForEach(region_obj, regions) {
                if (ml->derp_region_count >= ML_MAX_DERP_REGIONS) break;
                ml_derp_region_t *r = &ml->derp_regions[ml->derp_region_count];
                memset(r, 0, sizeof(*r));

                cJSON *rid = cJSON_GetObjectItem(region_obj, "RegionID");
                if (rid) r->region_id = (uint16_t)rid->valuedouble;

                cJSON *rcode = cJSON_GetObjectItem(region_obj, "RegionCode");
                if (rcode && rcode->valuestring) {
                    strncpy(r->code, rcode->valuestring, sizeof(r->code) - 1);
                }

                cJSON *avoid = cJSON_GetObjectItem(region_obj, "Avoid");
                if (avoid && cJSON_IsTrue(avoid)) r->avoid = true;

                /* Parse nodes */
                cJSON *nodes = cJSON_GetObjectItem(region_obj, "Nodes");
                if (nodes) {
                    cJSON *node_obj;
                    cJSON_ArrayForEach(node_obj, nodes) {
                        if (r->node_count >= ML_MAX_DERP_NODES) break;
                        ml_derp_node_t *n = &r->nodes[r->node_count];
                        memset(n, 0, sizeof(*n));

                        cJSON *hn = cJSON_GetObjectItem(node_obj, "HostName");
                        if (hn && hn->valuestring) {
                            strncpy(n->hostname, hn->valuestring, sizeof(n->hostname) - 1);
                        }

                        cJSON *ip4 = cJSON_GetObjectItem(node_obj, "IPv4");
                        if (ip4 && ip4->valuestring) {
                            strncpy(n->ipv4, ip4->valuestring, sizeof(n->ipv4) - 1);
                        }

                        cJSON *ip6 = cJSON_GetObjectItem(node_obj, "IPv6");
                        if (ip6 && ip6->valuestring) {
                            strncpy(n->ipv6, ip6->valuestring, sizeof(n->ipv6) - 1);
                        }

                        cJSON *sp = cJSON_GetObjectItem(node_obj, "STUNPort");
                        if (sp) n->stun_port = (uint16_t)sp->valuedouble;

                        cJSON *dp = cJSON_GetObjectItem(node_obj, "DERPPort");
                        if (dp) n->derp_port = (uint16_t)dp->valuedouble;

                        cJSON *so = cJSON_GetObjectItem(node_obj, "STUNOnly");
                        if (so && cJSON_IsTrue(so)) n->stun_only = true;

                        r->node_count++;
                    }
                }

                ESP_LOGI(TAG, "  DERP region %d (%s): %d nodes%s",
                         r->region_id, r->code, r->node_count,
                         r->avoid ? " [avoid]" : "");
                for (int ni = 0; ni < r->node_count; ni++) {
                    ESP_LOGI(TAG, "    node: %s (v4=%s v6=%s stun=%d derp=%d%s)",
                             r->nodes[ni].hostname,
                             r->nodes[ni].ipv4[0] ? r->nodes[ni].ipv4 : "-",
                             r->nodes[ni].ipv6[0] ? r->nodes[ni].ipv6 : "-",
                             r->nodes[ni].stun_port ? r->nodes[ni].stun_port : 3478,
                             r->nodes[ni].derp_port ? r->nodes[ni].derp_port : 443,
                             r->nodes[ni].stun_only ? " stun-only" : "");
                }

                ml->derp_region_count++;
            }
            ESP_LOGI(TAG, "DERPMap: parsed %d regions", ml->derp_region_count);

            /*
             * Choose the home DERP region now that the map is known, before the
             * relay is connected (issue #19). The server assigns Dallas by default
             * because that is the PreferredDERP we historically reported, which is
             * a bad relay for anyone outside North America. A configured region id
             * wins; otherwise probe latency and pick the nearest, falling back to
             * Frankfurt (region code "fra") and then whatever we already had.
             */
            if (ml->derp_region_count > 0 && !ml->derp_home_probed) {
                ml->derp_home_probed = true;
                if (ml->config.derp_home_region) {
                    ml->derp_home_region = ml->config.derp_home_region;
                    ESP_LOGI(TAG, "Home DERP region: %u (configured)", ml->derp_home_region);
                } else {
                    uint16_t nearest = ml_stun_pick_nearest_region(ml);
                    if (nearest) {
                        ml->derp_home_region = nearest;
                    } else {
                        uint16_t fra = 0;
                        for (int i = 0; i < ml->derp_region_count; i++) {
                            if (strcmp(ml->derp_regions[i].code, "fra") == 0) {
                                fra = ml->derp_regions[i].region_id;
                                break;
                            }
                        }
                        if (fra) {
                            ml->derp_home_region = fra;
                            ESP_LOGW(TAG, "DERP probe failed; falling back to Frankfurt (region %u)",
                                     fra);
                        } else {
                            ESP_LOGW(TAG, "DERP probe failed, no Frankfurt in map; keeping region %u",
                                     ml->derp_home_region ? ml->derp_home_region : ML_DERP_REGION);
                        }
                    }
                }
            }
        }
    }

    cJSON_Delete(map_json);
    free(resp_buf);

    int64_t t_map_done = esp_timer_get_time();
    ESP_LOGI(TAG, "[TIMING] MapResponse recv+parse: %lld ms (total map: %lld ms, %dKB)",
             (t_map_done - t_map_sent) / 1000,
             (t_map_done - t_map_start) / 1000,
             (int)(h2_total / 1024));

    return 0;
}

/* ============================================================================
 * State: LONG_POLL - Start streaming MapRequest + process incremental updates
 * ========================================================================== */

/* Send MapRequest with Stream=true to start long-poll on H2 stream 5 */
static int do_start_long_poll(microlink_t *ml, ml_noise_state_t *noise) {
    cJSON *root = cJSON_CreateObject();
    if (!root) return -1;

    cJSON_AddNumberToObject(root, "Version", ML_CTRL_PROTOCOL_VER);

    char key_hex[65], key_str[80];
    bytes_to_hex(ml->wg_public_key, 32, key_hex);
    snprintf(key_str, sizeof(key_str), "nodekey:%s", key_hex);
    cJSON_AddStringToObject(root, "NodeKey", key_str);

    bytes_to_hex(ml->disco_public_key, 32, key_hex);
    snprintf(key_str, sizeof(key_str), "discokey:%s", key_hex);
    cJSON_AddStringToObject(root, "DiscoKey", key_str);

    /* Hostinfo - REQUIRED by control plane even for Stream=true.
     * V1 includes this; without it, server may not keep us "online". */
    cJSON *hostinfo = cJSON_CreateObject();
    if (hostinfo) {
        const char *dev_name = (ml->config.device_name && ml->config.device_name[0]) ? ml->config.device_name : microlink_default_device_name();
        cJSON_AddStringToObject(hostinfo, "Hostname", dev_name);
        cJSON_AddStringToObject(hostinfo, "OS", "linux");
        cJSON_AddStringToObject(hostinfo, "OSVersion", "ESP-IDF");
        cJSON_AddStringToObject(hostinfo, "GoArch", "arm");
        cJSON_AddItemToObject(root, "Hostinfo", hostinfo);
    }

    /* NetInfo: tell control plane our preferred DERP region and NAT type.
     * MUST be inside Hostinfo — the control plane reads Hostinfo.NetInfo.PreferredDERP
     * to populate Node.HomeDERP for other peers. */
    cJSON *netinfo = cJSON_CreateObject();
    if (netinfo) {
        cJSON_AddNumberToObject(netinfo, "PreferredDERP", ml->derp_home_region ? ml->derp_home_region : ML_DERP_REGION);
        if (ml->stun_nat_checked) {
            cJSON_AddBoolToObject(netinfo, "MappingVariesByDestIP", ml->nat_mapping_varies);
        }
        cJSON_AddItemToObject(hostinfo, "NetInfo", netinfo);
    }

    /* Stream=true for long-poll, KeepAlive=true so server sends keepalives
     * (which marks us as "online" on the control plane) */
    cJSON_AddBoolToObject(root, "Stream", true);
    cJSON_AddBoolToObject(root, "KeepAlive", true);
    cJSON_AddStringToObject(root, "Compress", "");    /* Disable compression */
    /* No OmitPeers: on a stream it lets the server leave out the peers, and
     * this stream is where peer changes arrive. */

    /* NOTE: With Version >= 68, the control plane IGNORES Endpoints and
     * Hostinfo in Stream=true MapRequests. Endpoints are sent via separate
     * do_send_endpoint_update() calls (Stream=false, OmitPeers=true). */

    char *json_str = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (!json_str) return -1;

    size_t json_len = strlen(json_str);
    ESP_LOGI(TAG, "MapRequest: %d bytes (Stream=true)", (int)json_len);

    /* Build H2 frames on stream ID 5 */
    uint8_t *h2_buf = ml_psram_malloc(json_len + 512);
    if (!h2_buf) { free(json_str); return -1; }

    int h2_pos = 0;
    int hdr_len = ml_h2_build_headers_frame(h2_buf, json_len + 512,
                                              "POST", "/machine/map",
                                              CTRL_HOST(ml), "application/json",
                                              5, false);
    if (hdr_len < 0) { free(json_str); free(h2_buf); return -1; }
    h2_pos += hdr_len;

    int data_len = ml_h2_build_data_frame(h2_buf + h2_pos, json_len + 512 - h2_pos,
                                            (uint8_t *)json_str, json_len,
                                            5, true);
    free(json_str);
    if (data_len < 0) { free(h2_buf); return -1; }
    h2_pos += data_len;

    if (noise_send(ml, noise, h2_buf, h2_pos) < 0) {
        free(h2_buf);
        return -1;
    }
    free(h2_buf);

    ESP_LOGI(TAG, "Streaming MapRequest sent on stream 5");
    return 0;
}

/* Send a "lite" endpoint update to the control plane.
 * This is a non-streaming MapRequest (Stream=false, OmitPeers=true) that
 * only updates our endpoints and hostinfo. Required because Version >= 68
 * means the control plane IGNORES Endpoints in streaming (Stream=true)
 * MapRequests. The reference client uses a dedicated "updateRoutine" for this.
 *
 * Uses H2 stream 7. Response body is discarded (only HTTP status matters).
 * Returns 0 on success, -1 on send failure. */
static int do_send_endpoint_update(microlink_t *ml, ml_noise_state_t *noise) {

    cJSON *root = cJSON_CreateObject();
    if (!root) return -1;

    cJSON_AddNumberToObject(root, "Version", ML_CTRL_PROTOCOL_VER);

    char key_hex[65], key_str[80];
    bytes_to_hex(ml->wg_public_key, 32, key_hex);
    snprintf(key_str, sizeof(key_str), "nodekey:%s", key_hex);
    cJSON_AddStringToObject(root, "NodeKey", key_str);

    bytes_to_hex(ml->disco_public_key, 32, key_hex);
    snprintf(key_str, sizeof(key_str), "discokey:%s", key_hex);
    cJSON_AddStringToObject(root, "DiscoKey", key_str);

    /* Stream=false so control plane PROCESSES our endpoints (Version >= 68) */
    cJSON_AddBoolToObject(root, "Stream", false);
    cJSON_AddBoolToObject(root, "KeepAlive", true);
    /* OmitPeers=true — we don't need peers back, just updating our endpoints */
    cJSON_AddBoolToObject(root, "OmitPeers", true);
    cJSON_AddStringToObject(root, "Compress", "");

    /* Hostinfo (required — control plane reads NetInfo from here) */
    cJSON *hostinfo = cJSON_CreateObject();
    if (hostinfo) {
        const char *dev_name = (ml->config.device_name && ml->config.device_name[0]) ? ml->config.device_name : microlink_default_device_name();
        cJSON_AddStringToObject(hostinfo, "Hostname", dev_name);
        cJSON_AddStringToObject(hostinfo, "OS", "linux");
        cJSON_AddStringToObject(hostinfo, "OSVersion", "ESP-IDF");
        cJSON_AddStringToObject(hostinfo, "GoArch", "arm");
        cJSON_AddItemToObject(root, "Hostinfo", hostinfo);

        cJSON *netinfo = cJSON_CreateObject();
        if (netinfo) {
            cJSON_AddNumberToObject(netinfo, "PreferredDERP", ml->derp_home_region ? ml->derp_home_region : ML_DERP_REGION);
            if (ml->stun_nat_checked) {
                cJSON_AddBoolToObject(netinfo, "MappingVariesByDestIP", ml->nat_mapping_varies);
            }
            cJSON_AddItemToObject(hostinfo, "NetInfo", netinfo);
        }
    }

    /* Endpoints + EndpointTypes */
    int ep_count = add_endpoints_to_json(ml, root);

    char *json_str = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (!json_str) return -1;

    size_t json_len = strlen(json_str);
    ESP_LOGI(TAG, "Endpoint update: %d bytes, %d endpoints (Stream=false, OmitPeers=true)",
             (int)json_len, ep_count);

    /* Use incrementing odd stream IDs (H2 client-initiated = odd).
     * Streams 1, 3, 5 are used by register, fetch peers, and long-poll.
     * Start at 7, increment by 2 for each endpoint update.
     * Reset to 7 on reconnect (h2_next_stream_id initialized in connect). */
    if (ml->h2_next_stream_id < 7) ml->h2_next_stream_id = 7;
    uint32_t sid = ml->h2_next_stream_id;
    ml->h2_next_stream_id += 2;

    uint8_t *h2_buf = ml_psram_malloc(json_len + 512);
    if (!h2_buf) { free(json_str); return -1; }

    int h2_pos = 0;
    int hdr_len = ml_h2_build_headers_frame(h2_buf, json_len + 512,
                                              "POST", "/machine/map",
                                              CTRL_HOST(ml), "application/json",
                                              sid, false);
    if (hdr_len < 0) { free(json_str); free(h2_buf); return -1; }
    h2_pos += hdr_len;

    int data_len = ml_h2_build_data_frame(h2_buf + h2_pos, json_len + 512 - h2_pos,
                                            (uint8_t *)json_str, json_len,
                                            sid, true);  /* END_STREAM */
    free(json_str);
    if (data_len < 0) { free(h2_buf); return -1; }
    h2_pos += data_len;

    if (noise_send(ml, noise, h2_buf, h2_pos) < 0) {
        free(h2_buf);
        ESP_LOGE(TAG, "Failed to send endpoint update");
        return -1;
    }
    free(h2_buf);

    ESP_LOGI(TAG, "Endpoint update sent on H2 stream %lu", (unsigned long)sid);
    /* Response body is discarded — server may send empty response or
     * we'll consume it in the next poll_map_update() iteration.
     * Only the HTTP status code matters (200 = success). */
    return 0;
}

typedef struct {
    microlink_t *ml;
    ml_noise_state_t *noise;
} map_stream_context_t;

static bool map_stream_frame(uint8_t type, uint8_t flags, uint32_t stream,
    const uint8_t *payload, size_t length, void *arg)
{
    map_stream_context_t *ctx = arg;
    uint8_t reply[26];
    int size = 0;
    if (type == 0 && length) {
        size = ml_h2_build_window_update(reply, 13, 0, length);
        size += ml_h2_build_window_update(reply + size, 13, stream, length);
    } else if (type == 6 && length == 8 && !(flags & 1)) {
        const uint8_t header[] = {0,0,8,6,1,0,0,0,0};
        memcpy(reply, header, 9);
        memcpy(reply + 9, payload, 8);
        size = 17;
    } else if (type == 4 && !(flags & 1)) {
        const uint8_t ack[] = {0,0,0,4,1,0,0,0,0};
        memcpy(reply, ack, sizeof(ack));
        size = sizeof(ack);
    }
    return !size || noise_send(ctx->ml, ctx->noise, reply, size) >= 0;
}

static bool map_stream_message(const uint8_t *data, size_t length, void *arg)
{
    map_stream_context_t *ctx = arg;
    cJSON *root = cJSON_ParseWithLengthOpts((const char *)data, length + 1, NULL, true);
    if (!cJSON_IsObject(root)) {
        ctx->ml->peer_map_valid = false;
        cJSON_Delete(root);
        return false;
    }
    ESP_LOGI(TAG, "Long-poll MapResponse update received");
    bool valid = parse_peers_from_map_response(ctx->ml, root, false);
    cJSON_Delete(root);
    return valid;
}

/* Noise records can split H2 headers, DATA frames and control messages at
 * any byte. Keep both framing states until the entire message arrives. */
static int poll_map_update(microlink_t *ml, ml_noise_state_t *noise, ml_h2_stream_t *stream)
{
    fd_set readfds;
    FD_ZERO(&readfds);
    FD_SET(ml->coord_sock, &readfds);
    struct timeval tv = { .tv_sec = 0, .tv_usec = 50000 };
    int selected = ml_select_fds(ml->coord_sock + 1, &readfds, NULL, NULL, &tv);
    if (selected <= 0) return selected;
    struct timeval timeout = { .tv_sec = 2 };
    ml_setsockopt(ml->coord_sock, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
    uint8_t *record = ml_psram_malloc(65536);
    if (!record) return -1;
    int length = noise_recv(ml, noise, record, 65536);
    int saved_errno = errno;
    if (length <= 0) {
        free(record);
        return length < 0 && (saved_errno == EAGAIN || saved_errno == EWOULDBLOCK) ? 0 : length;
    }
    map_stream_context_t context = { .ml = ml, .noise = noise };
    bool valid = ml_h2_stream_feed(stream, record, length,
        map_stream_frame, map_stream_message, &context);
    free(record);
    return valid ? 1 : -1;
}

/* ============================================================================
 * Coordination Task Main Loop
 * ========================================================================== */

void ml_coord_task(void *arg) {
    microlink_t *ml = (microlink_t *)arg;
    ESP_LOGI(TAG, "Coord task started (Core %d)", xPortGetCoreID());

    coord_state_t state = COORD_IDLE;
    ml_coord_cmd_t cmd;
    uint64_t last_activity_ms = ml_get_time_ms();
    int reconnect_attempts = 0;
    /* The last registration returned a login URL: the user may approve it
     * at any moment, so the next registration follows soon. */
    bool login_pending = false;

    /* Noise protocol state - owned exclusively by this task */
    ml_noise_state_t noise = {0};
    ml_h2_stream_t map_stream = { .stream = 5, .frame_capacity = 65536,
        .message_capacity = ML_JSON_BUFFER_SIZE };

    /* Wait for WiFi/cellular OR shutdown */
    ESP_LOGI(TAG, "Waiting for WiFi...");
    {
        EventBits_t wb = xEventGroupWaitBits(ml->events,
                             ML_EVT_WIFI_CONNECTED | ML_EVT_SHUTDOWN_REQUEST,
                             pdFALSE, pdFALSE, portMAX_DELAY);
        if (wb & ML_EVT_SHUTDOWN_REQUEST) {
            ESP_LOGI(TAG, "Shutdown requested before WiFi, exiting");
            xEventGroupSetBits(ml->events, ML_EVT_COORD_EXITED);
    vTaskDeleteWithCaps(NULL);
            return;
        }
    }

    map_stream.frame = ml_psram_malloc(map_stream.frame_capacity);
    map_stream.message = ml_psram_malloc(map_stream.message_capacity);
    if (!map_stream.frame || !map_stream.message) {
        free(map_stream.frame);
        free(map_stream.message);
        ml->state = ML_STATE_ERROR;
        xEventGroupSetBits(ml->events, ML_EVT_COORD_EXITED);
        vTaskDeleteWithCaps(NULL);
        return;
    }

    while (!(xEventGroupGetBits(ml->events) & ML_EVT_SHUTDOWN_REQUEST)) {
        /* Check for commands (non-blocking) */
        if (xQueueReceive(ml->coord_cmd_queue, &cmd, 0) == pdTRUE) {
            switch (cmd) {
            case ML_CMD_CONNECT:
                if (state == COORD_IDLE) {
                    state = COORD_STUN_PROBE;
                    ml->state = ML_STATE_CONNECTING;
                }
                break;
            case ML_CMD_DISCONNECT:
                if (ml->coord_sock >= 0) {
                    ml_close_sock(ml->coord_sock);
                    ml->coord_sock = -1;
                }
                state = COORD_IDLE;
                ml->state = ML_STATE_IDLE;
                break;
            case ML_CMD_FORCE_RECONNECT:
                state = COORD_RECONNECTING;
                break;
            case ML_CMD_UPDATE_ENDPOINTS:
                /* TODO: Send endpoint update on existing H2 connection */
                break;
            }
        }

        /* DERP reconnect is now handled by the DERP I/O task itself.
         * The I/O task checks ML_EVT_DERP_RECONNECT directly. */

        switch (state) {
        case COORD_IDLE:
            vTaskDelay(pdMS_TO_TICKS(100));
            break;

        case COORD_STUN_PROBE:
            ESP_LOGI(TAG, "Running STUN probe...");
            /* Pre-resolve STUN servers (cached, only resolves once) */
            ml_stun_resolve_servers(ml);
            /* Reset retry state for fresh probe sequence */
            ml->stun_retry_count = 1;  /* First probe counts as attempt 1 */
            ml->stun_using_fallback = false;
            ml->stun_last_probe_ms = ml_get_time_ms();
            /* Send first probe to Tailscale STUN primary (IPv4) */
            if (ml->stun_primary_ip) {
                ml_stun_send_probe_to(ml, ml->stun_primary_ip, ML_STUN_PRIMARY_PORT);
            } else if (ml->stun_fallback_ip) {
                ml->stun_using_fallback = true;
                ml_stun_send_probe_to(ml, ml->stun_fallback_ip, ML_STUN_FALLBACK_PORT);
            } else {
                /* Both DNS failed, try hostname-based as last resort */
                ml_stun_send_probe(ml, ML_STUN_PRIMARY_HOST, ML_STUN_PRIMARY_PORT);
            }
            /* Also send IPv6 STUN probe if resolved */
            {
                static const uint8_t zero16[16] = {0};
                if (memcmp(ml->stun_primary_ip6, zero16, 16) != 0) {
                    ml_stun_send_probe_ipv6(ml, ml->stun_primary_ip6, ML_STUN_PRIMARY_PORT);
                }
            }
            state = COORD_DNS_RESOLVE;
            break;

        case COORD_DNS_RESOLVE:
        case COORD_TCP_CONNECT:
            if (do_tcp_connect(ml) < 0) {
                ESP_LOGE(TAG, "TCP connect failed, retrying...");
                state = COORD_RECONNECTING;
                break;
            }
            ml->h2_next_stream_id = 7;  /* Reset H2 stream counter for new connection */
            state = COORD_NOISE_HANDSHAKE;
            break;

        case COORD_NOISE_HANDSHAKE:
            ml->state = ML_STATE_REGISTERING;
            if (do_noise_handshake(ml, &noise) < 0) {
                ESP_LOGE(TAG, "Noise handshake failed");
                ml_close_sock(ml->coord_sock);
                ml->coord_sock = -1;
                state = COORD_RECONNECTING;
                break;
            }
            /* Process proactive server frames (EarlyNoise, H2 SETTINGS)
             * before sending our H2 preface. Extracts nodeKeyChallenge
             * and adjusts rx_nonce. */
            process_proactive_frames(ml, &noise);
            state = COORD_H2_PREFACE;
            break;

        case COORD_H2_PREFACE:
            if (do_h2_preface(ml, &noise) < 0) {
                ESP_LOGE(TAG, "H2 preface failed");
                ml_close_sock(ml->coord_sock);
                ml->coord_sock = -1;
                state = COORD_RECONNECTING;
                break;
            }
            state = COORD_REGISTER;
            break;

        case COORD_REGISTER:
            ESP_LOGI(TAG, "Registering...");
            int registration_result = do_register(ml, &noise);
            /* A registration without an answer keeps the last answer's state. */
            if (registration_result >= 0) login_pending = registration_result == 2;
            if (registration_result != 0) {
                ESP_LOGE(TAG, "Registration failed");
                ml_close_sock(ml->coord_sock);
                ml->coord_sock = -1;
                state = COORD_RECONNECTING;
                break;
            }
            state = COORD_FETCH_PEERS;
            break;

        case COORD_FETCH_PEERS:
            ESP_LOGI(TAG, "Fetching peers...");
            if (do_fetch_peers(ml, &noise) < 0) {
                ESP_LOGW(TAG, "MapRequest failed, will retry");
                ml_close_sock(ml->coord_sock);
                ml->coord_sock = -1;
                state = COORD_RECONNECTING;
                break;
            }

            xEventGroupSetBits(ml->events, ML_EVT_COORD_REGISTERED);

            /* Signal DERP I/O task to connect (connection now owned by I/O task) */
            if (!ml->derp.connected) {
                xEventGroupSetBits(ml->events, ML_EVT_DERP_CONNECT_REQ);
                /* Wait for DERP to connect (up to 15s) before continuing */
                ESP_LOGI(TAG, "Waiting for DERP I/O task to connect...");
                xEventGroupWaitBits(ml->events, ML_EVT_DERP_CONNECTED,
                                    pdFALSE, pdTRUE, pdMS_TO_TICKS(15000));
            }

            /* Start streaming long-poll for incremental updates. The initial map stream is closed by now: without the update
             * stream the node would miss peer and filter changes. */
            if (do_start_long_poll(ml, &noise) < 0) {
                ESP_LOGW(TAG, "Failed to start long-poll, reconnecting");
                ml_close_sock(ml->coord_sock);
                ml->coord_sock = -1;
                state = COORD_RECONNECTING;
                break;
            }

            /* Send initial endpoint update if STUN already completed.
             * (STUN runs concurrently and may have results by now.) */
            do_send_endpoint_update(ml, &noise);

            /* Send initial HTTP/2 PING immediately to keep connection active
             * before the periodic 5s timer kicks in (server has ~20s idle timeout) */
            {
                uint8_t ping_frame[17];
                ping_frame[0] = 0x00; ping_frame[1] = 0x00; ping_frame[2] = 0x08;
                ping_frame[3] = 0x06; ping_frame[4] = 0x00;
                ping_frame[5] = 0x00; ping_frame[6] = 0x00;
                ping_frame[7] = 0x00; ping_frame[8] = 0x00;
                memset(ping_frame + 9, 0x42, 8);
                noise_send(ml, &noise, ping_frame, sizeof(ping_frame));
                ESP_LOGI(TAG, "Sent initial HTTP/2 PING after long-poll");
            }

            state = COORD_LONG_POLL;
            ml->state = ML_STATE_CONNECTED;
            reconnect_attempts = 0;
            last_activity_ms = ml_get_time_ms();

            /* Notify app */
            if (ml->state_cb) {
                ml->state_cb(ml, ML_STATE_CONNECTED, ml->state_cb_data);
            }
            break;

        case COORD_LONG_POLL:
            {
                uint64_t now = ml_get_time_ms();

                /* Check control plane watchdog (120s) */
                if (now - last_activity_ms > ml->t_ctrl_watchdog_ms) {
                    ESP_LOGW(TAG, "Control plane watchdog timeout");
                    state = COORD_RECONNECTING;
                    break;
                }

                /* Check for STUN response in queue (IPv4 or IPv6) */
                ml_rx_packet_t stun_pkt;
                if (xQueueReceive(ml->stun_rx_queue, &stun_pkt, 0) == pdTRUE) {
                    uint32_t pub_ip;
                    uint16_t pub_port;
                    bool parsed = false;

                    /* Try IPv4 parse first */
                    if (ml_stun_parse_response(stun_pkt.data, stun_pkt.len,
                                                &pub_ip, &pub_port)) {
                        if (ml->stun_public_ip == 0) {
                            /* First STUN result (primary server) */
                            ml->stun_public_ip = pub_ip;
                            ml->stun_public_port = pub_port;
                            ml->stun_retry_count = 0;
                            /* Send probe to fallback for symmetric NAT check */
                            if (!ml->stun_nat_checked && ml->stun_fallback_ip) {
                                ml_stun_send_probe_to(ml, ml->stun_fallback_ip,
                                                       ML_STUN_FALLBACK_PORT);
                                ESP_LOGI(TAG, "Sent STUN to fallback for NAT type detection");
                            }
                        } else if (!ml->stun_nat_checked) {
                            /* Second STUN result (fallback) — compare ports */
                            ml->stun_secondary_port = pub_port;
                            ml->stun_nat_checked = true;
                            if (ml->stun_public_port != pub_port) {
                                ml->nat_mapping_varies = true;
                                ESP_LOGW(TAG, "Symmetric NAT detected: port %u vs %u "
                                         "(direct connections unlikely)",
                                         ml->stun_public_port, pub_port);
                            } else {
                                ml->nat_mapping_varies = false;
                                ESP_LOGI(TAG, "Cone NAT: port %u consistent "
                                         "(direct connections possible)", pub_port);
                            }
                        } else {
                            /* Periodic re-probe — update primary result */
                            ml->stun_public_ip = pub_ip;
                            ml->stun_public_port = pub_port;
                            ml->stun_retry_count = 0;
                        }
                        parsed = true;
                    }

                    /* Try IPv6 parse (response may contain IPv6 XOR-MAPPED-ADDRESS) */
                    if (!parsed) {
                        uint8_t ip6[16];
                        uint16_t port6;
                        if (ml_stun_parse_response_ipv6(stun_pkt.data, stun_pkt.len,
                                                         ip6, &port6)) {
                            memcpy(ml->stun_public_ip6, ip6, 16);
                            ml->stun_public_port6 = port6;
                            ml->stun_has_ipv6 = true;
                            /* Use IPv6 port for NAT detection if no IPv4 result yet */
                            if (ml->stun_public_port == 0) {
                                ml->stun_public_port = port6;
                            }
                            ml->stun_retry_count = 0;  /* Got a result, stop retrying */
                            ESP_LOGI(TAG, "STUN IPv6 result obtained, port %u", port6);
                            /* Trigger NAT type check if not done yet */
                            if (!ml->stun_nat_checked && ml->stun_fallback_ip) {
                                ml_stun_send_probe_to(ml, ml->stun_fallback_ip,
                                                       ML_STUN_FALLBACK_PORT);
                                ESP_LOGI(TAG, "Sent STUN to fallback for NAT type detection");
                            }
                            parsed = true;
                        }
                    }

                    if (parsed) {
                        xEventGroupSetBits(ml->events, ML_EVT_STUN_COMPLETE);
                        /* Send endpoint update to control plane immediately.
                         * With Version >= 68, Stream=true MapRequests have endpoints
                         * IGNORED, so we need this separate Stream=false request. */
                        do_send_endpoint_update(ml, &noise);
                    }
                    free(stun_pkt.data);
                }

                /* STUN retry logic: 3 attempts per server, 2s apart, then fallback */
                if (ml->stun_retry_count > 0 && ml->stun_retry_count <= ML_STUN_MAX_RETRIES) {
                    if (now - ml->stun_last_probe_ms > ML_STUN_RETRY_INTERVAL_MS) {
                        if (!ml->stun_using_fallback && ml->stun_primary_ip) {
                            ml_stun_send_probe_to(ml, ml->stun_primary_ip, ML_STUN_PRIMARY_PORT);
                        } else if (ml->stun_fallback_ip) {
                            ml_stun_send_probe_to(ml, ml->stun_fallback_ip, ML_STUN_FALLBACK_PORT);
                        }
                        ml->stun_last_probe_ms = now;
                        ml->stun_retry_count++;
                        if (ml->stun_retry_count > ML_STUN_MAX_RETRIES && !ml->stun_using_fallback) {
                            /* Primary exhausted, switch to fallback */
                            ESP_LOGW(TAG, "STUN primary failed after %d retries, trying fallback",
                                     ML_STUN_MAX_RETRIES);
                            ml->stun_using_fallback = true;
                            ml->stun_retry_count = 1;
                            if (ml->stun_fallback_ip) {
                                ml_stun_send_probe_to(ml, ml->stun_fallback_ip, ML_STUN_FALLBACK_PORT);
                                ml->stun_last_probe_ms = now;
                            }
                        }
                    }
                }

                /* Periodic STUN re-probe (every 23s) — fresh probe sequence */
                static uint64_t last_stun_ms = 0;
                if (now - last_stun_ms > ml->t_stun_interval_ms) {
                    ml->stun_retry_count = 1;
                    ml->stun_using_fallback = false;
                    ml->stun_last_probe_ms = now;
                    /* IPv4 probe */
                    if (ml->stun_primary_ip) {
                        ml_stun_send_probe_to(ml, ml->stun_primary_ip, ML_STUN_PRIMARY_PORT);
                    } else if (ml->stun_fallback_ip) {
                        ml->stun_using_fallback = true;
                        ml_stun_send_probe_to(ml, ml->stun_fallback_ip, ML_STUN_FALLBACK_PORT);
                    } else {
                        ml_stun_send_probe(ml, ML_STUN_PRIMARY_HOST, ML_STUN_PRIMARY_PORT);
                    }
                    /* IPv6 probe (alongside IPv4) */
                    {
                        static const uint8_t zero16[16] = {0};
                        if (memcmp(ml->stun_primary_ip6, zero16, 16) != 0) {
                            ml_stun_send_probe_ipv6(ml, ml->stun_primary_ip6, ML_STUN_PRIMARY_PORT);
                        }
                    }
                    last_stun_ms = now;
                }

                /* Periodic DERP NotePreferred keepalive (every 60s) */
                static uint64_t last_derp_keepalive_ms = 0;
                if (ml->derp.connected && now - last_derp_keepalive_ms > 60000) {
                    uint8_t preferred = 0x01;
                    uint8_t *ka_data = malloc(1);
                    if (ka_data) {
                        *ka_data = preferred;
                        ml_derp_tx_item_t ka_item = {
                            .data = ka_data,
                            .len = 1,
                            .frame_type = 0x07,  /* NotePreferred */
                        };
                        memset(ka_item.dest_pubkey, 0, 32);
                        if (xQueueSend(ml->derp_tx_queue, &ka_item, 0) != pdTRUE) {
                            free(ka_data);
                        }
                    }
                    last_derp_keepalive_ms = now;
                }

                /* Key expiry check (every 60s) — re-register if expired */
                if (ml->key_lifetime.expiry_epoch > 0 || microlink_key_expired(ml)) {
                    static uint64_t last_expiry_check_ms = 0;
                    if (now - last_expiry_check_ms > 60000) {
                        last_expiry_check_ms = now;
                        if (microlink_key_expired(ml)) {
                            if (!ml->registration_completed && ml->config.auth_key && ml->config.auth_key[0]) {
                                ESP_LOGW(TAG, "Key expired, re-registering with auth_key...");
                                state = COORD_RECONNECTING;
                                break;
                            } else {
                                ESP_LOGE(TAG, "Key expired but no auth_key — manual re-provisioning needed!");
                            }
                        }

                    }
                }

                /* Send HTTP/2 PING every 5 seconds to keep control plane alive.
                 * The server has a ~20s idle timeout; PINGs maintain bidirectional
                 * activity and are what actually keep us "online". (v1 reference) */
                static uint64_t last_h2_ping_ms = 0;
                if (now - last_h2_ping_ms >= 5000) {
                    uint8_t ping_frame[17];
                    ping_frame[0] = 0x00; ping_frame[1] = 0x00; ping_frame[2] = 0x08;
                    ping_frame[3] = 0x06;  /* Type: PING */
                    ping_frame[4] = 0x00;  /* Flags: none */
                    ping_frame[5] = 0x00; ping_frame[6] = 0x00;
                    ping_frame[7] = 0x00; ping_frame[8] = 0x00;  /* Stream 0 */
                    /* Opaque 8-byte payload (timestamp for identification) */
                    uint64_t ping_id = now;
                    for (int b = 0; b < 8; b++)
                        ping_frame[9 + b] = (ping_id >> (56 - b * 8)) & 0xFF;
                    int ping_ret = noise_send(ml, &noise, ping_frame, sizeof(ping_frame));
                    if (ping_ret >= 0) {
                        last_h2_ping_ms = now;
                    } else {
                        ESP_LOGW(TAG, "H2 PING send failed, reconnecting");
                        state = COORD_RECONNECTING;
                        break;
                    }
                }

                /* Poll for streaming MapResponse updates */
                int poll_ret = poll_map_update(ml, &noise, &map_stream);
                if (poll_ret > 0) {
                    last_activity_ms = now;  /* Reset watchdog */
                } else if (poll_ret < 0) {
                    ESP_LOGW(TAG, "Long-poll connection lost");
                    state = COORD_RECONNECTING;
                    break;
                }

                vTaskDelay(pdMS_TO_TICKS(50));
            }
            break;

        case COORD_RECONNECTING:
            {
                uint32_t backoff_ms = 1000 << (reconnect_attempts > 4 ? 4 : reconnect_attempts);
                if (backoff_ms > ML_CTRL_BACKOFF_MAX_MS) backoff_ms = ML_CTRL_BACKOFF_MAX_MS;
                if (login_pending && backoff_ms > ML_CTRL_LOGIN_POLL_MS) backoff_ms = ML_CTRL_LOGIN_POLL_MS;

                ESP_LOGI(TAG, "Reconnecting in %lu ms (attempt %d)",
                         (unsigned long)backoff_ms, reconnect_attempts + 1);

                ml->state = ML_STATE_RECONNECTING;

                /* Wait on command queue with backoff timeout (interruptible!) */
                ml_coord_cmd_t wake_cmd;
                if (xQueueReceive(ml->coord_cmd_queue, &wake_cmd, pdMS_TO_TICKS(backoff_ms)) == pdTRUE) {
                    if (wake_cmd == ML_CMD_DISCONNECT) {
                        state = COORD_IDLE;
                        ml->state = ML_STATE_IDLE;
                        break;
                    }
                }

                reconnect_attempts++;

                /* Close old connection */
                if (ml->coord_sock >= 0) {
                    ml_close_sock(ml->coord_sock);
                    ml->coord_sock = -1;
                }

                /* Reset Noise state for fresh handshake */
                memset(&noise, 0, sizeof(noise));
                ml_h2_stream_reset(&map_stream);

                state = COORD_STUN_PROBE;
            }
            break;
        }
    }

    /* Cleanup */
    if (ml->coord_sock >= 0) {
        ml_close_sock(ml->coord_sock);
        ml->coord_sock = -1;
    }
    /* Free the TLS context too: mbedtls_ssl_free zeroizes the session key
     * material, matching how the Noise state is wiped just below and the
     * long-term keys in microlink_destroy. Otherwise a shutdown while connected
     * over TLS would leak the context and leave its keys in freed heap. */
    coord_tls_free(ml);
    memset(&noise, 0, sizeof(noise));

    free(map_stream.frame);
    free(map_stream.message);
    ESP_LOGI(TAG, "Coord task exiting");
    xEventGroupSetBits(ml->events, ML_EVT_COORD_EXITED);
    vTaskDeleteWithCaps(NULL);
}
