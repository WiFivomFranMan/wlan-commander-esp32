# Experimental ESP32-C5 combined firmware — 2026.10.03+1cff3220

This prerelease publishes the exact combined firmware already bundled with
unified sensor 2.1.29 and the 2.1.30 candidate. It adds no new firmware rebuild
or hardware qualification. Runtime identity is `airhorn-dev`.

- BLE sniffer plus USB relative spectrum and explicitly started finite RF tests.
- Physical checks cover two **8 MB ESP32-C5** boards. Other ESP32 families and
  4 MB C5 hardware are not qualified for this combined build.
- Two-board flash-region readbacks, 119 private Pi web API checks, BLE frames,
  ownership/Stop/owner-loss checks, and simulator grouped spectrum passed.
  Independent RF measurements observed finite CW carrier presence on channels
  1 and 36; calibrated power/duty, packet attribution and actual FTM remain unqualified.
- Spectrum is relative, uncalibrated, gapped and limited to 2.4/5 GHz. RF output
  is finite, explicitly started and lease-bounded; no DFS or 6 GHz transmit.
- Three-to-six boards, physical iPhone UI, Android native C5 Spectrum, injected
  PHY stalls and full restoration of current backups remain unqualified.
  Static SRAM margin is **928 bytes**. This is an experimental opt-in.

## Downloads

`c5-all-in-one-image.zip` contains the four flash regions, exact configuration,
build report and generated source metadata. `c5-all-in-one-source.tar.xz`
contains matching GPL source and SDK/dependency snapshots.
`wlc_sniffer_sdr.bin` is application-only, not a first-install image.
`experimental.json`, SHA256SUMS, per-file checksums, detached RSA-SHA256
signatures and the existing `01-release-2026.pub` authenticate the downloads.
They do not enable secure boot or signature-enforcing ESP OTA.

Image ZIP SHA-256:
`ad7a395db36ba2e0b46745f87afaf5457dac164a2cbab4cd67fff4aae70e5a3d`

Corresponding source SHA-256:
`bafc8d4e3da40c56ba926c1389318d66af4d5c4ae32a993fdf6316ceb9bd3995`

Use the [installation, recovery and evidence guide](https://github.com/WiFivomFranMan/wlan-commander-esp32/blob/main/docs/experimental-c5.md)
and [pinned build source](https://github.com/WiFivomFranMan/wlan-commander-esp32/tree/main/firmware/wlc_sniffer_airhorn).
The combined variant is GPL-3.0-or-later; dependency notices are preserved.
The existing MIT stable sniffer, stable OTA and default browser flasher remain
unchanged. This prerelease does not automatically upgrade existing devices.
