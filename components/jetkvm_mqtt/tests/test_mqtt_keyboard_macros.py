"""Run the keyboard_macro/set handler and the macro button discovery of
jetkvm_mqtt.c against fakes of the stored macros, the HID input bridge and a
broker that keeps retained messages. The fake bridge records what the handler
queues; no HID step runs (jetkvm/kvm 9dae89aa)."""
import json
from pathlib import Path
import re
import subprocess
import tempfile
import unittest

from test_mqtt_values import block

REPO = Path(__file__).resolve().parents[3]


def function(source, name):
    return block(source, r"static [\w \*]+?\b" + name + r"\(")


MODEL = r'''
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "cJSON.h"
#include "jetkvm_control.h"
#include "jetkvm_mqtt.h"
'''

MODEL_BODY = r'''
#define portMAX_DELAY 0
#define xSemaphoreTake(lock, ticks) ((void)(lock), (void)(ticks))
#define xSemaphoreGive(lock) ((void)(lock))
typedef void *SemaphoreHandle_t;
#define ESP_LOGI(tag, ...) ((void)(tag))
#define ESP_LOGW(tag, ...) (printf("log\t" __VA_ARGS__), printf("\n"))
#define ESP_LOGE(tag, ...) (printf("log\t" __VA_ARGS__), printf("\n"))
#define JETKVM_DEVICE_ID_CAPACITY 32U
static const char *TAG = "jetkvm_mqtt";
typedef struct { const char *version; } esp_app_desc_t;
static const esp_app_desc_t app = {"1.2.3"};
static const esp_app_desc_t *esp_app_get_description(void) { return &app; }
static void network_snapshot(char ip_address[16], char hostname[64])
{
    ip_address[0] = '\0';
    snprintf(hostname, 64, "kvm");
}
typedef struct {
    const char *data;
    int data_len;
    int total_data_len;
    int current_data_offset;
    bool retain;
} esp_mqtt_event_t;
static struct {
    int lock;
    jetkvm_mqtt_config_t config;
    struct { unsigned commands_rejected; } status;
    char macro_buttons[MQTT_MACRO_BUTTON_CAPACITY]
                      [JETKVM_CONTROL_KEYBOARD_MACRO_ID_CAPACITY];
    size_t macro_button_count;
    uint32_t macro_revision;
} s_mqtt;
static bool runtime_ready(void) { return true; }
static SemaphoreHandle_t macro_discovery_lock(void) { return NULL; }

/* The stored macros (jetkvm_hid_profiles). */
static jetkvm_control_keyboard_macro_list_t stored;
static uint32_t revision = 1U;
typedef struct { uint32_t revision; } jetkvm_hid_profiles_status_t;
static esp_err_t jetkvm_hid_profiles_get_status(jetkvm_hid_profiles_status_t *status)
{
    status->revision = revision;
    return ESP_OK;
}
static esp_err_t jetkvm_hid_profiles_get_macros(jetkvm_control_keyboard_macro_list_t *macros)
{
    *macros = stored;
    return ESP_OK;
}
static void *jetkvm_memory_policy_control_alloc(size_t size) { return malloc(size); }
static void jetkvm_memory_policy_control_free(void *memory) { free(memory); }

/* The HID input bridge: what a macro run would type. */
typedef enum {
    JETKVM_HID_INPUT_SUBMIT_ACCEPTED = 0,
    JETKVM_HID_INPUT_SUBMIT_NOT_READY,
    JETKVM_HID_INPUT_SUBMIT_BACKPRESSURE,
    JETKVM_HID_INPUT_SUBMIT_BUSY,
} jetkvm_hid_input_submit_result_t;
static bool macro_running;
static jetkvm_hid_input_submit_result_t jetkvm_hid_input_submit_local_macro(
    const jetkvm_hidrpc_macro_step_t *steps, uint32_t step_count, bool replace)
{
    if (macro_running && !replace) {
        return JETKVM_HID_INPUT_SUBMIT_BUSY;
    }
    printf("type\treplace=%d", replace ? 1 : 0);
    for (uint32_t i = 0; i < step_count; ++i) {
        printf("\t%02x:%02x:%u", steps[i].modifier, steps[i].keys[0],
               (unsigned)steps[i].delay_ms);
    }
    printf("\n");
    return JETKVM_HID_INPUT_SUBMIT_ACCEPTED;
}

/* A broker that keeps the last retained payload of each topic. */
#define RETAINED_MAX 64
static struct { char topic[256]; char *payload; } retained[RETAINED_MAX];
static bool broker_reachable = true;
static bool publish_current_topic(const char *topic, const char *payload,
                                  size_t length, bool retain)
{
    if (!broker_reachable || !retain) {
        return false;
    }
    int slot = -1;
    for (int i = 0; i < RETAINED_MAX; ++i) {
        if (retained[i].payload != NULL && strcmp(retained[i].topic, topic) == 0) {
            slot = i;
        }
    }
    if (slot >= 0) {
        free(retained[slot].payload);
        retained[slot].payload = NULL;
    }
    if (length == 0U) {
        return true;
    }
    for (int i = 0; i < RETAINED_MAX; ++i) {
        if (retained[i].payload == NULL) {
            snprintf(retained[i].topic, sizeof(retained[i].topic), "%s", topic);
            retained[i].payload = strndup(payload, length);
            return true;
        }
    }
    return false;
}
'''

