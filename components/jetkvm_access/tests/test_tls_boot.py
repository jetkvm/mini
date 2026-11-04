#!/usr/bin/env python3
"""Exercise production TLS initialization with fault-injected NVS and crypto."""
from pathlib import Path
import subprocess
import tempfile

component = Path(__file__).resolve().parents[1]
source = (component / 'jetkvm_tls.c').read_text()
init = source[source.index('esp_err_t jetkvm_tls_init('):source.index('esp_err_t jetkvm_tls_start(')]
header = (component / 'include/jetkvm_tls.h').read_text()
record = header[header.index('#define JETKVM_TLS_CERT_MAX'):header.index('/* apply runs')]
harness = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
typedef int esp_err_t;
typedef int nvs_handle_t;
#define ESP_OK 0
#define ESP_ERR_NO_MEM 1
#define ESP_ERR_NVS_NOT_FOUND 2
#define ESP_ERR_INVALID_SIZE 3
#define NVS_READONLY 0
#define TLS_MAGIC 0x314c544aU
#define ESP_LOGW(...) ((void)0)
RECORD
static void *s_mutex;
static jetkvm_tls_config_t *s_config, stored;
static esp_err_t (*s_apply)(const jetkvm_tls_config_t *);
static int open_error, read_error;
static size_t stored_size;
static bool pair_ok, allocation_ok, mutex_ok;
static jetkvm_tls_config_t *allocate_config(void) { return allocation_ok ? calloc(1, sizeof(stored)) : NULL; }
static void destroy(jetkvm_tls_config_t *c) { free(c); }
static void *xSemaphoreCreateMutex(void) { return mutex_ok ? (void *)1 : NULL; }
static int nvs_open(const char *name, int flags, nvs_handle_t *h) {
    assert(!strcmp(name, "jk_access") && flags == NVS_READONLY); *h = 1; return open_error;
}
static int nvs_get_blob(nvs_handle_t h, const char *key, void *out, size_t *size) {
    assert(h == 1 && !strcmp(key, "tls"));
    if (read_error) return read_error;
    memcpy(out, &stored, stored_size < *size ? stored_size : *size);
    *size = stored_size; return ESP_OK;
}
static void nvs_close(nvs_handle_t h) { assert(h == 1); }
static bool valid_pair(const jetkvm_tls_config_t *c) { assert(c == s_config); return pair_ok; }
/* The RPC callbacks are only registered here, not invoked. */
static void get_state(void) {}
static void set_state(void) {}
typedef struct { void (*get)(void); void (*set)(void); } jetkvm_tls_context_t;
static void jetkvm_control_set_default_tls_context(const jetkvm_tls_context_t *ctx) { assert(ctx->get && ctx->set); }
INIT
static void reset(void) {
    free(s_config); s_config = NULL; s_mutex = NULL;
    memset(&stored, 0, sizeof(stored)); stored.magic = TLS_MAGIC;
    strcpy(stored.mode, "custom"); strcpy(stored.certificate, "certificate"); strcpy(stored.private_key, "key");
    stored_size = sizeof(stored); open_error = read_error = 0;
    pair_ok = allocation_ok = mutex_ok = true;
}
static void check(bool enabled) {
    jetkvm_tls_config_t before = stored;
    assert(jetkvm_tls_init(NULL) == ESP_OK);
    assert(s_mutex && s_config->magic == TLS_MAGIC);
    assert(!strcmp(s_config->mode, enabled ? "custom" : "disabled"));
    if (!enabled) assert(!s_config->certificate[0] && !s_config->private_key[0]);
    assert(!memcmp(&before, &stored, sizeof(stored)));
}
int main(void) {
    reset(); check(true);
    reset(); open_error = ESP_ERR_NVS_NOT_FOUND; check(false);
    reset(); read_error = ESP_ERR_NVS_NOT_FOUND; check(false);
    reset(); open_error = 99; check(false);
    reset(); read_error = 99; check(false);
    reset(); stored_size--; check(false);
    reset(); stored_size++; check(false);
    reset(); stored.magic = 0; check(false);
    reset(); memset(stored.mode, 'x', sizeof(stored.mode)); check(false);
    reset(); memset(stored.certificate, 'x', sizeof(stored.certificate)); check(false);
    reset(); memset(stored.private_key, 'x', sizeof(stored.private_key)); check(false);
    reset(); strcpy(stored.mode, "unknown"); check(false);
    reset(); pair_ok = false; check(false);
    reset(); allocation_ok = false; assert(jetkvm_tls_init(NULL) == ESP_ERR_NO_MEM && !s_mutex && !s_config);
    reset(); mutex_ok = false; assert(jetkvm_tls_init(NULL) == ESP_ERR_NO_MEM && !s_mutex && !s_config);
    free(s_config);
    puts("TLS boot recovery and allocation failures: OK");
}
'''.replace('RECORD', record).replace('INIT', init)
with tempfile.TemporaryDirectory() as directory:
    c = Path(directory) / 'boot.c'
    binary = Path(directory) / 'boot'
    c.write_text(harness)
    subprocess.run(['cc', '-std=c11', '-Wall', '-Wextra', '-Werror', '-fsanitize=address,undefined', str(c), '-o', str(binary)], check=True)
    subprocess.run([str(binary)], check=True)
