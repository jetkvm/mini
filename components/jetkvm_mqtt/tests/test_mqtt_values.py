"""Run the payload and discovery code of jetkvm_mqtt.c and render the value
templates of its entities against the payloads, as Home Assistant does."""
import json
from pathlib import Path
import re
import subprocess
import tempfile
import unittest

import jinja2


def block(source, start_pattern):
    """The source text from start_pattern to the brace that closes it. Braces
    in string literals (the JSON payloads) do not count."""
    match = re.search(start_pattern, source)
    assert match, start_pattern
    end = source.index("{", match.start()) + 1
    depth = 1
    in_string = False
    while depth:
        character = source[end]
        if character == "\\":
            end += 1
        elif character == '"':
            in_string = not in_string
        elif not in_string:
            depth += (character == "{") - (character == "}")
        end += 1
    return source[match.start():end]


def function(source, name):
    return block(source, r"static [\w ]+\b" + name + r"\(")


MODEL = r'''
#include <inttypes.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#define ESP_OK 0
#define portMAX_DELAY 0
#define xSemaphoreTake(lock, ticks) ((void)(lock))
#define xSemaphoreGive(lock) ((void)(lock))
#define MQTT_TOPIC_CAPACITY 256U
typedef uint32_t TickType_t;
#define pdMS_TO_TICKS(ms) ((TickType_t)((ms) / 10U))
static TickType_t ticks;
TickType_t xTaskGetTickCount(void) { return ticks; }
typedef struct {
    bool mode_valid, video_switching;
    uint16_t width, height, fps;
} webrtc_transport_stats_t;
typedef struct { uint64_t bytes_used, bytes_free; } jetkvm_virtual_media_space_t;
static struct {
    int lock;
    char video_payload[MQTT_VIDEO_PAYLOAD_CAPACITY];
    TickType_t video_lost_at;
    bool video_lost;
} s_mqtt;
static webrtc_transport_stats_t transport;
static bool broker_reachable = true;
int webrtc_transport_get_stats(webrtc_transport_stats_t *stats)
{
    *stats = transport;
    return ESP_OK;
}
static bool publish_suffix(const char *suffix, const char *payload, size_t length)
{
    if (broker_reachable) {
        printf("publish\t%s\t%.*s\n", suffix, (int)length, payload);
    }
    return broker_reachable;
}
static bool publish_current_topic(const char *topic, const char *payload,
                                  size_t length, bool retained)
{
    printf("publish\t%s\t%.*s\t%s\n", topic, (int)length, payload,
           retained ? "retained" : "not retained");
    return true;
}
typedef int *esp_mqtt_client_handle_t;
static void direct_publish(esp_mqtt_client_handle_t client, const char *topic,
                           const char *payload)
{
    (void)client;
    printf("publish\t%s\t%s\tretained\n", topic, payload);
}
'''

