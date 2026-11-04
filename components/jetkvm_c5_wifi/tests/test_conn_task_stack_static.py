#!/usr/bin/env python3
import re
from pathlib import Path


# ESP-Hosted takes its SDIO buffers from internal DMA RAM, which this board
# is short of. The connection task runs only to join a network or start
# setup and writes NVS, so its stack is in LP (RTC) RAM: internal, usable
# while a flash write disables the cache, and not DMA-capable.
component = Path(__file__).resolve().parents[1]
source = (component / "jetkvm_c5_wifi.c").read_text()
create = re.search(r'xTaskCreate\w*\(\s*c5_conn_task,\s*"c5_wifi_conn".*?!= pdPASS', source, re.S)
assert create, "c5_wifi_conn is not created"
assert "MALLOC_CAP_RTCRAM" in create.group(0), f"c5_wifi_conn stack not in LP RAM: {create.group(0)}"

print("c5_wifi_conn stack in LP RAM")
