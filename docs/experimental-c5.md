# Experimental combined ESP32-C5 firmware

Build: **2026.10.03+1cff3220** · published October 6, 2026 · runtime identity `airhorn-dev`.

This is the exact combined image bundled with unified sensor 2.1.29 and the
2.1.30 candidate, not a new firmware compilation. It combines BLE capture,
relative USB spectrum snapshots and explicitly started finite RF tests.
The stable BLE sniffer and its OTA/flash manifests remain separate.

## Downloads and installation

Use the versioned [prerelease](https://github.com/WiFivomFranMan/wlan-commander-esp32/releases/tag/c5-experimental-2026.10.03-1cff3220).
`c5-all-in-one-image.zip` contains all four flash regions, offsets, exact
`sdkconfig`, generated source metadata and the build report.
`c5-all-in-one-source.tar.xz` supplies corresponding GPL source and SDK/dependency
snapshots. `experimental.json` records all sizes, SHA-256 digests and pinned
revisions. Detached signatures and the public release key accompany downloads.
These signatures authenticate download files; this firmware does not implement
a signature-enforcing OTA updater or ESP secure boot.

The qualified physical boards have **8 MB flash**. The image uses a 4 MB layout,
but a 4 MB board has not been qualified for this combined build. This firmware
is ESP32-C5 only; do not install it on C3, C6, S3 or generic ESP32 boards.

Save two independently read, matching complete flash backups before changing a
working board. Download and verify the image ZIP, then extract it. Use a USB data
cable and desktop esptool. Close serial monitors. Identify the board and its port;
enter ROM by holding BOOT, tapping RESET and releasing BOOT if needed.

```sh
python -m esptool --chip esp32c5 --port YOUR_PORT \
  --before no-reset --after watchdog-reset write-flash \
  --flash-mode dio --flash-freq 80m --flash-size 4MB \
  0x2000 bootloader.bin 0x8000 partition-table.bin \
  0xe000 ota_data_initial.bin 0x10000 wlc_sniffer_sdr.bin
```

Verify the flash regions against the manifest before use. If flashing fails,
unplug/reconnect and return to ROM before retrying; do not repeatedly write a
wedged port. The two-board programming evidence used standalone USB JTAG OpenOCD;
the command above documents the four-part serial installation, not a newly
qualified browser/serial flash on this exact bundle. Complete reads using the
default 4096-byte esptool payload failed on the tested boards; 2048-byte reads
worked. A failed default read is not proof of a bad board.

Boot must identify `airhorn-dev`, sniffer mode, `run=false`, `ftm=false` and BLE
initialized. It starts idle. BLE capture can be used by the mobile app; spectrum
and RF controls use USB through a WLAN Commander Linux sensor with the C5 helper
and web tools. Connect each C5 with its own USB data port. The sensor's C5 firmware
page never flashes automatically. Spectrum, capture/FTM and RF generation have
exclusive ownership; stop the current task before switching modes.

RF tests require explicit Start. Bursts are limited to 30 seconds, and a five
second explicit heartbeat lease stops RF and returns to idle after owner loss.
Read-only status polling does not renew that lease. There is no DFS or 6 GHz
transmit. Levels, output power and duty cycle are uncalibrated.

Recovery is a full restoration of the verified original backup using its
original flash settings, followed by complete readback. Restore verification of
the current backups was not executed in the two-board experiment; keep them.

## What passed and what remains experimental

On this exact image, two physical C5 boards passed matching flash-region
readbacks, 119 private Pi HTTPS API checks, real BLE frame notifications,
exclusive ownership, Stop and owner-loss idle restoration. Grouped spectrum was
checked from a native iPhone simulator using both real USB boards. Independent
receive measurements observed finite CW carrier presence on channels 1 and 36.
Those observations do not establish calibrated power, packet waveform/duty,
three-to-six-board behavior, an injected PHY stall response, or actual FTM
ranging. Physical iPhone UI and Android native C5 Spectrum were not qualified.
Spectrum covers only 2.4/5 GHz, has gaps, and is relative rather than calibrated dBm.
The build has **928 bytes of static SRAM margin**. It is an experimental opt-in,
not the stable auto-update target.

## Reproduce and review the source

The tracked `firmware/wlc_sniffer_airhorn/` and sibling `firmware/wlc_sniffer/`
match the binary's build input hashes. Follow [the source build guide](../firmware/wlc_sniffer_airhorn/README.md).
The complete corresponding source archive carries SDK dependencies and exact
build configuration. It is supplied alongside the binary, not just as GitHub's
automatic source ZIP. Dependency commits and generated-input hashes are pinned.
No rebuild or new hardware test is claimed by this publication.

## License and attribution

The original stable sniffer is MIT. The combined variant is **GPL-3.0-or-later**,
including its ESP-SDR integration; see its LICENSE and NOTICE. Preserve the
upstream ESPARGOS, Espressif, NimBLE and CodeHedge notices and dependency licenses.
The repository root MIT license does not override the combined variant's license.
