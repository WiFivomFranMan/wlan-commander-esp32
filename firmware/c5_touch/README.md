# ESP32-C5 touchscreen launcher and sensor source

October 12, 2026 source snapshot for Waveshare Touch-LCD-2.8 (32 MB, 240x320) and Touch-LCD-1.69 (16 MB, 240x280).

The launcher defaults Passpoint to WFA HS2.0 advertisements, with an unchecked Include Interworking-only option. These are advertised capabilities, not proof of authentication. Matching sensor applications provide spectrum and approval-gated RF tools. AirHORN device approval remains separate from per-session unlock.

Full configuration and managed dependencies are in c5-touch-source.tar.xz in the release. Shared pinned ESP-IDF/Arduino/NimBLE/ESP-SDR/ESP-DSP source is in c5-all-in-one-source.tar.xz; download both. Read the dependency lock and CMake configuration before building. Source paths in the frozen dependency lock must be relocated to the extracted source directories. Build wlc_touch for the selected board with the release sdkconfig, and build wlc_sensor with its matching sensor-sdkconfig. Use tools/device_tool.py to identify hardware and tools/screenshot.py to capture the launcher framebuffer. Never use another model's firmware or flash size.

Region layout: bootloader 0x2000, partition table 0x8000, otadata 0xd000, launcher 0x20000, sensor 0x720000. Keep NVS at 0x9000 to retain device approval. Do not erase all flash.

2.8-inch: flashed and USB console UI checks completed. 1.69-inch: build verified; no physical QA for this release. See ../../docs/experimental-c5.md for exact limitations. Preserve original component license notices.

## DFS radar repeat (development)

AirHORN's Workflow selector includes **DFS radar repeat**, alongside the original
one-shot DFS radar option. Set **Event count** (1–100) and **Gap between events
(ms)** (1–30,000); each event uses the selected preset unchanged. Start runs the
finite sequence. The generator shows the event number and quiet gap; Stop cancels
the remaining events, including while waiting in a gap. There is no final gap.

The firmware reserves up to one second of PHY setup time per radar event when
checking the remaining 15-minute session budget. Approval, a fresh containment
acknowledgement, session expiry and the independent stop guard still apply.
Each event includes the existing CW setup transient. Repeat improves the chance
of a swept analyzer observing a signal; it does not establish correct radiated
pulse timing or access-point DFS detection.

Build/install both the matching launcher and `touch-airhorn-v4` sensor image.
The 48-byte saved configuration layout is unchanged; workflow 3 stays one-shot,
workflow 4 repeats using the existing count and off-time fields. NVS and the signed
device permit must be preserved during an application-slot update.
