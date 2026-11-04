"""Run the virtual_media/set handler of jetkvm_mqtt.c against a model of the
virtual media state, and check what each select option does."""
from pathlib import Path
import subprocess
import tempfile
import unittest

from test_mqtt_values import function

MODEL = r'''
#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
typedef int esp_err_t;
#define ESP_OK 0
#define ESP_FAIL -1
#define ESP_ERR_INVALID_ARG 0x102
#define portMAX_DELAY 0
#define xSemaphoreTake(lock, ticks) ((void)(lock))
#define xSemaphoreGive(lock) ((void)(lock))
#define JETKVM_VIRTUAL_MEDIA_FILENAME_CAPACITY 256U
#define JETKVM_VIRTUAL_MEDIA_URL_CAPACITY 2048U
typedef enum {
    JETKVM_VIRTUAL_MEDIA_SOURCE_STORAGE = 0,
    JETKVM_VIRTUAL_MEDIA_SOURCE_HTTP,
    JETKVM_VIRTUAL_MEDIA_SOURCE_BUILTIN,
} jetkvm_virtual_media_source_t;
typedef struct {
    bool image_mounted;
    jetkvm_virtual_media_source_t source;
    char filename[JETKVM_VIRTUAL_MEDIA_FILENAME_CAPACITY];
    char url[JETKVM_VIRTUAL_MEDIA_URL_CAPACITY];
} jetkvm_virtual_media_state_t;
typedef struct { char filename[JETKVM_VIRTUAL_MEDIA_FILENAME_CAPACITY]; } jetkvm_virtual_media_file_t;
typedef struct {
    size_t count;
    jetkvm_virtual_media_file_t files[4];
} jetkvm_virtual_media_file_list_t;
typedef struct { const char *data; int data_len; } esp_mqtt_event_t;
static struct {
    int lock;
    struct { unsigned commands_rejected; } status;
} s_mqtt;
static jetkvm_virtual_media_state_t media;
static jetkvm_virtual_media_file_list_t card;
esp_err_t jetkvm_virtual_media_get_state(jetkvm_virtual_media_state_t *state)
{
    *state = media;
    return ESP_OK;
}
esp_err_t jetkvm_virtual_media_list_files(jetkvm_virtual_media_file_list_t *files)
{
    *files = card;
    return ESP_OK;
}
void *jetkvm_memory_policy_control_alloc(size_t size) { return malloc(size); }
void jetkvm_memory_policy_control_free(void *memory) { free(memory); }
esp_err_t jetkvm_virtual_media_unmount(void)
{
    printf("unmount\n");
    media.image_mounted = false;
    return ESP_OK;
}
bool webrtc_transport_mount_storage_image(const char *filename, bool read_only)
{
    (void)read_only;
    printf("mount storage %s\n", filename);
    return false;
}
static bool actions_allowed(void) { return true; }
static bool copy_trimmed_payload(const esp_mqtt_event_t *event, char *output,
                                 size_t capacity)
{
    if ((size_t)event->data_len >= capacity) {
        return false;
    }
    memcpy(output, event->data, (size_t)event->data_len);
    output[event->data_len] = '\0';
    return true;
}
static void publish_virtual_media_state(void) { printf("publish state\n"); }
static void publish_ha_discovery(void) { printf("publish discovery\n"); }
static void jetkvm_mqtt_secure_zero(void *memory, size_t size)
{
    memset(memory, 0, size);
}
'''

MAIN = r'''
/* <source none|storage|http|builtin> <filename> <url> <option> [card file]... */
int main(int argc, char **argv)
{
    if (argc < 5 || argc > 9) {
        return 2;
    }
    for (int i = 5; i < argc; ++i) {
        snprintf(card.files[card.count++].filename,
                 sizeof(card.files[0].filename), "%s", argv[i]);
    }
    media.image_mounted = strcmp(argv[1], "none") != 0;
    media.source = strcmp(argv[1], "http") == 0 ?
        JETKVM_VIRTUAL_MEDIA_SOURCE_HTTP :
        strcmp(argv[1], "builtin") == 0 ?
            JETKVM_VIRTUAL_MEDIA_SOURCE_BUILTIN :
            JETKVM_VIRTUAL_MEDIA_SOURCE_STORAGE;
    snprintf(media.filename, sizeof(media.filename), "%s", argv[2]);
    snprintf(media.url, sizeof(media.url), "%s", argv[3]);
    esp_mqtt_event_t event = {argv[4], (int)strlen(argv[4])};
    handle_virtual_media_command(&event);
    printf("mounted %d rejected %u\n", media.image_mounted,
           s_mqtt.status.commands_rejected);
    return 0;
}
'''

BUILTIN = ("builtin", "Install OS Services.iso", "")


class MqttVirtualMediaTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.temp = tempfile.TemporaryDirectory()
        cls.addClassCleanup(cls.temp.cleanup)
        source = (Path(__file__).parents[1] / "jetkvm_mqtt.c").read_text()
        parts = [MODEL, *(function(source, name) for name in (
            "copy_text", "url_basename", "virtual_media_label",
            "mounted_image_option", "file_list_has",
            "valid_virtual_media_filename", "option_is_mounted_image",
            "handle_virtual_media_command")), MAIN]
        test_source = Path(cls.temp.name) / "virtual_media.c"
        test_source.write_text("\n".join(parts))
        cls.binary = Path(cls.temp.name) / "virtual_media"
        subprocess.run(["cc", "-std=c11", "-Wall", "-Wextra", "-Werror",
                        str(test_source), "-o", str(cls.binary)], check=True)

    def select(self, mounted, option, *card):
        return subprocess.run([str(self.binary), *mounted, option, *card],
                              check=True, capture_output=True,
                              text=True).stdout.splitlines()

    def test_selecting_the_mounted_builtin_disc_keeps_it_mounted(self):
        self.assertEqual(self.select(BUILTIN, "Install OS Services.iso"), [
            "publish state", "publish discovery", "mounted 1 rejected 0"])

    def test_selecting_the_mounted_url_image_keeps_it_mounted(self):
        mounted = ("http", "", "https://example.com/images/debian.iso?x=1")
        self.assertEqual(self.select(mounted, "debian.iso"), [
            "publish state", "publish discovery", "mounted 1 rejected 0"])

    def test_a_card_file_replaces_the_builtin_disc(self):
        self.assertEqual(self.select(BUILTIN, "other.iso"), [
            "unmount", "mount storage other.iso", "mounted 0 rejected 1"])

    def test_a_card_file_with_the_builtin_disc_name_replaces_the_disc(self):
        self.assertEqual(
            self.select(BUILTIN, "Install OS Services.iso", "Install OS Services.iso"),
            ["unmount", "mount storage Install OS Services.iso",
             "mounted 0 rejected 1"])

    def test_a_card_file_with_the_url_image_name_replaces_the_image(self):
        mounted = ("http", "", "https://example.com/debian.iso")
        self.assertEqual(self.select(mounted, "debian.iso", "debian.iso"), [
            "unmount", "mount storage debian.iso", "mounted 0 rejected 1"])

    def test_the_builtin_name_without_the_disc_is_a_card_file(self):
        self.assertEqual(self.select(("none", "", ""), "Install OS Services.iso"), [
            "unmount", "mount storage Install OS Services.iso",
            "mounted 0 rejected 1"])

    def test_no_media_unmounts_the_builtin_disc(self):
        self.assertEqual(self.select(BUILTIN, "-- no media --"), [
            "unmount", "publish state", "publish discovery",
            "mounted 0 rejected 0"])


if __name__ == "__main__":
    unittest.main()
