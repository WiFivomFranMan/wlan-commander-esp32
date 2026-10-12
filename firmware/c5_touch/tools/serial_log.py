#!/usr/bin/env python3
"""Read a board's USB-Serial/JTAG console without resetting it (DTR/RTS untouched)."""
import os, sys, time
sys.path.insert(0, os.path.join(os.path.dirname(__file__), "../firmware/wlc_sensor"))
from probe_usb import open_native_usb  # opens without the DTR/RTS pulse that resets the chip
port, seconds = sys.argv[1], float(sys.argv[2]) if len(sys.argv) > 2 else 8
s = open_native_usb(port)
s.timeout = 0.2
end = time.time() + seconds
while time.time() < end:
    data = s.read(4096)
    if data: sys.stdout.write(data.decode("utf-8", "replace")); sys.stdout.flush()
s.close()
