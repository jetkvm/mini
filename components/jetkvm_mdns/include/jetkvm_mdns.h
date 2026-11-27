#pragma once
/*
 * The mDNS responder: <hostname>.local and the DNS-SD service _jetkvm._tcp,
 * as upstream jetkvm/kvm advertises them (#1785). The instance name is the
 * hostname, the port is the HTTPS port when an HTTPS listener runs and the
 * HTTP port otherwise, and the TXT record is id=<device ID>,
 * setup=<true|false>, version=<firmware version>, in that order.
 *
 * The responder uses the predefined interfaces of espressif/mdns: ETH_DEF on
 * the Ethernet board, WIFI_STA_DEF on the Wi-Fi board. It follows their link
 * and address events itself, and with them it answers over IPv6 and with the
 * IPv6 addresses of an interface that has them (the ipv6_mode setting). The
 * mode "ipv4_only" turns that off.
 */
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "jetkvm_control.h"

#ifdef __cplusplus
extern "C" {
#endif

#define JETKVM_MDNS_SERVICE_TYPE "_jetkvm"
#define JETKVM_MDNS_SERVICE_PROTO "_tcp"
#define JETKVM_MDNS_TXT_COUNT 3U
#define JETKVM_MDNS_DEVICE_ID_CAPACITY 33U
#define JETKVM_MDNS_VERSION_CAPACITY 33U

/* What the advertisement is made of. */
typedef struct {
    char hostname[JETKVM_NETWORK_HOSTNAME_CAPACITY];
    uint8_t mode; /* jetkvm_mdns_mode_t */
    bool setup;
    uint16_t http_port;
    uint16_t https_port; /* 0: no HTTPS listener */
    char device_id[JETKVM_MDNS_DEVICE_ID_CAPACITY];
    char version[JETKVM_MDNS_VERSION_CAPACITY];
} jetkvm_mdns_device_t;

typedef struct {
    const char *key;
    char value[JETKVM_MDNS_VERSION_CAPACITY];
} jetkvm_mdns_txt_t;

/* The responder as it must run. */
typedef struct {
    bool enabled;
    /* No answer over IPv6 and no AAAA record (the mode "ipv4_only"). */
    bool ipv4_only;
    char hostname[JETKVM_NETWORK_HOSTNAME_CAPACITY]; /* lower case, without ".local" */
    char instance[JETKVM_NETWORK_HOSTNAME_CAPACITY];
    uint16_t port;
    jetkvm_mdns_txt_t txt[JETKVM_MDNS_TXT_COUNT];
} jetkvm_mdns_plan_t;

void jetkvm_mdns_plan(const jetkvm_mdns_device_t *device, jetkvm_mdns_plan_t *plan);

/* The steps that take a responder from `running` to `wanted`. START and STOP
 * exclude the other steps. */
enum {
    JETKVM_MDNS_START = 1U << 0,
    JETKVM_MDNS_STOP = 1U << 1,
    JETKVM_MDNS_HOSTNAME = 1U << 2,
    JETKVM_MDNS_PORT = 1U << 3,
    JETKVM_MDNS_TXT = 1U << 4,
    JETKVM_MDNS_FAMILIES = 1U << 5,
};
unsigned jetkvm_mdns_changes(const jetkvm_mdns_plan_t *running, const jetkvm_mdns_plan_t *wanted);

/* Device side. Call jetkvm_mdns_init() first; the setters only record their
 * value until jetkvm_mdns_start(), and then update the responder. */
void jetkvm_mdns_init(const char *device_id, const char *version, uint16_t http_port);
void jetkvm_mdns_set_network(const char *hostname, uint8_t mode);
void jetkvm_mdns_set_setup(bool setup);
void jetkvm_mdns_set_https_port(uint16_t port);
void jetkvm_mdns_start(void);
/* The name the responder answers to now, without ".local", and its length; ""
 * and 0 when no responder runs. It is the planned hostname, or that name with
 * the number that espressif/mdns added because another host on the link has
 * the name. */
size_t jetkvm_mdns_get_hostname(char *hostname, size_t capacity);

#ifdef __cplusplus
}
#endif
