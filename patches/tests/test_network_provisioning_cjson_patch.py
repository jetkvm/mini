#!/usr/bin/env python3
"""Exercise provisioning's version cleanup with custom cJSON allocation hooks."""

from pathlib import Path
import subprocess
import tempfile


ROOT = Path(__file__).resolve().parents[2]
MANAGER = Path("managed_components/espressif__network_provisioning/src/manager.c")
PATCH = ROOT / "patches/network_provisioning-1.2.4-cjson-free.patch"
CJSON = ROOT / "managed_components/espressif__cjson/cJSON"

HARNESS = r'''
#include <assert.h>
#include <stdlib.h>
#include <string.h>
#include "cJSON.h"

static unsigned live, wrong_free;
static void *hook_alloc(size_t size) {
    void *p = malloc(size);
    if (p) ++live;
    return p;
}
static void hook_free(void *p) {
    if (p) { assert(live); --live; free(p); }
}
static void system_free(void *p) {
    if (p) { ++wrong_free; free(p); }
}
static cJSON *network_prov_get_info_json(void) {
    cJSON *json = cJSON_CreateObject();
    assert(cJSON_AddStringToObject(json, "ver", "v1"));
    return json;
}
static int protocomm_set_version(void *pc, const char *ep, const char *version) {
    (void)pc;
    assert(strcmp(ep, "proto-ver") == 0);
    assert(strstr(version, "v1"));
    return 0;
}
int main(void) {
    cJSON_Hooks hooks = {hook_alloc, hook_free};
    cJSON_InitHooks(&hooks);
    struct { void *pc; } context = {0}, *prov_ctx = &context;
    int ret;
    #define free system_free
    VERSION_CLEANUP
    #undef free
    return ret || wrong_free || live;
}
'''

with tempfile.TemporaryDirectory() as directory:
    work = Path(directory)
    target = work / MANAGER
    target.parent.mkdir(parents=True)
    source = (ROOT / MANAGER).read_text()
    # Accept either a fresh dependency or an already patched build tree.
    target.write_text(source)
    reverse = subprocess.run(
        ["git", "apply", "--reverse", "--check", str(PATCH)], cwd=work,
        capture_output=True,
    )
    if reverse.returncode == 0:
        subprocess.run(["git", "apply", "--reverse", str(PATCH)], cwd=work, check=True)
    for patched in (False, True):
        if patched:
            subprocess.run(["git", "apply", str(PATCH)], cwd=work, check=True)
        source = target.read_text()
        start = source.index("    cJSON *version_json = network_prov_get_info_json();")
        end = source.index("    if (ret != ESP_OK)", start)
        harness = work / "cleanup.c"
        harness.write_text(HARNESS.replace("VERSION_CLEANUP", source[start:end]))
        binary = work / "cleanup"
        subprocess.run([
            "cc", "-std=c11", "-I", str(CJSON), str(harness),
            str(CJSON / "cJSON.c"), "-lm", "-o", str(binary),
        ], check=True)
        result = subprocess.run([str(binary)])
        assert result.returncode == (0 if patched else 1), result.returncode
    # The configure helper must also recognize the applied patch on rebuild.
    subprocess.run(["git", "apply", "--reverse", "--check", str(PATCH)],
                   cwd=work, check=True)
print("Provisioning cJSON allocator regression passed (original fails, patch passes)")
