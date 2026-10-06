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
   Keep stable `data/releases.json`, browser-flash manifest and OTA metadata
   unchanged unless a stable promotion was separately tested and authorized.
7. Deploy only the website's `public/` directory to the existing Cloudflare Pages
   production project from an owned machine. Check live guide/download links,
   fetch and hash every release asset, verify signatures again, and compare the
   live stable catalogue and firmware with their pre-publication hashes.
8. Record repository revision, tag, asset hashes, deployment ID and live checks
   in a persistent publication receipt. A Git push, GitHub release and website
   deployment are separate operations. Completion requires public readback.

To roll back a broken website link, publish a corrective website commit and deploy.
Withdraw or annotate a bad experimental release without reusing its tag or files.
Do not change stable OTA in order to repair an experimental download.