MAIN = r'''
static void add_macro(const char *id, const char *name, int key_id)
{
    jetkvm_control_keyboard_macro_t *macro = &stored.macros[stored.count++];
    memset(macro, 0, sizeof(*macro));
    snprintf(macro->id, sizeof(macro->id), "%s", id);
    snprintf(macro->name, sizeof(macro->name), "%s", name);
    macro->step_count = 1U;
    macro->steps[0].key_count = 1U;
    macro->steps[0].key_ids[0] = (uint8_t)key_id;
    macro->steps[0].modifier_count = 1U;
    macro->steps[0].modifier_ids[0] = 2U; /* ShiftLeft */
    macro->steps[0].delay_ms = 300U;
    revision++;
}

static void delete_macro(const char *id)
{
    for (size_t i = 0; i < stored.count; ++i) {
        if (strcmp(stored.macros[i].id, id) == 0) {
            memmove(&stored.macros[i], &stored.macros[i + 1],
                    (stored.count - i - 1U) * sizeof(stored.macros[0]));
            stored.count--;
            revision++;
            return;
        }
    }
}

/* One command per line, fields separated by tabs:
 *   macro <id> <name> <key id> | delete <id> | switch on|off
 *   | running on|off | broker up|down | discovery | stale
 *   | command <payload> [retained] | rejected | retained */
int main(void)
{
    jetkvm_mqtt_config_defaults(&s_mqtt.config);
    s_mqtt.config.enabled = true;
    snprintf(s_mqtt.config.broker, sizeof(s_mqtt.config.broker), "broker");
    char line[512];
    while (fgets(line, sizeof(line), stdin) != NULL) {
        line[strcspn(line, "\n")] = '\0';
        char *fields[5] = {0};
        int count = 0;
        for (char *field = strtok(line, "\t"); field != NULL && count < 5;
             field = strtok(NULL, "\t")) {
            fields[count++] = field;
        }
        if (count == 0) {
            continue;
        }
        if (strcmp(fields[0], "macro") == 0) {
            add_macro(fields[1], fields[2], atoi(fields[3]));
        } else if (strcmp(fields[0], "delete") == 0) {
            delete_macro(fields[1]);
        } else if (strcmp(fields[0], "switch") == 0) {
            s_mqtt.config.enable_keyboard_macros = strcmp(fields[1], "on") == 0;
        } else if (strcmp(fields[0], "running") == 0) {
            macro_running = strcmp(fields[1], "on") == 0;
        } else if (strcmp(fields[0], "broker") == 0) {
            broker_reachable = strcmp(fields[1], "up") == 0;
        } else if (strcmp(fields[0], "discovery") == 0) {
            publish_macro_discovery(&s_mqtt.config, "dev");
        } else if (strcmp(fields[0], "stale") == 0) {
            printf("stale\t%d\n", macro_discovery_stale() ? 1 : 0);
        } else if (strcmp(fields[0], "command") == 0) {
            esp_mqtt_event_t event = {
                .data = fields[1],
                .data_len = (int)strlen(fields[1]),
                .total_data_len = (int)strlen(fields[1]),
                .retain = count > 2 && strcmp(fields[2], "retained") == 0,
            };
            handle_keyboard_macro_command(&event);
        } else if (strcmp(fields[0], "rejected") == 0) {
            printf("rejected\t%u\n", s_mqtt.status.commands_rejected);
        } else if (strcmp(fields[0], "retained") == 0) {
            for (int i = 0; i < RETAINED_MAX; ++i) {
                if (retained[i].payload != NULL) {
                    printf("retained\t%s\t%s\n", retained[i].topic,
                           retained[i].payload);
                }
            }
        } else {
            return 2;
        }
    }
    return 0;
}
'''


