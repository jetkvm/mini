#include "jetkvm_tailscale.h"
#include <ctype.h>
#include <stdio.h>
#include <string.h>

const char *jetkvm_tailscale_normalize_url(const char *input, char *output, size_t capacity,
                                         char *host, size_t host_capacity,
                                         bool *tls, uint16_t *port)
{
    if (!input || !output || !capacity || !host || !host_capacity || !tls || !port)
        return "Invalid control URL";
    while (isspace((unsigned char)*input)) ++input;
    size_t length = strlen(input);
    while (length && isspace((unsigned char)input[length - 1])) --length;
    if (!length) {
        input = JETKVM_TAILSCALE_DEFAULT_CONTROL_URL;
        length = strlen(input);
    }
    size_t prefix;
    /* The node fetches the server's Noise key over this connection and then
     * sends the auth key encrypted to it, so the connection must be TLS. */
    if (length >= 8 && strncmp(input, "https://", 8) == 0) { *tls = true; prefix = 8; }
    else return "Control URL must start with https://";
    if (length && input[length - 1] == '/') --length;
    size_t host_end = prefix;
    while (host_end < length && input[host_end] != ':') ++host_end;
    size_t host_length = host_end - prefix;
    if (!host_length || host_length >= host_capacity)
        return "Control URL must include a host of at most 63 characters";
    for (size_t i = prefix; i < host_end; ++i)
        if (!isalnum((unsigned char)input[i]) && input[i] != '.' && input[i] != '-')
            return "Control URL must contain only a host and optional port";
    if (input[prefix] == '.' || input[prefix] == '-' || input[host_end - 1] == '-')
        return "Invalid control server host";
    *port = *tls ? 443 : 80;
    if (host_end < length) {
        unsigned number = 0;
        if (host_end + 1 == length) return "Control URL port is missing";
        for (size_t i = host_end + 1; i < length; ++i) {
            if (!isdigit((unsigned char)input[i])) return "Invalid control URL port";
            number = number * 10 + (unsigned)(input[i] - '0');
            if (number > 65535) return "Control URL port must be between 1 and 65535";
        }
        if (!number) return "Control URL port must be between 1 and 65535";
        *port = (uint16_t)number;
    }
    if (length >= capacity) return "Control URL is too long";
    memcpy(host, input + prefix, host_length);
    host[host_length] = '\0';
    memcpy(output, input, length);
    output[length] = '\0';
    return NULL;
}

const char *jetkvm_tailscale_validate_config(const jetkvm_tailscale_config_t *config)
{
    if (!config || !memchr(config->auth_key, 0, sizeof(config->auth_key)) ||
        !memchr(config->control_url, 0, sizeof(config->control_url))) return "Invalid Tailscale settings";
    char normalized[JETKVM_TAILSCALE_CONTROL_URL_CAPACITY], host[64];
    bool tls;
    uint16_t port;
    const char *error = jetkvm_tailscale_normalize_url(config->control_url, normalized,
        sizeof(normalized), host, sizeof(host), &tls, &port);
    if (error) return error;
    if (config->auth_key[0]) {
        /* Headscale 0.26 preauth keys are 24 random bytes encoded as hex. */
        bool headscale_key = strcmp(normalized, JETKVM_TAILSCALE_DEFAULT_CONTROL_URL) &&
            strlen(config->auth_key) == 48;
        for (const char *p = config->auth_key; headscale_key && *p; ++p)
            headscale_key = isxdigit((unsigned char)*p) != 0;
        if (!headscale_key && strncmp(config->auth_key, "tskey-auth-", 11) != 0)
            return "Expected a Tailscale auth key or a custom server's preauth key";
        for (const char *p = config->auth_key; *p; ++p)
            if (!isalnum((unsigned char)*p) && *p != '-' && *p != '_') return "Invalid Tailscale auth key";
    }
    return NULL;
}
