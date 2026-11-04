#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "jetkvm_time_sync_servers.h"

int main(void)
{
    char names[4][JETKVM_TIME_SYNC_NAME_CAPACITY];

    /* No configuration: public server only. */
    assert(jetkvm_time_sync_server_list(NULL, names, 4) == 1);
    assert(strcmp(names[0], JETKVM_TIME_SYNC_FALLBACK_SERVER) == 0);

    /* User servers query first; the fallback closes the list. */
    jetkvm_time_sync_config_t custom = {
        .servers = {"192.168.1.135", "", "ntp.lab.test"}, .count = 3, .fallback = true,
    };
    assert(jetkvm_time_sync_server_list(&custom, names, 4) == 3);
    assert(strcmp(names[0], "192.168.1.135") == 0);
    assert(strcmp(names[1], "ntp.lab.test") == 0);
    assert(strcmp(names[2], JETKVM_TIME_SYNC_FALLBACK_SERVER) == 0);

    /* Disabled fallback leaves only the user's servers, even none at all. */
    custom.fallback = false;
    assert(jetkvm_time_sync_server_list(&custom, names, 4) == 2);
    jetkvm_time_sync_config_t empty = {.count = 0, .fallback = false};
    assert(jetkvm_time_sync_server_list(&empty, names, 4) == 0);

    /* A user entry equal to the fallback is not queried twice. */
    jetkvm_time_sync_config_t same = {
        .servers = {JETKVM_TIME_SYNC_FALLBACK_SERVER}, .count = 1, .fallback = true,
    };
    assert(jetkvm_time_sync_server_list(&same, names, 4) == 1);

    /* The lwIP slot count bounds the list; user servers keep priority. */
    jetkvm_time_sync_config_t full = {
        .servers = {"a.test", "b.test", "c.test"}, .count = 3, .fallback = true,
    };
    assert(jetkvm_time_sync_server_list(&full, names, 2) == 2);
    assert(strcmp(names[1], "b.test") == 0);

    /* Oversized names cannot be handed to lwIP. */
    char oversized[JETKVM_TIME_SYNC_NAME_CAPACITY + 1];
    memset(oversized, 'x', sizeof(oversized) - 1);
    oversized[sizeof(oversized) - 1] = 0;
    jetkvm_time_sync_config_t big = {.servers = {oversized}, .count = 1, .fallback = false};
    assert(jetkvm_time_sync_server_list(&big, names, 4) == 0);

    puts("time sync server order tests passed");
    return 0;
}
