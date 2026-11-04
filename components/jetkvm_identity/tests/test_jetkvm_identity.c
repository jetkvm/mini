#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "jetkvm_identity.h"

int main(void)
{
    const uint8_t mac[JETKVM_DEVICE_ID_MAC_BYTES] = {
        0xe8, 0xf6, 0x0a, 0x01, 0xb2, 0xcf,
    };
    char device_id[JETKVM_DEVICE_ID_CAPACITY];
    assert(jetkvm_device_id_format(mac, device_id, sizeof(device_id)));
    assert(strcmp(device_id, "e8f60a01b2cf") == 0);

    char too_small[JETKVM_DEVICE_ID_LENGTH];
    memset(too_small, 'x', sizeof(too_small));
    assert(!jetkvm_device_id_format(mac, too_small, sizeof(too_small)));
    assert(!jetkvm_device_id_format(NULL, device_id, sizeof(device_id)));
    assert(!jetkvm_device_id_format(mac, NULL, sizeof(device_id)));
    puts("JetKVM stable device identity: OK");
    return 0;
}
