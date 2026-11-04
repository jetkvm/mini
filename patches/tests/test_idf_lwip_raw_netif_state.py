#!/usr/bin/env python3
"""Exercise ESP-IDF's interface lookup with WireGuard and managed interfaces."""
from pathlib import Path
import os
import shutil
import subprocess
import tempfile

root = Path(__file__).resolve().parents[2]
idf = Path(os.environ.get("IDF_PATH", "/nonexistent"))
relative = "components/lwip/port/include/lwipopts.h"
patch = root / "patches/esp-idf-6.0.2-lwip-raw-netif-state.patch"
assert f"apply_idf_patch(patches/{patch.name})" in (root / "CMakeLists.txt").read_text()
assert "CONFIG_LWIP_ESP_NETIF_CLIENT_DATA=y" in (root / "sdkconfig.defaults").read_text()
if not (idf / relative).is_file():
    print("SKIP raw-netif lookup regression: needs IDF_PATH")
    raise SystemExit(0)

source = (idf / "components/esp_netif/lwip/esp_netif_lwip.c").read_text()
begin = source.index("static inline esp_netif_t* lwip_get_esp_netif(")
lookup = source[begin:source.index("\n}", begin) + 2]
with tempfile.TemporaryDirectory() as directory:
    tmp = Path(directory)
    for name in (relative, "components/lwip/Kconfig"):
        target = tmp / name
        target.parent.mkdir(parents=True, exist_ok=True)
        shutil.copyfile(idf / name, target)
    # The test image may already have the patch from idf.py reconfigure.
    applied = subprocess.run(["git", "apply", "--reverse", "--check", str(patch)], cwd=tmp,
                             capture_output=True).returncode == 0
    if applied:
        subprocess.run(["git", "apply", "--reverse", str(patch)], cwd=tmp, check=True)
    for patched in (False, True):
        if patched:
            subprocess.run(["git", "apply", str(patch)], cwd=tmp, check=True)
        options = (tmp / relative).read_text()
        begin = options.index("#ifndef CONFIG_LWIP_NUM_NETIF_CLIENT_DATA")
        end = options.index("\n", options.index("#define LWIP_NUM_NETIF_CLIENT_DATA", begin))
        options = options[begin:end]
        harness = """#include <stddef.h>
#define CONFIG_LWIP_ESP_NETIF_CLIENT_DATA 1
""" + options + """
typedef struct { unsigned marker; } esp_netif_t;
struct netif { void *state; void *client_data[LWIP_NUM_NETIF_CLIENT_DATA + 1]; };
enum { lwip_netif_client_id = 0 };
#define netif_get_client_data(n, id) ((n)->client_data[id])
""" + lookup + """
int main(void) {
    unsigned wireguard_driver[32] = {0};
    struct netif raw = {.state = wireguard_driver};
    if (lwip_get_esp_netif(&raw) != NULL) return 1;
    esp_netif_t ethernet = {.marker = 42};
    struct netif managed = {.state = &ethernet};
    managed.client_data[0] = &ethernet;
    if (lwip_get_esp_netif(&managed) != &ethernet) return 2;
    managed.state = wireguard_driver;
    if (lwip_get_esp_netif(&managed) != &ethernet) return 3;
    return LWIP_NUM_NETIF_CLIENT_DATA == 1 ? 0 : 4;
}
"""
        c = tmp / "test.c"
        c.write_text(harness)
        binary = tmp / "test"
        subprocess.run(["cc", "-std=c11", "-Wall", "-Wextra", "-Werror", str(c), "-o", str(binary)], check=True)
        result = subprocess.run([str(binary)], check=False).returncode
        assert result == (0 if patched else 1), (patched, result)
print("ESP-NETIF: original confuses WireGuard state; dedicated slot preserves raw and managed interfaces: PASS")
