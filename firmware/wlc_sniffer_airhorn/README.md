# Experimental C5 RF generator

This separate GPL variant combines WLAN Commander's sniffer, ESP-SDR snapshots,
and Espressif's non-signaling RF transmitter. It is a Mac bench experiment for
AirHORN's single-channel, traverse and pulse workflows. The Mac bench verified
2.4 GHz and all nine permitted 5 GHz packet/tone outputs. Pulse timing, explicit
stop and host-loss cessation were observed on channels 1 and 36. Calibrated power
and duty-cycle equivalence remain unverified; this is not a complete replacement.
The original sniffer, SDR source tree and Pi SDR board are separate.

## USB control

The continuous packet call runs in a separate task so USB commands and finite
stop deadlines remain responsive. Generator mode exclusively owns the radio.
Entry refuses active or pending BLE capture and FTM. It replies `OK REBOOT`, sets
a one-shot RTC request and reboots directly into raw PHY mode, without starting
normal Wi-Fi/BLE. The request is consumed before PHY initialization. No transmit
starts until a separate TX command arrives. Hot Wi-Fi teardown did not produce
measurable packets on the test board. Spectrum and generation cannot run together.
Release stops RF, replies `OK REBOOT` and uses a full RTC watchdog reset to restore
idle Wi-Fi/BLE. The host reconnects across both resets. SDK software restart from
raw PHY mode left this board in ROM and is not used for generator exit.

| Command | Behavior |
|---|---|
| `MODE?`, `WLCINFO?` | Mode and board identity |
| `MODE GENERATOR` | Reboot into exclusive RF-test mode |
| `TXINFO?` | JSON state, remaining time and limits |
| `TX PACKET channel backoff duration_ms` | Repeated 6 Mb/s OFDM packets; 20 MHz configuration |
| `TX CW channel backoff duration_ms` | Narrow diagnostic tone |
| `TXSTOP` | Stop and remain in generator mode |
| `PING` | Renew host lease |
| `RELEASE`, `MODE SNIFFER` | Stop and reboot to idle sniffer/BLE |
| `MODE SPECTRUM` | Existing SDR snapshots; refused in generator mode |

Allowed bench channels are 1–11 and 36, 40, 44, 48, 149, 153, 157, 161, 165.
DFS channels are excluded. Duration is 1–30,000 ms. Backoff is 0–80 in quarter-dB
units; start at 80 (20 dB attenuation). It is not calibrated absolute dBm.
Five seconds without an accepted transmit command or explicit `PING` stops RF and reboots to idle. Read-only `MODE?`, `WLCINFO?`, `TXINFO?`, capability queries and invalid requests do not renew the generator lease. Owner tools send `PING` separately from status queries. A separate 50 ms
esp_timer task reboots after a deadline plus 100 ms grace if a PHY call blocks
normal-loop stopping. It requires working scheduling and interrupts; physical
cessation still requires independent measurement.

CW is not AirHORN's channel-wide raw RF energy waveform. The measured channel 1
packet signal spans about 16.3 MHz; calibrated power and duty-cycle equivalence
remain unverified. Normal explicit stop and host-loss cessation were observed;
the software fallback was not tested with an injected PHY stall. Host tools
implement single channel, 5/30-second traversal and finite pulses; see the
[public evidence summary](../../docs/experimental-c5.md).

## Reproduce the build

Pins are in dependency-lock.json: IDF 5.5.5, Arduino 3.3.11, NimBLE 2.5.1 and
ESP-SDR 550fadea4d00a9e26ce921c5832167becb3dc20c. Use a private SDK copy.
The RF archive has 44,656 bytes of scratch BSS. prepare_rf_archive.py renames
seven pinned BSS sections in that copy; rf_buffers.ld places them at 0x40820000
in the existing reserved bank. Code, symbols, relocations and sizes are preserved.
This bank is shared exclusively with the packet pool; entry clears its first
64 KiB. Never patch the SDK used by the other SDR chat.

From the repository root with fresh dependencies and sdkconfig:

```bash
python3 firmware/wlc_sniffer_airhorn/fetch_dependencies.py --output /tmp/wlc-airhorn-deps
export IDF_TOOLS_PATH=/tmp/wlc-airhorn-idf-tools
/tmp/wlc-airhorn-deps/idf/install.sh esp32c5
. /tmp/wlc-airhorn-deps/idf/export.sh
python3 firmware/wlc_sniffer_airhorn/prepare_rf_archive.py --archive "$IDF_PATH/components/esp_phy/lib/esp32c5/librftest.a" --tool-prefix riscv32-esp-elf-
cd firmware/wlc_sniffer_airhorn
idf.py -B /tmp/wlc-airhorn-build -DIDF_TARGET=esp32c5 -DWLC_DEPS_DIR=/tmp/wlc-airhorn-deps build
python3 check_build.py --build /tmp/wlc-airhorn-build --config sdkconfig
```

The checker verifies RF scratch address/type/size, 128 KiB heap exclusion, SRAM
section ends, adctrig, SDK configuration and image size. The tested image has
928 bytes of static SRAM margin. The portal lease-fix build has a 1,257,568-byte application; the earlier RF bench image had 1,257,472 bytes. Their hardware evidence is recorded separately. Preserve the
combined-build-report.json, generated source manifest and sdkconfig. The filename
remains wlc_sniffer_sdr.bin for build compatibility. Scripts do not flash hardware.

The original host-tool tests and private bench evidence belong to the unified
sensor/app workspace. This public source snapshot supplies the firmware build
checker and matching generated-input hashes. See [the public exact-image summary](../../docs/experimental-c5.md)
for the checks that were actually executed.

Before flashing, identify the board and preserve two matching independent complete
flash backups outside /tmp. Verify full restoration after testing. Bench evidence
is in out/c5-airhorn; receiver baselines are not C5 transmit proof. This variant is experimental. The sensor C5 integration supplies grouped Spectrum and finite RF-test webpages; native iPhone spectrum needs the helper that also accepts `airhorn-dev`. RF controls use an independent authenticated operator lease and identity-keyed USB ownership.