MAIN = r'''
int main(int argc, char **argv)
{
    if (argc == 2 && strcmp(argv[1], "entities") == 0) {
        for (size_t i = 0U;
             i < sizeof(s_general_discovery) / sizeof(s_general_discovery[0]);
             ++i) {
            printf("%s\t%s\t%s\t%s\n", s_general_discovery[i].component,
                   s_general_discovery[i].object_id,
                   s_general_discovery[i].state_suffix,
                   s_general_discovery[i].value_template);
        }
        return 0;
    }
    if (argc == 2 && strcmp(argv[1], "removed") == 0) {
        remove_unsupported_discovery("0123abcd");
        return 0;
    }
    if (argc == 2 && strcmp(argv[1], "discovery off") == 0) {
        direct_remove_all_discovery(&s_mqtt.lock, "0123abcd");
        return 0;
    }
    if (argc == 4 && strcmp(argv[1], "system") == 0) {
        jetkvm_virtual_media_space_t space = {0};
        const bool card = sscanf(argv[2], "%" SCNu64, &space.bytes_used) == 1 &&
                          sscanf(argv[3], "%" SCNu64, &space.bytes_free) == 1;
        char payload[256];
        size_t length = system_state_payload(
            40U * 1048576U, 64U * 1048576U, card ? &space : NULL, payload,
            sizeof(payload));
        printf("%.*s\n", (int)length, payload);
        return 0;
    }
    /* video <step>...: each step is
     * <ms>,<valid 0|1>,<switching 0|1>,<width>,<height>,<fps>,<call>: the
     * time since the first step, the transport's state, and the call, which
     * is "all" (connect and every 15 s), "change" (every second),
     * "unreachable" (a change publish that the client refuses) or
     * "connect" (the client connected to a broker). The tick count starts
     * shortly before its wrap. */
    for (int i = 2; i < argc; ++i) {
        unsigned ms, valid, switching, width, height, fps;
        char call[16];
        if (sscanf(argv[i], "%u,%u,%u,%u,%u,%u,%15s", &ms, &valid, &switching,
                   &width, &height, &fps, call) != 7) {
            return 2;
        }
        ticks = UINT32_MAX - 150U + pdMS_TO_TICKS(ms);
        transport = (webrtc_transport_stats_t){
            .mode_valid = valid != 0U, .video_switching = switching != 0U,
            .width = (uint16_t)width, .height = (uint16_t)height,
            .fps = (uint16_t)fps,
        };
        printf("step\n");
        if (strcmp(call, "connect") == 0) {
            s_mqtt.video_payload[0] = '\0';
            publish_video_state(false);
            continue;
        }
        broker_reachable = strcmp(call, "unreachable") != 0;
        publish_video_state(strcmp(call, "all") != 0);
    }
    return 0;
}
'''

READY_1080 = '{"ready":true,"width":1920,"height":1080,"fps":60.0}'
READY_720 = '{"ready":true,"width":1280,"height":720,"fps":30.0}'
NO_SIGNAL = '{"ready":false,"error":"no_signal","width":0,"height":0,"fps":0}'


class MqttValueTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.temp = tempfile.TemporaryDirectory()
        cls.addClassCleanup(cls.temp.cleanup)
        source = (Path(__file__).parents[1] / "jetkvm_mqtt.c").read_text()
        defines = re.findall(
            r"^#define MQTT_(?:VIDEO_PAYLOAD_CAPACITY|NO_SIGNAL_DELAY_MS) .+$",
            source, re.MULTILINE)
        assert len(defines) == 2
        cls.no_signal_delay_ms = int(
            re.search(r"NO_SIGNAL_DELAY_MS (\d+)U", defines[1]).group(1))
        parts = [
            *defines, MODEL,
            block(source, r"typedef struct \{\s*const char \*component;") +
            " ha_entity_t;",
            block(source, r"static const ha_entity_t s_general_discovery\[\]") + ";",
            function(source, "copy_text"),
            function(source, "video_state_payload"),
            function(source, "publish_video_state"),
            function(source, "system_state_payload"),
            function(source, "remove_discovery"),
            re.search(r"static const struct \{[^}]*\} s_unsupported_discovery"
                      r"\[\] = \{.*?\n\};", source, re.DOTALL).group(),
            function(source, "remove_unsupported_discovery"),
            function(source, "direct_clear_discovery"),
            function(source, "direct_remove_all_discovery"),
            MAIN,
        ]
        test_source = Path(cls.temp.name) / "values.c"
        test_source.write_text("\n".join(parts))
        cls.binary = Path(cls.temp.name) / "values"
        subprocess.run(["cc", "-std=c11", "-Wall", "-Wextra", "-Werror",
                        str(test_source), "-o", str(cls.binary)], check=True)
        cls.entities = {}
        for line in cls.run_binary("entities").splitlines():
            component, object_id, topic, template = line.split("\t")
            cls.entities[object_id] = (component, topic, template)

    @classmethod
    def run_binary(cls, *arguments):
        return subprocess.run([str(cls.binary), *arguments], check=True,
                              capture_output=True, text=True).stdout

    def video_publishes(self, *steps):
        """The video/state payload of each step, or None for no publish. A
        step is (seconds, video, call); the video is "1080", "720", "none"
        (no mode) or "switching" (no mode, during a change of the source)."""
        video = {"1080": "1,0,1920,1080,60", "720": "1,0,1280,720,30",
                 "none": "0,0,0,0,0", "switching": "0,1,0,0,0"}
        output = self.run_binary("video", *(
            f"{round(seconds * 1000)},{video[state]},{call}"
            for seconds, state, call in steps))
        published = []
        for step in output.split("step\n")[1:]:
            lines = step.splitlines()
            self.assertLessEqual(len(lines), 1)
            published.append(lines[0].split("\t")[2] if lines else None)
            if lines:
                self.assertEqual(lines[0].split("\t")[1], "video/state")
        return published

    def render(self, object_id, payload):
        """The entity's state text: the value template with the payload as
        value_json. An undefined field is an error, as it is one in the log of
        Home Assistant."""
        template = jinja2.Environment(undefined=jinja2.StrictUndefined) \
            .from_string(self.entities[object_id][2])
        return template.render(value_json=json.loads(payload))

    def system_payload(self, used="", free=""):
        return self.run_binary("system", used, free).strip()

    def test_video_mode_shows_its_resolution_and_frame_rate(self):
        self.assertEqual(self.video_publishes((0, "1080", "all")),
                         [READY_1080])
        self.assertEqual(self.render("video_signal", READY_1080), "ON")
        self.assertEqual(self.render("video_resolution", READY_1080),
                         "1920x1080")
        self.assertEqual(self.render("video_fps", READY_1080), "60.0")

    def test_no_signal_shows_text_and_an_unknown_frame_rate(self):
        delay = self.no_signal_delay_ms / 1000
        self.assertEqual(
            self.video_publishes((0, "none", "all"), (delay, "none", "all")),
            [None, NO_SIGNAL])
        self.assertEqual(self.render("video_signal", NO_SIGNAL), "OFF")
        self.assertEqual(self.render("video_resolution", NO_SIGNAL),
                         "No signal")
        # Home Assistant sets a numeric MQTT sensor to unknown for "None".
        self.assertEqual(self.render("video_fps", NO_SIGNAL), "None")

    def test_lost_signal_is_published_after_the_delay(self):
        delay = self.no_signal_delay_ms / 1000
        self.assertEqual(
            self.video_publishes(
                (0, "1080", "all"),
                (1, "none", "change"),
                (2, "none", "all"),
                (delay + 0.9, "none", "change"),
                (delay + 1, "none", "change"),
                (delay + 2, "none", "change"),
                (delay + 3, "none", "all"),
                (delay + 4, "1080", "change")),
            [READY_1080, None, None, None, NO_SIGNAL, None, NO_SIGNAL,
             READY_1080])

    def test_source_change_publishes_no_lost_signal(self):
        # A change to service video and back: the capture has its mode
        # about a second after the change back ended.
        self.assertEqual(
            self.video_publishes(
                (0, "1080", "all"),
                (1, "switching", "change"),
                (2, "switching", "all"),     # The 15 s publish in the change.
                (5, "720", "change"),
                (6, "720", "change"),
                (10, "switching", "change"),
                (12.4, "none", "change"),    # Capture, not yet with a mode.
                (13, "none", "all"),
                (13.8, "1080", "change")),
            [READY_1080, None, None, READY_720, None, None, None, None,
             READY_1080])

    def test_change_that_ends_without_a_mode_publishes_no_signal(self):
        delay = self.no_signal_delay_ms / 1000
        self.assertEqual(
            self.video_publishes(
                (0, "1080", "all"),
                (1, "switching", "change"),
                (10, "switching", "change"),
                (11, "none", "change"),
                (11 + delay - 0.1, "none", "change"),
                (11 + delay, "none", "change")),
            [READY_1080, None, None, None, None, NO_SIGNAL])

    def test_new_mode_is_published_between_the_periodic_publishes(self):
        self.assertEqual(
            self.video_publishes(
                (0, "1080", "all"),
                (1, "1080", "change"),
                (2, "720", "change"),
                (3, "720", "change")),
            [READY_1080, None, READY_720, None])

    def test_refused_publish_is_sent_again(self):
        self.assertEqual(
            self.video_publishes(
                (0, "1080", "all"),
                (1, "720", "unreachable"),
                (2, "720", "change")),
            [READY_1080, None, READY_720])

    def test_new_connection_gets_the_state_after_a_change(self):
        self.assertEqual(
            self.video_publishes(
                (0, "1080", "all"),
                (1, "switching", "connect"),
                (2, "1080", "change")),
            [READY_1080, None, READY_1080])

    def test_storage_with_a_card(self):
        payload = self.system_payload(str(3 * 1048576), str(5 * 1048576 // 2))
        self.assertEqual(json.loads(payload), {
            "memory_used": 40 * 1048576, "memory_total": 64 * 1048576,
            "storage_used": 3 * 1048576, "storage_free": 5 * 1048576 // 2})
        self.assertEqual(self.render("storage_used", payload), "3.0")
        self.assertEqual(self.render("storage_free", payload), "2.5")
        self.assertEqual(self.render("memory_used", payload), "40.0")

    def test_empty_card_is_not_a_missing_card(self):
        payload = self.system_payload("0", str(1048576))
        self.assertEqual(self.render("storage_used", payload), "0.0")
        self.assertEqual(self.render("storage_free", payload), "1.0")

    def test_storage_without_a_card_is_unknown(self):
        payload = self.system_payload()
        self.assertEqual(json.loads(payload), {
            "memory_used": 40 * 1048576, "memory_total": 64 * 1048576})
        self.assertEqual(self.render("storage_used", payload), "None")
        self.assertEqual(self.render("storage_free", payload), "None")
        self.assertEqual(self.render("memory_used", payload), "40.0")

    def test_every_system_and_video_template_reads_only_published_fields(self):
        payloads = {
            "video/state": [READY_1080, NO_SIGNAL],
            "system/state": [self.system_payload("1", "2"),
                             self.system_payload()],
        }
        rendered = 0
        for object_id, (_, topic, _) in self.entities.items():
            for payload in payloads.get(topic, []):
                self.render(object_id, payload)  # Raises on an absent field.
                rendered += 1
        self.assertEqual(rendered, 2 * 6)

    def empty_configs(self, command):
        """The (component, object id) of each empty retained config."""
        removed = set()
        for line in self.run_binary(command).splitlines():
            _, topic, payload, retained = line.split("\t")
            self.assertEqual((payload, retained), ("", "retained"))
            prefix, component, node, object_id, suffix = topic.split("/")
            self.assertEqual((prefix, node, suffix),
                             ("homeassistant", "jetkvm_0123abcd", "config"))
            removed.add((component, object_id))
        return removed

    def test_discovery_off_removes_every_entity(self):
        removed = self.empty_configs("discovery off")
        announced = {(component, object_id) for object_id, (component, _, _)
                     in self.entities.items()}
        self.assertLessEqual(announced | self.empty_configs("removed"), removed)

    def test_unmeasured_values_are_removed_from_home_assistant(self):
        removed = self.empty_configs("removed")
        self.assertLessEqual({("sensor", "cpu_load"), ("sensor", "temperature")},
                             removed)
        announced = {(component, object_id) for object_id, (component, _, _)
                     in self.entities.items()}
        self.assertEqual(announced & removed, set())
        for field in ("cpu_load", "temperature"):
            self.assertNotIn(field, json.loads(self.system_payload("1", "2")))


if __name__ == "__main__":
    unittest.main()
