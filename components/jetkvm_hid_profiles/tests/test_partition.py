#!/usr/bin/env python3
from pathlib import Path


REPO = Path(__file__).resolve().parents[3]
rows = {}
for raw in (REPO / "partitions.csv").read_text(encoding="utf-8").splitlines():
    line = raw.strip()
    if not line or line.startswith("#"):
        continue
    fields = [field.strip() for field in line.split(",")]
    rows[fields[0]] = fields

# Single-artifact layout: the web pack is embedded in the app image, so there
# is no web partition.  There is no factory app: exactly two 6 MiB OTA slots
# provide A/B rollback.  All persistent application settings, including HID
# profiles, share one generously sized NVS partition.  The remaining flash is
# one LittleFS volume for future logs and files without bloating NVS's RAM
# index.
expected = {
    "phy_init": (0xF000, 0x1000),
    "ota_0": (0x10000, 0x600000),
    "ota_1": (0x610000, 0x600000),
    "otadata": (0xC10000, 0x2000),
    "nvs": (0xC20000, 0x40000),
    "storage": (0xC60000, 0x3A0000),
}
assert "web_app" not in rows, "the web pack must live inside the app image"
assert "factory" not in rows, "A/B layout must not contain a factory app"
assert "hid_profiles" not in rows, "HID profiles must share the system NVS"
nvs_rows = [row for row in rows.values() if row[1:3] == ["data", "nvs"]]
assert len(nvs_rows) == 1, f"expected one NVS partition, got {len(nvs_rows)}"
assert rows["storage"][1:3] == ["data", "littlefs"], (
    f"storage must be a LittleFS data partition, got {rows['storage'][1:3]}"
)
app_rows = {
    name: row for name, row in rows.items() if row[1] == "app"
}
assert set(app_rows) == {"ota_0", "ota_1"}, (
    f"expected exactly ota_0 and ota_1 app partitions, got {sorted(app_rows)}"
)
for name, (offset, size) in expected.items():
    row = rows[name]
    assert int(row[3], 0) == offset, f"{name} offset: {row[3]} != {hex(offset)}"
    assert int(row[4], 0) == size, f"{name} size: {row[4]} != {hex(size)}"

# App partitions must be 64 KiB aligned and every region must fit the 16 MiB
# flash without overlap.
for name in app_rows:
    assert int(rows[name][3], 0) % 0x10000 == 0, f"{name} is not 64 KiB aligned"
spans = sorted(
    (int(row[3], 0), int(row[3], 0) + int(row[4], 0), name)
    for name, row in rows.items()
)
for (prev_start, prev_end, prev_name), (start, end, name) in zip(spans, spans[1:]):
    assert start >= prev_end, f"{name} overlaps {prev_name}"
assert spans[-1][1] <= 0x1000000, f"{spans[-1][2]} exceeds the 16 MiB flash"
print("Single-artifact OTA partition layout: OK")
