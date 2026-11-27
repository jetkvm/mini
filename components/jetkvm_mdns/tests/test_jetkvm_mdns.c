#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "jetkvm_mdns.h"

static jetkvm_mdns_device_t base(void)
{
    jetkvm_mdns_device_t device = {
        .hostname = "JetKVM-Desk",
        .mode = JETKVM_MDNS_MODE_AUTO,
        .http_port = 80,
        .device_id = "e8f60a01b2cf",
        .version = "0.2.1",
    };
    return device;
}

static void expect_service(const jetkvm_mdns_plan_t *plan, unsigned port, const char *setup)
{
    assert(plan->enabled);
    assert(!strcmp(plan->hostname, "jetkvm-desk"));
    assert(!strcmp(plan->instance, "JetKVM-Desk"));
    assert(plan->port == port);
    /* Upstream's TXT entries, in upstream's order. */
    assert(!strcmp(plan->txt[0].key, "id") && !strcmp(plan->txt[0].value, "e8f60a01b2cf"));
    assert(!strcmp(plan->txt[1].key, "setup") && !strcmp(plan->txt[1].value, setup));
    assert(!strcmp(plan->txt[2].key, "version") && !strcmp(plan->txt[2].value, "0.2.1"));
}

static void test_plan(void)
{
    jetkvm_mdns_device_t device = base();
    jetkvm_mdns_plan_t plan;
    jetkvm_mdns_plan(&device, &plan);
    expect_service(&plan, 80, "false");

    device.setup = true;
    jetkvm_mdns_plan(&device, &plan);
    expect_service(&plan, 80, "true");

    device = base();
    device.https_port = 443;
    jetkvm_mdns_plan(&device, &plan);
    expect_service(&plan, 443, "false");

    device = base();
    device.mode = JETKVM_MDNS_MODE_IPV4_ONLY;
    jetkvm_mdns_plan(&device, &plan);
    expect_service(&plan, 80, "false");

    device.mode = JETKVM_MDNS_MODE_DISABLED;
    jetkvm_mdns_plan(&device, &plan);
    assert(!plan.enabled);

    /* No hostname (the network interface never came up): nothing to answer to. */
    device = base();
    device.hostname[0] = '\0';
    jetkvm_mdns_plan(&device, &plan);
    assert(!plan.enabled);
}

static void test_changes(void)
{
    jetkvm_mdns_device_t device = base();
    jetkvm_mdns_plan_t stopped = {0}, running, wanted;
    jetkvm_mdns_plan(&device, &running);
    assert(jetkvm_mdns_changes(&running, &running) == 0);
    assert(jetkvm_mdns_changes(&stopped, &stopped) == 0);
    assert(jetkvm_mdns_changes(&stopped, &running) == JETKVM_MDNS_START);
    assert(jetkvm_mdns_changes(&running, &stopped) == JETKVM_MDNS_STOP);

    /* Setup completes: only the TXT record changes (upstream UpdateTXT). */
    device.setup = true;
    jetkvm_mdns_plan(&device, &wanted);
    assert(jetkvm_mdns_changes(&running, &wanted) == JETKVM_MDNS_TXT);

    device = base();
    device.https_port = 443;
    jetkvm_mdns_plan(&device, &wanted);
    assert(jetkvm_mdns_changes(&running, &wanted) == JETKVM_MDNS_PORT);

    /* A hostname that differs only in case still renames the instance. */
    device = base();
    strcpy(device.hostname, "jetkvm-desk");
    jetkvm_mdns_plan(&device, &wanted);
    assert(jetkvm_mdns_changes(&running, &wanted) == JETKVM_MDNS_HOSTNAME);

    device = base();
    strcpy(device.hostname, "kvm-lab");
    device.https_port = 443;
    jetkvm_mdns_plan(&device, &wanted);
    assert(jetkvm_mdns_changes(&running, &wanted) == (JETKVM_MDNS_HOSTNAME | JETKVM_MDNS_PORT));

    device = base();
    device.mode = JETKVM_MDNS_MODE_DISABLED;
    jetkvm_mdns_plan(&device, &wanted);
    assert(jetkvm_mdns_changes(&running, &wanted) == JETKVM_MDNS_STOP);
}

int main(void)
{
    test_plan();
    test_changes();
    puts("jetkvm_mdns: OK");
    return 0;
}
