# ESP32 publication runbook

Build and publish on owned Macs/Linux machines only. **Never use GitHub Actions.**

1. Freeze the exact firmware input revisions, dependency lock, flash layout and
   generated-input hashes. Preserve existing stable files. Compare every region
   and matching source against the qualified bundle; do not relabel a rebuild as
   the previously tested binary.
2. Keep stable and experimental releases separate. Stable entrypoints are
   `ota.json`, `manifest.json` and the firmware paths they name. Experimental
   metadata is `experimental.json`; use an immutable GitHub prerelease tag and
   versioned assets. Experimental is not an automatic update promotion.
3. Publish readable firmware/build source, complete corresponding dependency
   source, exact configuration, license notices, a scoped hardware summary, all
   four first-install regions and SHA-256 checksums. Do not publish lab MACs,
   SSIDs, IPs, credentials, private keys or raw capture evidence.
4. Sign the release ZIP, corresponding source archive and application binary
   with the existing WLAN Commander pinned release key on the Mac. Publish only
   the public key. Verify each detached signature before upload. This download
   signature does not add secure boot or signature enforcement to ESP firmware.
5. Commit and push focused source/docs changes. Create a **draft prerelease**,
   upload immutable assets, compare uploaded sizes and hashes, then publish it
   as a prerelease with `make_latest=false`. Never overwrite published bytes.
6. Update the maintained `wlancommander.com` website source, experimental guide
   and experimental download metadata, then build and run its existing gates.
   Keep stable `data/releases.json` and OTA metadata unchanged unless a stable
   promotion was separately tested and authorized (see *Promoting a stable sniffer*).
   Every experimental image still gets its `releases.json` entry and `known_versions`
   so the app can judge those boards. The October 12 owner-authorized
   browser update selects the experimental headless build by default and retains
   the unchanged stable bytes as an explicit model-selector option.
7. Deploy only the website's `public/` directory to the existing Cloudflare Pages
   production project from an owned machine. Check live guide/download links,
   fetch and hash every release asset, verify signatures again, and compare the
   live stable catalogue and firmware with their pre-publication hashes.
8. Record repository revision, tag, asset hashes, deployment ID and live checks
   in a persistent publication receipt. A Git push, GitHub release and website
   deployment are separate operations. Completion requires public readback.

## Every firmware fix must be published — the gate

The rx4/rx5 Bluetooth forwarding fixes (2026-10-31, 2026-11-04) sat unpublished for weeks while
every image on the website still had the bug where a board stops forwarding frames after a few
minutes. A user flashed a board from the site and got it. Three things now prevent a repeat:

- **Source gate (WLAN Commander repo).** `firmware/published.json` records the sha256 of every
  source file each published image was built from. `scripts/check-c5-firmware-published.py`
  fails when a recorded file changed, or a new source file joined a build, without a new
  publication. It runs in the `quick` and `integration` QA profiles (`c5-firmware-published` in
  `qa/checks.json`), so no mobile release qualifies while a firmware fix is unpublished.
  There is no allowlist. The fix is to publish, then update that image's entry **in the same
  change**.
- **Catalogue gate.** After deploying, run
  `scripts/check-c5-firmware-published.py --catalogue https://wlancommander.com/releases.json`.
  It fails unless `releases.json` lists every image (`esp32-c5`, `esp32-c5-all-in-one`,
  `esp32-c5-touch`) at the version `published.json` records, with that version in
  `known_versions`.
- **The app tells the user.** Core's `check_c5_firmware_update` compares the board's status line
  (`fw=`, `product=`, `version=`) with `releases.json` and shows "Firmware update available" on
  the board's device page. A board only learns it is out of date if the old version is still
  in `known_versions`, so **never drop a previously published version from that list.**

## Promoting a stable sniffer

1. Build with `scripts/flash-c5.sh` in the WLAN Commander repo from a clean, pushed commit, so
   `fw=` names that commit. Flash one board and read back `fw=`. Keep the build output.
2. The bootloader, partition table and `boot_app0` should be byte-identical to the previous
   stable; compare hashes. A difference means the toolchain or layout moved, so stop and work out why.
3. Package: four regions, a source tarball (`wlc_sniffer.ino`, `wlc_rx_contract.h`,
   `build.options.json`, `flash_args`, LICENSE, BUILD.md), `.sha256` for each, and `.sig` for the
   application binary and the source tarball (release key; verify each signature).
4. Here: add `firmware/<version>/` (never overwrite an earlier version's bytes), point
   `manifest.json` and `ota.json` at it, update `wlc_sniffer.ino`/`wlc_rx_contract.h` and the
   README table. Tag `c5-sniffer-<date>-<sha>` and publish a GitHub release with the assets.
5. Website: copy the regions to `public/firmware/esp32-c5/<version>/`, update
   `public/sensors/esp32-c5/flash/stable.json`, `content/imported/esp32/manifest.json`, the
   flasher model description and `data/releases.json` (`version`, `known_versions` keeping
   every earlier version, `download_url`, `sig_url`, `size_bytes`, `sha256`). Build, validate,
   deploy, then run the catalogue gate against the live URL.
6. WLAN Commander repo: update the `esp32-c5` entry in `firmware/published.json` and commit.

To roll back a broken website link, publish a corrective website commit and deploy.
Withdraw or annotate a bad experimental release without reusing its tag or files.
Do not change stable OTA in order to repair an experimental download.
