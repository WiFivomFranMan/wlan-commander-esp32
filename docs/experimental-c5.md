# Experimental WLAN Commander C5 firmware

The current screenless build is **WLAN Commander C5 All-in-One**, version **2026.11.26**, built **November 26, 2026**. It reports its product name, build date and version over USB and BLE. Source revisions are diagnostic metadata. Current touchscreen bundles keep the October 28 launcher/boot bytes and replace their sensor slots with the November 26 rx5 build. November 27 limited hardware checks: one 8 MB screenless board and one Touch-LCD-1.69 board flashed the exact published regions with independent readback verification. Pixel 6 Pro / Android 16 / app build 170 received BLE AP/client results on channels 6 and 36 and stopped both boards; USB status independently confirmed Stop. The 1.69 launcher framebuffer and software Sensor navigation were checked. These observations do not qualify physical touch, RF tools, ranging, sustained operation, reconnect or combined-device topologies. The 2.8-inch November sensor slot remains hardware QA pending.

The browser installer displays hardware specs separately from the running firmware identity. Firmware that does not report an identity, including boards in ROM download mode, is shown as unknown. Installation logs follow new output; scroll up to pause following. Leave Erase device unchecked to retain NVS on a compatible update.

[November 26 screenless release](https://github.com/WiFivomFranMan/wlan-commander-esp32/releases/tag/c5-all-in-one-2026.11.26) · [November 26 touchscreen bundles](https://github.com/WiFivomFranMan/wlan-commander-esp32/releases/tag/c5-touch-2026.11.26-9b539ed).

## October 12 touchscreen release and prior qualification

Use the [model-specific browser installer](https://wlancommander.com/sensors/esp32-c5/flash/) or download the exact image ZIP from the [immutable prerelease](https://github.com/WiFivomFranMan/wlan-commander-esp32/releases/tag/c5-experimental-2026.10.12-9e760afc).

* Screenless 8 MB: 2026.10.12+9e760afc; four first-install regions.
* Touch-LCD-2.8, 32 MB: launcher 2026.10.12+f162839b; five regions, including sensor in ota_1.
* Touch-LCD-1.69, 16 MB: launcher 2026.10.12+89df13ec; five regions; hardware QA pending.

All application binaries are experimental. Metadata in experimental.json and touchscreen.json describes exact files, offsets, sizes and SHA-256. Each ZIP includes matching configuration. Source is readable under firmware; complete corresponding-source downloads also contain managed dependencies and the shared pinned SDK/library sources. Download both archives for touchscreen builds. Preserve component licenses; GPL-3.0-or-later applies to the combined SDR variant.

The browser rejects other ESP32 families and mismatched flash capacity before writing. Models of equal capacity still require correct manual selection. It verifies each downloaded SHA-256 and each write's device MD5. Do not erase all NVS if retaining device approval. Use all four/five manifest regions for a first installation; an application binary alone is incomplete. A BOOT/RESET sequence may be needed for USB download mode.

## Historical October qualification and limits

16 physical screenless 8 MB boards passed flashing and region digest verification. Runtime idle state, HS2.0 scope and USB spectrum were checked on a subset. This does not establish 16-board simultaneous operation. One 2.8-inch touchscreen passed launcher screenshot checks, default unchecked Interworking option, toggle both ways, and signed AirHORN approval persistence; these were USB console UI hooks, not independent finger-tap QA. Local spectrum reported active, 8,233 frames and 549 sweeps. The spectrum marketing image shows its selection menu. No new physical 1.69-inch, independent RF transmission or actual FTM qualification is claimed.

Screenless static SRAM margin: 728 bytes. Spectrum covers 2.4/5 GHz and has relative, uncalibrated, gapped levels; no 6 GHz. RF requires device approval and an explicit per-session shielding acknowledgement. No RF was started for approval or marketing captures. September stable OTA remains unchanged.

Passpoint defaults to advertised WFA HS2.0 capability. Include Interworking-only is optional; advertising capability does not prove successful authentication.

## October 28 touchscreen update

Both touchscreen image ZIPs include the matching `touch-airhorn-v4` sensor and launcher. AirHORN adds a finite DFS radar repeat workflow (1–100 events with a configurable gap), Stop during gaps, and one-tap 0/10/20 dB attenuation presets. Lower attenuation means stronger output; output power remains uncalibrated.

The 2.8-inch application slots were flashed and digest verified. USB checks observed all 20 events and Stop during a gap, and the new preset controls were rendered on the board. The operator reported analyzer blips; waveform timing and AP DFS detection remain unverified. The 1.69-inch pair passed builds and image/source checks without physical QA. Device approval and per-session RF containment acknowledgement remain required. Stable and screenless downloads are unchanged.
