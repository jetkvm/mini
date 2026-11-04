#pragma once
#include "esp_err.h"

#define JETKVM_TLS_CERT_MAX 8192
#define JETKVM_TLS_KEY_MAX 4096
typedef struct {
    unsigned magic;
    char mode[12];
    char certificate[JETKVM_TLS_CERT_MAX + 1];
    char private_key[JETKVM_TLS_KEY_MAX + 1];
} jetkvm_tls_config_t;

/* apply runs in a service worker, outside the RPC/HTTP tasks. The callback
 * retains any listener whose stop failed; the service reapplies old state. */
esp_err_t jetkvm_tls_init(esp_err_t (*apply)(const jetkvm_tls_config_t *));
esp_err_t jetkvm_tls_start(void);
/* Reject new settings and join any pending application before listener teardown. */
void jetkvm_tls_stop(void);
