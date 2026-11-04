"""Exercise the actual client teardown against delayed and missing PUBACKs."""
import os
from pathlib import Path
import re
import resource
import subprocess
import tempfile
import unittest


def function(source, name):
    match = re.search(r"static (?:bool|void) " + name + r"\([^;]*?\)\s*\{", source)
    if not match:
        return ""  # The legacy teardown has no drain helper (negative control).
    depth = 1
    end = match.end()
    while depth:
        depth += (source[end] == "{") - (source[end] == "}")
        end += 1
    return source[match.start():end]


MODEL = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
typedef uint32_t TickType_t;
#define pdMS_TO_TICKS(ms) ((TickType_t)((ms) / 10U))
#define ESP_LOGW(...) ((void)++warnings)
typedef struct {
    unsigned pending, acknowledged, lost, disconnected, stopped, destroyed;
    TickType_t acknowledge_at;
    bool never_acknowledge;
} client_t;
typedef client_t *esp_mqtt_client_handle_t;
static TickType_t ticks;
static unsigned warnings;
TickType_t xTaskGetTickCount(void) { return ticks; }
void vTaskDelay(TickType_t delay) { ticks += delay; }
int esp_mqtt_client_get_outbox_size(client_t *client) {
    if (!client->never_acknowledge &&
        (int32_t)(ticks - client->acknowledge_at) >= 0) {
        client->acknowledged += client->pending;
        client->pending = 0;
    }
    return (int)(client->pending * 100U);
}
int esp_mqtt_client_disconnect(client_t *client) {
    client->disconnected++;
    client->lost += client->pending;
    client->pending = 0;
    return 0;
}
int esp_mqtt_client_stop(client_t *client) {
    client->stopped++;
    client->lost += client->pending;
    client->pending = 0;
    return 0;
}
int esp_mqtt_client_destroy(client_t *client) { client->destroyed++; return 0; }
'''

SCENARIOS = r'''
int main(int argc, char **argv) {
    assert(argc == 2);
    const bool wrap = strcmp(argv[1], "wrap") == 0;
    ticks = wrap ? UINT32_MAX - 3U : 0;
    const TickType_t started = ticks;
    client_t client = {.pending = 8, .acknowledge_at = ticks + 7U};
    if (strcmp(argv[1], "null") == 0) {
        destroy_client(NULL, true);
        assert(ticks == started && warnings == 0);
        return 0;
    }
    const bool force = strcmp(argv[1], "force") == 0;
    const bool timeout = strcmp(argv[1], "timeout") == 0;
    client.never_acknowledge = timeout;
    destroy_client(&client, !force);
    assert(client.stopped == 1 && client.destroyed == 1);
    if (force) {
        assert(ticks == started && client.disconnected == 0);
        assert(client.lost == 8 && warnings == 0);
    } else if (timeout) {
        assert((TickType_t)(ticks - started) == pdMS_TO_TICKS(5000U));
        assert(client.disconnected == 1 && client.lost == 8 && warnings == 1);
    } else {
        assert(client.acknowledged == 8 && client.lost == 0);
        assert((TickType_t)(ticks - started) == 7U);
        assert(client.disconnected == 1 && warnings == 0);
    }
    return 0;
}
'''


class PublishDrainTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.temp = tempfile.TemporaryDirectory()
        cls.addClassCleanup(cls.temp.cleanup)
        source = Path(os.environ.get(
            "MQTT_SOURCE", Path(__file__).parents[1] / "jetkvm_mqtt.c")).read_text()
        timeout = re.search(r"^#define MQTT_PUBLISH_DRAIN_TIMEOUT_MS .+$",
                            source, re.MULTILINE)
        definition = timeout.group() if timeout else "#define MQTT_PUBLISH_DRAIN_TIMEOUT_MS 5000U"
        test_source = Path(cls.temp.name) / "drain.c"
        test_source.write_text(MODEL + definition + "\n" + function(source, "wait_for_publish_acks") +
                               function(source, "destroy_client") + SCENARIOS)
        cls.binary = Path(cls.temp.name) / "drain"
        subprocess.run(["cc", "-std=c11", "-Wall", "-Wextra", "-Werror",
                        str(test_source), "-o", str(cls.binary)], check=True)

    def scenario(self, name):
        def no_core():
            resource.setrlimit(resource.RLIMIT_CORE, (0, 0))
        result = subprocess.run([str(self.binary), name], capture_output=True,
                                text=True, preexec_fn=no_core)
        self.assertEqual(result.returncode, 0, result.stderr)

    def test_retained_deletions_are_acknowledged_before_disconnect(self):
        self.scenario("delayed")

    def test_unreachable_broker_has_bounded_shutdown(self):
        self.scenario("timeout")

    def test_tick_wrap_does_not_skip_drain(self):
        self.scenario("wrap")

    def test_failed_connection_can_be_stopped_immediately(self):
        self.scenario("force")

    def test_missing_client_is_safe(self):
        self.scenario("null")


if __name__ == "__main__":
    unittest.main()
