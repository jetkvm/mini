# ESP32-C5 ESP-Hosted-MCU firmware

These are zlib-compressed flash images of the [ESP-Hosted-MCU](https://github.com/espressif/esp-hosted-mcu) 2.12.3 slave firmware for the Wi-Fi board's ESP32-C5 coprocessor, built with ESP-IDF 5.5.4. The version must match the P4 host component `espressif/esp_hosted` 2.12.3, and the 4-bit SDIO settings in `sdkconfig.wifi.defaults` must match the build.

At boot, `jetkvm_c5_wifi.c` connects to the C5 ROM's SDIO download mode and rewrites any image whose MD5 differs. It leaves the OTA data image alone unless another image changed, because the C5 bootloader updates it.

Regenerate the files from a slave build with `tools/package_c5_hosted_firmware.sh /path/to/esp-hosted-mcu-build`. P4 builds need no C5 toolchain.
