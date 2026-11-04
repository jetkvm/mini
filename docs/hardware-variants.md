# Hardware variants

The board variant is chosen at configure time with `-DJETKVM_BOARD_VARIANT=ethernet` (default) or `wifi`. Use a separate build directory for each.

## Pins

| Function | Ethernet board | Wi-Fi board |
| --- | --- | --- |
| Network | IP101 Ethernet | ESP32-C5-MINI over 4-bit SDIO |
| Screen MOSI / SCLK / BL | GPIO3 / GPIO13 / GPIO4 | GPIO3 / GPIO13 / GPIO4 |
| Screen DC / RST / CS | GPIO20 / GPIO21 / GPIO22 | GPIO14 / GPIO15 / GPIO16 |
| SW2 button (active low) | GPIO47 | GPIO47 |
| SW3 mode button (active low) | GPIO23 | GPIO17 |
| TF card | SDMMC slot 1 | SDMMC slot 0, GPIO39..44 |
| ESP32-C5 | — | SDMMC slot 1, GPIO18..23 |

On the Wi-Fi board the TF card and the C5 share the P4's SDMMC controller on two slots. The screen has no MISO line.

### ESP32-C5 connector (FPC1, Wi-Fi board)

| Pin | Net | P4 |
| --- | --- | --- |
| 1, 2 | `ESP_3V3` | 3.3 V |
| 3 | `C5_EN` | GPIO9 |
| 4 | `C5_DAT1` | GPIO21 |
| 5 | `C5_DAT0` | GPIO20 |
| 6 | `C5_CLK` | GPIO19 |
| 7 | `C5_CMD` | GPIO18 |
| 8 | `C5_DAT3` | GPIO23 |
| 9 | `C5_DAT2` | GPIO22 |
| 10, 11 | `TX0`, `RX0` | test pads only |
| 12 | `C5_BOOT` | GPIO7 |
| 13, 14 | `GND` | ground |
