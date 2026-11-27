#include "jetkvm_mdns.h"
#include <ctype.h>
#include <stdio.h>
#include <string.h>

void jetkvm_mdns_plan(const jetkvm_mdns_device_t *device, jetkvm_mdns_plan_t *plan)
{
    memset(plan, 0, sizeof(*plan));
    if (device->mode == JETKVM_MDNS_MODE_DISABLED || !device->hostname[0]) return;
    plan->enabled = true;
    /* Upstream answers to the lower-case name; the instance keeps the case. */
    for (size_t i = 0; device->hostname[i] && i + 1 < sizeof(plan->hostname); ++i) {
        plan->hostname[i] = (char)tolower((unsigned char)device->hostname[i]);
    }
    snprintf(plan->instance, sizeof(plan->instance), "%s", device->hostname);
    plan->port = device->https_port ? device->https_port : device->http_port;
    plan->txt[0].key = "id";
    snprintf(plan->txt[0].value, sizeof(plan->txt[0].value), "%s", device->device_id);
    plan->txt[1].key = "setup";
    snprintf(plan->txt[1].value, sizeof(plan->txt[1].value), "%s", device->setup ? "true" : "false");
    plan->txt[2].key = "version";
    snprintf(plan->txt[2].value, sizeof(plan->txt[2].value), "%s", device->version);
}

unsigned jetkvm_mdns_changes(const jetkvm_mdns_plan_t *running, const jetkvm_mdns_plan_t *wanted)
{
    if (running->enabled != wanted->enabled) return wanted->enabled ? JETKVM_MDNS_START : JETKVM_MDNS_STOP;
    if (!wanted->enabled) return 0;
    unsigned changes = 0;
    if (strcmp(running->hostname, wanted->hostname) || strcmp(running->instance, wanted->instance))
        changes |= JETKVM_MDNS_HOSTNAME;
    if (running->port != wanted->port) changes |= JETKVM_MDNS_PORT;
    for (size_t i = 0; i < JETKVM_MDNS_TXT_COUNT; ++i) {
        if (strcmp(running->txt[i].value, wanted->txt[i].value)) changes |= JETKVM_MDNS_TXT;
    }
    return changes;
}
