#!/usr/bin/env python3
"""Send line commands to the sensor image's USB protocol and print each reply line."""
import sys, time
sys.path.insert(0, __import__("os").path.join(__import__("os").path.dirname(__file__), "../firmware/wlc_sensor"))
from probe_usb import open_native_usb
port = open_native_usb(sys.argv[1]); port.timeout = 2
time.sleep(0.2); port.reset_input_buffer()
for cmd in sys.argv[2:]:
    port.write((cmd + "\n").encode()); port.flush()
    print(cmd, "->", port.readline().decode("utf-8", "replace").strip(), flush=True)
port.close()
