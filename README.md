# WLAN Commander — ESP32-C5 sniffer

Firmware that turns an ESP32-C5 devkit into a 2.4 / 5 GHz management-frame sensor for
[WLAN Commander](https://wlancommander.com/), talking to the app over Bluetooth LE.

**Flash it from a browser: <https://wlancommander.com/sensors/esp32-c5/flash/>**
(desktop Chrome or Edge — Web Serial exists nowhere else.)

Current guide: https://wlancommander.com/sensors/esp32-c5/

The old GitHub Pages guide redirects to the current site. Firmware, manifest and OTA URLs
remain available for existing devices and integrations.

## Current firmware channels

| Channel / model | Firmware | Use |
|---|---|---|
| Experimental screenless C5 · 8 MB | WLAN Commander C5 All-in-One · built October 20, 2026 | Latest browser default: BLE capture, USB spectrum, approved RF tools |
| Touch-LCD-2.8 · 32 MB | `2026.10.12+f162839b` launcher | HS2.0-only Passpoint default; optional Interworking-only results |
| Touch-LCD-1.69 · 16 MB | `2026.10.12+89df13ec` launcher | Same features; build verified, hardware QA pending |
| Legacy stable screenless C5 · 4/8 MB | `2026.09.07+1876915` | Explicit browser option; existing stable OTA remains unchanged |

[October 12 prerelease, image ZIPs, signatures and complete source](https://github.com/WiFivomFranMan/wlan-commander-esp32/releases/tag/c5-experimental-2026.10.12-9e760afc).
[Current installation and qualification](docs/experimental-c5.md) ·
[Touchscreen source](firmware/c5_touch/) · [Publication runbook](docs/publishing.md).

The all-in-one remains experimental and GPL-3.0-or-later. Sixteen screenless boards were flashed with matching region digests; the 2.8-inch launcher was checked through device screenshot/tap console hooks. The 1.69-inch has no physical QA for this release. The web installer supports ESP32-C5 only, checks flash capacity and hashes, and uses model-specific layouts. Download both corresponding-source archives to rebuild touchscreen applications with the exact shared SDK dependencies. No GitHub Actions are used.

Touchscreen Passpoint matches the WFA HS2.0 vendor IE by default. Enable **Include Interworking-only** for 802.11u Interworking advertisements. An advertised IE is not proof of successful service authentication. AirHORN requires a signed device approval and per-session shielding acknowledgement.

Stable files and OTA retain their September version. The stable instructions below describe that release.

## What it does

Promiscuous capture on 2.4 and 5 GHz at 20 or 40 MHz. Every management and control frame is
wrapped in a synthesized radiotap header and streamed to the phone as a BLE GATT notification,
where the app parses it like any other capture source.

It also sends GAS/ANQP queries to Passpoint APs and captures the replies — the same
pre-association query a phone makes before joining a hotspot. That is what fills in NAI realms,
roaming consortiums, domains and venue names.

**It boots idle.** No promiscuous mode, no hopping, no transmission — BLE advertising only —
until the app sends `0x04 SET_RUN 1`. Subscribing to the frame characteristic starts nothing.
If the phone goes away for 15 seconds the board stops itself, so a crash, a flat battery or
walking out of range leaves nothing running.

ANQP querying is off by default and additionally requires `run=1`: `run=0` means no
transmission under any setting.

## Which board

Any ESP32-C5 devkit with a USB data connection works. Two worth naming:

| Board | Flash | |
|---|---|---|
| [Seeed XIAO ESP32-C5](https://www.seeedstudio.com/Seeed-Studio-XIAO-ESP32C5-p-6609.html) | 8 MB + 8 MB PSRAM | developed and tested on this |
| [M5Stack Stamp-C5](https://docs.m5stack.com/en/core/Stamp-C5) | 4 MB | runs this image, with nothing spare |

**Check the flash size before buying** — C5 boards ship as either 4 MB or 8 MB and listings do
not always say so plainly. This image is built for a 4 MB layout and runs on both: the sketch
is 1.33 MB in a 1.92 MB app slot (~71%), with a second slot the same size for updates. A 4 MB
board is not a mistake. It is close to full, though, so 8 MB is the better choice if you are
buying now.

## Flashing by hand

```
esptool --port /dev/cu.usbmodemXXXX --no-stub write-flash \
  --flash-mode dio --flash-freq 80m --flash-size 4MB \
  0x2000  firmware/wlc_sniffer.ino.bootloader.bin \
  0x8000  firmware/wlc_sniffer.ino.partitions.bin \
  0xe000  firmware/boot_app0.bin \
  0x10000 firmware/wlc_sniffer.ino.bin
```

`--no-stub` is deliberate: on the boards tested here the stub loader often fails to start, and
the failure reads as a dead board rather than a flashing problem.

**These boards accept about one flashing session per USB enumeration.** If an attempt fails
partway, unplug and replug before retrying rather than hammering a wedged port. A board that is
silent or boot-looping needs **BOOT** held while plugging in (or BOOT held and **RESET**
tapped) to force ROM download mode — and BOOT released afterwards, or it will write the image
and then sit in download mode instead of running it.

## Verifying

A flashed board advertises over BLE as `WLC C5 SNIFFER` and prints a status line on serial
every two seconds. Freshly flashed, idle:

```
run=0 poke=0 filt=3 width=0 cap=0 fwd=0 ... 2.4=0 5G=0 ch=1 sub=0
```

`run=0`, `cap=0` and `2.4=0 5G=0` together mean it has captured nothing and transmitted
nothing — which is what a board should look like before anyone asks it to work.

## Building

Arduino CLI, with the partition scheme that matters:

```
arduino-cli compile \
  --fqbn "esp32:esp32:esp32c5:PartitionScheme=min_spiffs,CDCOnBoot=cdc,DebugLevel=none" \
  --output-dir build .
```

The default partition scheme gives the app 1,310,720 bytes and this sketch does not fit —
it links at ~1.39 MB and needs `min_spiffs`, which gives 1,966,080.

Requires the `esp32` core (3.3.x) and NimBLE-Arduino.

## Licence and lineage

MIT — see [LICENSE](LICENSE).

The dual-band C5 capture recipe is borrowed from CodeHedge's
[ESP32DualBandWardriver](https://github.com/CodeHedge/ESP32DualBandWardriver), MIT. The sniffer
itself, the BLE protocol, the fragmentation scheme and the ANQP work are this project's.
