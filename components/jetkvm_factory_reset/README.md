# Factory reset

A factory reset erases device settings and the internal `storage` partition, reboots once, and continues into onboarding. It keeps TF-card files (the card is never mounted or inspected), the firmware and boot selection, PHY data and eFuse identity.

## Request

The `factoryReset` RPC calls `jetkvm_factory_reset_request()`, which saves a durable intent in the `jk_reset` NVS namespace. The reboot follows one second after the RPC response and the `willReboot` notification have been sent; `jetkvm_factory_reset_dispatch_complete()` starts that delay. If no response channel survives, request cleanup releases it. The request and restart workers run on internal-RAM stacks, because NVS writes and restart disable the external-RAM cache.

## Cleanup on boot

1. After NVS starts, and before any settings reader or network service, `jetkvm_factory_reset_boot_begin()` loads the intent. If a reset is pending, it starts the display through the progress callback, which shows `Resetting... Keep power connected` with default display settings and accepts no navigation.
2. It erases every NVS namespace except `jk_reset`, then the `storage` partition. Blocks of the partition that already read as erased are skipped.
3. `jetkvm_factory_reset_finish()` clears the intent. On the Wi-Fi board this happens only after the ESP32-C5 driver's normal `esp_wifi_restore()` cleanup succeeds, which runs because erasing `wifi_prov` removed `c5_clean`. It always happens before BLE provisioning or a Wi-Fi connection can start. The Ethernet board finishes right after the local erase.

## Failure handling

- A cleanup error keeps the intent, shows `Reset failed / Reboot to retry`, and stops normal startup before the HTTP service starts, so the local display is the only recovery interface. The next boot retries; the firmware never reboots in a loop.
- Erasure is idempotent, so a power loss mid-reset only repeats completed steps.
- An invalid intent is discarded without erasing anything. If the intent cannot be read or discarded, startup stops with the reset error and the data is preserved. Unreadable NVS is left for serial recovery.
- A Wi-Fi failure after the intent is cleared is an ordinary network startup failure, not an incomplete reset.

`/device/status` reports `factoryResetPending` and `factoryResetError`. `jetkvm_access` refuses setup while a reset is pending.

Host tests: `bash components/jetkvm_factory_reset/tests/run_host_test.sh`. Related checks live in the `jetkvm_c5_wifi`, `jetkvm_screen/tests/local_display` and `jetkvm_access` test directories.
