# Combined C5 sensor, spectrum and RF generator

This GPL variant retains the copied WLAN Commander sniffer/BLE and ESP-SDR
protocols, and adds standalone LCD/touch controls for the AirHORN tool. It builds
for the Waveshare 2.8-inch and 1.69-inch boards with `-DWLC_BOARD=2_8|1_69`.
The launcher remains in ota_0 and this image in ota_1. No LVGL is linked here.

See the repository [README](../../README.md#touchscreen-airhorn-and-dfs-events)
for touchscreen controls, preset provenance and verification limits.

## USB control and radio ownership

`MODE GENERATOR` replies `OK REBOOT` and consumes a one-shot RTC request before
entering raw PHY mode. Normal Wi-Fi/BLE is not initialized. No RF starts until a
separate TX command arrives. Entry refuses active/pending BLE capture and FTM.
`RELEASE` uses a full RTC watchdog reset to restore idle Wi-Fi/BLE; SDK software
restart from raw PHY mode was unreliable on the test board.

| Command | Behavior |
|---|---|
| `MODE?`, `WLCINFO?` | Mode and board identity |
| `MODE GENERATOR` | Reboot into exclusive host RF-test mode |
| `TXINFO?` | JSON state, remaining time and limits |
| `TX PACKET channel backoff duration_ms` | Repeated 6 Mb/s OFDM packets, 20 MHz configuration |
| `TX CW channel backoff duration_ms` | Diagnostic tone |
| `TXSTOP` | Cancel the whole local run or stop a host burst |
| `PING` | Renew the host generator lease |
| `RELEASE`, `MODE SNIFFER` | Stop and reboot to idle sniffer/BLE |
| `GENLOCAL?` | Standalone run state and touch health |
| `LCDTAP x y` | Headless landscape button action; requires a healthy touch controller |
| `LCDSHOT?` | Incrementally streamed deterministic RGB565 screen rendering |
| `RADARINFO?` | Experimental event software timing/counts, qualification flags |
| `LAUNCHER` | Stop and reboot to ota_0 |

Channels are 1–11; 36–64 and 100–144 in steps of four; and 149, 153, 157, 161,
165. DFS channels are supported. Host burst duration is 1–30,000 ms. Attenuation
is 0–80 quarter-dB units; default 80 means 20 dB backoff, not calibrated dBm.
Five seconds without accepted TX or explicit PING stops a host burst and returns
to idle. Queries and invalid commands do not renew that lease.

Launcher-owned runs consume validated settings and a one-shot NVS boot request,
then remain Ready until Start. Local ownership rejects host TX even while Ready.
The local control loop renews its watchdog only after a healthy pass; host loss
therefore does not end a healthy finite local run. A separate 50 ms esp_timer
resets after the finite deadline plus grace if normal stopping stalls. This
requires functioning scheduling/interrupts; injected PHY stalls are untested.
Screenshot output does not wait for USB backpressure and cannot renew RF leases.

## Experimental radar backend

The fixed FCC/ETSI examples are finite CW-gated events, not a certification suite.
A priority-2 worker controls the pinned C5 tone-enable register; short critical
sections gate each pulse. Software late intervals abort instead of compressing
missed pulses. Each event lasts less than 100 ms; normal-loop Stop is serviced
after that bounded event. The separate fail-off timer remains active.

The public tuning API produces a brief CW setup transient before gating begins.
The UI and `RADARINFO?` disclose it and report RF timing as unverified. CPU timing
and pulse counts are not RF measurements. Chirped and frequency-hopping patterns
are unsupported. Independent receiver measurements and AP detection tests remain
necessary before qualifying radar behavior. The older WifiMetrix binary was
inspected as data; no embedded radar presets were recovered.

## Reproduce and check the build

Use the private build host configured in the root README:

```sh
scripts/remote-build.sh wlc_sensor 2_8
scripts/remote-build.sh wlc_sensor 1_69
.venv/bin/python firmware/wlc_sensor/check_build.py --build artifacts/wlc_sensor-2_8 --config artifacts/wlc_sensor-2_8/sdkconfig
make -C tests/generator check
```

Pins are in `dependency-lock.json`: IDF 5.5.5, Arduino 3.3.11, NimBLE 2.5.1 and
ESP-SDR 550fadea4d00a9e26ce921c5832167becb3dc20c. Never modify another task's SDK.
The checker verifies RF scratch address/type/size, 128 KiB heap exclusion, SRAM
section ends, SDK configuration and image size. Preserve the build report,
generated source manifest and sdkconfig. Artifacts are named `wlc_sensor.bin`.

Before flashing, identify the board and verify two matching independent full
backups outside /tmp. `device_tool.py flash --slot ota_1` writes only this app,
preserving bootloader, partition table, ota_0, otadata and NVS. Do not operate the
USB console during flashing or verification. Scripts never trigger GitHub Actions.

RF approval: touch-airhorn-v3 rejects generator entry without a signed device
approval and fresh shielding acknowledgement. Session limit15min; see the root
README for protocol, installation, offline revocation and hardware evidence.