def key_id(name):
    names = re.findall(
        r'^\s*"([A-Za-z0-9_]+)",$',
        (REPO / "components/control_protocol/keyboard_key_names.inc").read_text(),
        re.M)
    return names.index(name)


class MqttKeyboardMacroTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.temp = tempfile.TemporaryDirectory()
        cls.addClassCleanup(cls.temp.cleanup)
        source = (REPO / "components/jetkvm_mqtt/jetkvm_mqtt.c").read_text()
        defines = re.findall(
            r"^#define (?:MQTT_TOPIC_CAPACITY|MACRO_OBJECT_ID_CAPACITY|"
            r"MQTT_MACRO_BUTTON_CAPACITY) [^\n]*", source, re.MULTILINE)
        assert len(defines) == 3, defines
        parts = [MODEL, *defines, MODEL_BODY, *(function(source, name) for name in (
            "copy_text", "copy_trimmed_payload", "topic_from_snapshot",
            "ha_device_info", "add_optional_string", "new_discovery_payload",
            "publish_discovery_root", "macro_object_id",
            "remove_macro_button", "keep_macro_button", "macro_list_has",
            "keyboard_macros_allowed",
            "publish_macro_discovery", "macro_discovery_stale",
            "handle_keyboard_macro_command")), MAIN]
        test_source = Path(cls.temp.name) / "keyboard_macros.c"
        test_source.write_text("\n".join(parts))
        cls.binary = Path(cls.temp.name) / "keyboard_macros"
        cjson = REPO / "managed_components/espressif__cjson/cJSON"
        subprocess.run([
            "cc", "-std=gnu11", "-Wall", "-Wextra", "-Werror",
            "-Wno-unused-function",
            f"-I{REPO / 'components/jetkvm_mqtt/tests/compat'}",
            f"-I{REPO / 'components/jetkvm_mqtt/include'}",
            f"-I{REPO / 'components/control_protocol/include'}",
            f"-I{REPO / 'components/control_protocol'}",
            f"-I{cjson}", str(test_source),
            str(REPO / "components/jetkvm_mqtt/jetkvm_mqtt_config.c"),
            str(REPO / "components/control_protocol/keyboard_macro_hid.c"),
            str(cjson / "cJSON.c"), "-lm", "-o", str(cls.binary)],
            check=True)

    def run_script(self, *lines):
        script = "".join("\t".join(map(str, line)) + "\n" for line in lines)
        output = subprocess.run([str(self.binary)], input=script, check=True,
                                capture_output=True, text=True).stdout
        return [line.split("\t") for line in output.splitlines()]

    def typed(self, output):
        return [line[1:] for line in output if line[0] == "type"]

    def buttons(self, output):
        """The retained macro buttons: object id -> discovery payload."""
        found = {}
        for line in output:
            if line[0] != "retained":
                continue
            match = re.fullmatch(
                r"homeassistant/button/jetkvm_dev/(macro_[^/]+)/config", line[1])
            if match:
                found[match.group(1)] = json.loads(line[2])
        return found

    def test_switch_off_types_nothing_even_with_actions_allowed(self):
        output = self.run_script(
            ("macro", "macro-1", "Login", key_id("KeyA")),
            ("command", "macro-1"), ("rejected",))
        self.assertEqual(self.typed(output), [])
        self.assertIn(["log", "keyboard macro command rejected: keyboard "
                       "macros over MQTT are disabled"], output)
        self.assertIn(["rejected", "1"], output)

    def test_macro_by_id_or_name_types_press_and_release(self):
        output = self.run_script(
            ("macro", "macro-1", "Login", key_id("KeyA")),
            ("macro", "macro-2", "Enter", key_id("Enter")),
            ("switch", "on"),
            ("command", " macro-2 \r\n"), ("command", "LOGIN"),
            ("rejected",))
        # ShiftLeft + key held 20 ms, then all keys up for the step's delay.
        # MQTT does not replace a macro that runs.
        self.assertEqual(self.typed(output), [
            ["replace=0", "02:28:20", "00:00:300"],
            ["replace=0", "02:04:20", "00:00:300"]])
        self.assertIn(["rejected", "0"], output)

    def test_retained_message_types_nothing(self):
        output = self.run_script(
            ("macro", "macro-1", "Login", key_id("KeyA")), ("switch", "on"),
            ("command", "macro-1", "retained"))
        self.assertEqual(self.typed(output), [])
        self.assertIn(["log", "ignoring retained keyboard macro command"],
                      output)

    def test_unknown_macro_types_nothing(self):
        output = self.run_script(
            ("macro", "macro-1", "Login", key_id("KeyA")), ("switch", "on"),
            ("command", "macro-9"), ("rejected",))
        self.assertEqual(self.typed(output), [])
        self.assertIn(["log", "unknown keyboard macro: macro-9"], output)
        self.assertIn(["rejected", "1"], output)

    def test_message_during_a_running_macro_is_dropped(self):
        output = self.run_script(
            ("macro", "macro-1", "Login", key_id("KeyA")), ("switch", "on"),
            ("running", "on"), *([("command", "macro-1")] * 50),
            ("rejected",))
        self.assertEqual(self.typed(output), [])
        self.assertEqual(output.count(
            ["log", "keyboard macro command dropped: a keyboard macro is "
                    "already running (Login)"]), 50)
        self.assertIn(["rejected", "50"], output)

    def test_buttons_follow_the_switch(self):
        macro = ("macro", "macro-1", "Login", key_id("KeyA"))
        output = self.run_script(macro, ("discovery",), ("retained",))
        self.assertEqual(self.buttons(output), {})

        output = self.run_script(
            macro, ("switch", "on"), ("discovery",), ("retained",))
        button = self.buttons(output)["macro_macro-1"]
        # The fields of Go publishMacroDiscovery, so Home Assistant sees the
        # same entity from either device.
        self.assertEqual({k: v for k, v in button.items() if k != "device"}, {
            "name": "Macro: Login",
            "unique_id": "jetkvm_dev_macro_macro-1",
            "command_topic": "jetkvm/dev/keyboard_macro/set",
            "payload_press": "macro-1",
            "icon": "mdi:keyboard-outline",
            "availability_topic": "jetkvm/dev/status",
            "availability_template":
                "{{ 'online' if value_json.online else 'offline' }}",
            "default_entity_id": "button.jetkvm_dev_macro_macro-1"})
        self.assertEqual(button["device"]["identifiers"], ["dev"])

        output = self.run_script(
            macro, ("switch", "on"), ("discovery",), ("switch", "off"),
            ("discovery",), ("retained",))
        self.assertEqual(self.buttons(output), {})

    def test_button_of_a_deleted_macro_is_cleared(self):
        output = self.run_script(
            ("macro", "macro-1", "One", key_id("KeyA")),
            ("macro", "with space/slash", "Two", key_id("KeyB")),
            ("switch", "on"), ("discovery",),
            ("broker", "down"), ("delete", "macro-1"), ("stale",),
            ("discovery",), ("broker", "up"), ("stale",),
            ("discovery",), ("retained",), ("stale",))
        self.assertEqual(
            [line for line in output if line[0] == "stale"],
            [["stale", "1"], ["stale", "0"], ["stale", "0"]])
        self.assertEqual(set(self.buttons(output)), {"macro_with_space_slash"})


if __name__ == "__main__":
    unittest.main()
