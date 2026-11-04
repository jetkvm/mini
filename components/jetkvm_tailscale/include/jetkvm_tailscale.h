#pragma once
#include "esp_err.h"
#include "jetkvm_control.h"

#define JETKVM_TAILSCALE_DEFAULT_CONTROL_URL "https://controlplane.tailscale.com"
/* Normalize a root HTTP(S) URL. Empty selects the hosted Tailscale service. */
const char *jetkvm_tailscale_normalize_url(const char *input, char *output, size_t capacity,
                                         char *host, size_t host_capacity,
                                         bool *tls, uint16_t *port);
const char *jetkvm_tailscale_validate_config(const jetkvm_tailscale_config_t *config);
esp_err_t jetkvm_tailscale_init(void);
void jetkvm_tailscale_fill_control_context(jetkvm_tailscale_context_t *context);
