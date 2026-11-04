# TC358743 bridge hardware contract

This component drives the TC358743 HDMI-to-CSI-2 bridge (reset, I²C, PLL, CSI, EDID, HPD) for `jetkvm_video`. Wiring below is from the Ethernet board netlist.

| TC358743 pin | ESP32-P4 pin | Pull-up |
| --- | --- | --- |
| G4 `I2C_SDA` | GPIO2 | 10 kΩ to `ESP_3V3` |
| H4 `I2C_SCL` | GPIO5 | 10 kΩ to `ESP_3V3` |
| G5 `RESETN` | GPIO32 | 10 kΩ to `ESP_3V3` |
| B3 `INT` | GPIO33 | none |

The I²C address is `0x0f` at 100 kHz. If it does not acknowledge, the driver scans the bus and logs what it finds.

The 27 MHz oscillator X4 drives `REFCLK` and is powered from the same net as P4 `VDD_O3` and `VDD_MIPI_DPHY`. Firmware enables P4 LDO channel 3 at 2.5 V with `RESETN` low and releases reset only after the oscillator settles. 2.5 V suits both the oscillator (1.8–3.3 V) and the D-PHY. The bridge's own rails are separate.

Each HDMI pair runs from the connector through its own common-mode filter (L8 to L11) to the bridge, with no mux.

`tc_get_detected_timings()` reads all timing registers in one I²C transaction and rejects sizes outside 640x480 to 1920x1080 or with totals below the active size. This keeps an unstable input from sizing capture buffers.
