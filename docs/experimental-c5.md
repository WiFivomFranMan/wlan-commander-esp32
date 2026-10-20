# Experimental WLAN Commander C5 firmware

The current screenless build is **WLAN Commander C5 All-in-One**, version **2026.10.20**, built **October 20, 2026**. It reports its product name, build date and version over USB and BLE. Source revisions are diagnostic metadata. Touchscreen images below retain their October 12 binaries.

The browser installer displays hardware specs separately from the running firmware identity. Firmware that does not report an identity, including boards in ROM download mode, is shown as unknown. Installation logs follow new output; scroll up to pause following. Leave Erase device unchecked to retain NVS on a compatible update.

[October 20 screenless release](https://github.com/WiFivomFranMan/wlan-commander-esp32/releases/tag/c5-all-in-one-2026.10.20).

## October 12 touchscreen release and prior qualification

Use the [model-specific browser installer](https://wlancommander.com/sensors/esp32-c5/flash/) or download the exact image ZIP from the [immutable prerelease](https://github.com/WiFivomFranMan/wlan-commander-esp32/releases/tag/c5-experimental-2026.10.12-9e760afc).

* Screenless 8 MB: 2026.10.12+9e760afc; four first-install regions.
* Touch-LCD-2.8, 32 MB: launcher 2026.10.12+f162839b; five regions, including sensor in ota_1.
* Touch-LCD-1.69, 16 MB: launcher 2026.10.12+89df13ec; five regions; hardware QA pending.

All application binaries are experimental. Metadata in experimental.json and touchscreen.json describes exact files, offsets, sizes and SHA-256. Each ZIP includes matching configuration. Source is readable under firmware; complete corresponding-source downloads also contain managed dependencies and the shared pinned SDK/library sources. Download both archives for touchscreen builds. Preserve component licenses; GPL-3.0-or-later applies to the combined SDR variant.

The browser rejects other ESP32 families and mismatched flash capacity before writing. Models of equal capacity still require correct manual selection. It verifies each downloaded SHA-256 and each write's device MD5. Do not erase all NVS if retaining device approval. Use all four/five manifest regions for a first installation; an application binary alone is incomplete. A BOOT/RESET sequence may be needed for USB download mode.

## Qualification and limits

16 physical screenless 8 MB boards passed flashing and region digest verification. Runtime idle state, HS2.0 scope and USB spectrum were checked on a subset. This does not establish 16-board simultaneous operation. One 2.8-inch touchscreen passed launcher screenshot checks, default unchecked Interworking option, toggle both ways, and signed AirHORN approval persistence; these were USB console UI hooks, not independent finger-tap QA. Local spectrum reported active, 8,233 frames and 549 sweeps. The spectrum marketing image shows its selection menu. No new physical 1.69-inch, independent RF transmission or actual FTM qualification is claimed.

Screenless static SRAM margin: 728 bytes. Spectrum covers 2.4/5 GHz and has relative, uncalibrated, gapped levels; no 6 GHz. RF requires device approval and an explicit per-session shielding acknowledgement. No RF was started for approval or marketing captures. September stable OTA remains unchanged.

Passpoint defaults to advertised WFA HS2.0 capability. Include Interworking-only is optional; advertising capability does not prove successful authentication.
