# ESP32-C5 touchscreen launcher and sensor source

October 12, 2026 source snapshot for Waveshare Touch-LCD-2.8 (32 MB, 240x320) and Touch-LCD-1.69 (16 MB, 240x280).

The launcher defaults Passpoint to WFA HS2.0 advertisements, with an unchecked Include Interworking-only option. These are advertised capabilities, not proof of authentication. Matching sensor applications provide spectrum and approval-gated RF tools. AirHORN device approval remains separate from per-session unlock.

Full configuration and managed dependencies are in c5-touch-source.tar.xz in the release. Shared pinned ESP-IDF/Arduino/NimBLE/ESP-SDR/ESP-DSP source is in c5-all-in-one-source.tar.xz; download both. Read the dependency lock and CMake configuration before building. Source paths in the frozen dependency lock must be relocated to the extracted source directories. Build wlc_touch for the selected board with the release sdkconfig, and build wlc_sensor with its matching sensor-sdkconfig. Use tools/device_tool.py to identify hardware and tools/screenshot.py to capture the launcher framebuffer. Never use another model's firmware or flash size.

Region layout: bootloader 0x2000, partition table 0x8000, otadata 0xd000, launcher 0x20000, sensor 0x720000. Keep NVS at 0x9000 to retain device approval. Do not erase all flash.

2.8-inch: flashed and USB console UI checks completed. 1.69-inch: build verified; no physical QA for this release. See ../../docs/experimental-c5.md for exact limitations. Preserve original component license notices.
