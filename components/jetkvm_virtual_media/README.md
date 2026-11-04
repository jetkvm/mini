# JetKVM virtual media storage

This component stores disk images on the TF (microSD) card and presents one image at a time to the target host as a 512-byte block device. It backs the mass-storage LUN that `jetkvm_usb_hid` exposes, through `jetkvm_virtual_media_read`, `_write` and `_sync`. It can also serve images from HTTP(S) servers and a built-in installer disc.

## Interface

The control protocol exposes these RPCs:

- `getStorageState`, plus list, space and delete on `/sdcard/jetkvm/images`, which matches JetKVM's `images` store. Paths are basenames only.
- `formatStorage` creates an exFAT filesystem with 64 KiB clusters and an empty images folder. It is refused without a card, while an image is mounted, while an upload or download writes to the card, and during a firmware update.
- `checkMountUrl` sends `Range: bytes=0-0` and requires a `206` response with a parseable total size. `mountWithHTTP` then serves the object read-only.
- `mountBuiltinInstaller` mounts the built-in installer disc.
- `downloadToStorage`, `getStorageDownloadState` and the `storageDownloadState` event download an object into the images folder.

Mount state uses JetKVM's `HTTP`, `Storage`, `CDROM` and `Disk` values, plus `Builtin`. Only a local `Disk` image is writable. Browser uploads arrive through `POST /storage/upload` in `jetkvm_virtual_media_http`.

## How it works

The component mounts the existing FAT or exFAT filesystem at `/sdcard` and never formats it implicitly. Image I/O calls FatFs directly with a 64-bit `FSIZE_t`, bypassing the ESP virtual filesystem's `off_t`, so images larger than 4 GiB work. The project patches the ESP-IDF HTTP server body length to 64 bits for the same reason.

Each mount and unmount advances a medium generation. Every read and write names its generation, so a transfer for one image never reaches another.

Uploads and downloads write `<filename>.incomplete` in order and rename it after the last byte. A new file first tries `f_expand` to reserve a contiguous extent. The committed offset advances only by bytes FatFs reports written; a retry resumes there (`alreadyUploadedBytes`). The resume key is filename plus size, as in the upstream UI, stored in a CRC-protected sidecar; an optional `sha256` adds digest matching. Only the final `f_close` syncs. Downloads (`storage_download.c`) resume with `Range: bytes=<n>-` and hold `WEBRTC_VIDEO_PAUSE_STORAGE_DOWNLOAD` while the body transfers.

Reads and writes from the host:

- Local images use read-ahead: a worker on CPU0 reads the next span into the upload DMA scratch buffer while TinyUSB sends the current one. Random or unaligned reads are synchronous.
- HTTP images have no block-device layer in between. Each `READ(10)` issues a synchronous Range request with a five-second timeout, and the `206` response's `Content-Range` must match exactly. HTTPS uses the ESP-IDF certificate bundle.
- `Disk` writes must be 512-byte aligned and inside the original size. The USB layer advertises a write-back cache and maps `SYNCHRONIZE CACHE`, stop and eject to `f_sync` or `f_close`, so `f_write` alone is not durable.

The built-in installer disc is an ISO generated at build time by `builtin_installer_iso.py`, labelled `JETKVM`. It holds `Install OS Services.html`, which redirects to `https://jetkvm.com/os-services/install?mini=<version>`. The firmware stores only its non-zero byte runs, and `builtin_image.c` fills zeros on read.

## Card insertion and removal

GPIO45 is the card-detect switch, low with a card present. The `tf_detect` task polls it every 250 ms and acts when two readings agree (`tf_detect_contract.c`).

- An inserted card gets the same power reset and mount as at boot. A card without a mountable filesystem is reported as such, and the UI offers to format it.
- A removed card, under the storage mutex, closes the image mounted from it, ends any upload or download (the next write fails with `ESP_ERR_NOT_FOUND`), and unmounts the filesystem. Open files are dropped without writing, because the slot may already hold another card. HTTP and built-in mounts stay.
- `getStorageState` reports `cardGeneration`, which advances on each removal, so the UI can detect a swap between polls. Bench builds (`JETKVM_MOCK_CLOUD_JWKS_URL`) add `testTfCardOut` to simulate removal.

## Constraints

- The storage mutex serializes all card access and leases the read-ahead scratch buffer. Starting an upload invalidates and pauses read-ahead.
- Never expose the whole card to the host. The firmware has FAT mounted, and host writes would corrupt it.
- On the Wi-Fi board, an upload holds `WEBRTC_VIDEO_PAUSE_STORAGE_UPLOAD` for the life of its writer lease, because video shares the radio link; audio stops with it. The hook set by `jetkvm_virtual_media_set_upload_writer_hook()` fires `true` in `jetkvm_virtual_media_claim_upload()` and `false` in `clear_upload_owner_locked()`, the only function that ends a lease. `JETKVM_UPLOAD_PAUSES_VIDEO` in `main/main.c` selects the boards; Ethernet keeps video during uploads.

## Hardware

- SDMMC pins: GPIO43 CLK, GPIO44 CMD, GPIO39 to GPIO42 for D0 to D3. Slot 0 uses IO_MUX, slot 1 the GPIO matrix.
- On-chip LDO channel 4 powers the GPIO39 to GPIO48 IO domain and the external 51 kΩ pull-ups. Firmware sets it to 3.3 V before configuring any TF pin and hands it to the SDMMC driver, so UHS-I can switch signalling to 1.8 V. Card power stays at 3.3 V.
- GPIO46 switches card power through Q1, a high-side P-MOSFET. At boot and insertion, firmware holds the bus low to avoid back-powering the card, cuts power for 500 ms, then starts SDMMC. A card left in SPI mode still needs a physical power cycle.

CMake cache options `JETKVM_TF_SDMMC_SLOT`, `JETKVM_TF_SDMMC_WIDTH` and `JETKVM_TF_SDMMC_FREQ_KHZ` (production: 4-bit, 40 MHz; above 40 MHz enables UHS-I) and the `JETKVM_TF_*` self-test switches are for diagnostics only.

Tests are in `tests/` (`run_static_test.sh`).
