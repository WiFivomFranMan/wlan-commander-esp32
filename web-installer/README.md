# WLAN Commander model-specific USB installer

Source for the October 12 website installer. ESP Web Tools 10.4.0 (Apache-2.0), esptool-js and SparkMD5 are pinned in package-lock.json. Our modified flash implementation rejects wrong chip family/JEDEC flash capacity, verifies every part SHA-256 before any erase/write and supplies device MD5 verification after writing. Manual model selection is still required for hardware sharing capacity. See THIRD-PARTY-NOTICES.txt.

Build on an owned machine: npm ci; npm run test:firmware; npm run build:flasher. Output goes to public/assets/flasher. The build redirects ESP Web Tools flash imports to the modified implementation. No GitHub Actions.

The website supplies a #firmware-model select with manifest paths, descriptions in option data-description, #firmware-selected status and esp-web-install-button. Each manifest has explicit flash_sizes in bytes and parts with path, offset, bytes and SHA-256. The website defaults to the current experimental 8 MB screenless C5 image; legacy stable OTA is unchanged. Touch models use their own five-part image, capacity and offsets.

Automated guard tests cover incorrect families/capacities, unknown flash IDs and corrupt/incorrect-size downloads. The October 12 physical flashes used local hardware tools; the custom browser installer was not hardware-flashed in that QA run.
