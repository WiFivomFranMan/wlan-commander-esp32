#!/usr/bin/env python3
"""Capture ESP-SDR frames from the sensor image over USB and print the bins around DC.

usage: spectrum_dump.py PORT FREQ_MHZ ["COMMAND" ...]   e.g. "BANDWIDTH 48"
"""
import os, sys, time, json
sys.path.insert(0, os.path.join(os.path.dirname(__file__), "../firmware/wlc_sensor"))
import probe_usb as p

port = p.open_native_usb(sys.argv[1]); freq = int(sys.argv[2])
time.sleep(0.2); port.reset_input_buffer()
p.expect(port, "MODE SPECTRUM", "OK")
for extra in sys.argv[3:]: print(extra, "->", p.command(port, extra))
p.expect(port, f"FREQ {freq}", "OK")
header = p.command(port, "SPEC 100 1 4 0 0 256").split()
frames = []
while True:
    magic = p.exact(port, 4)
    if magic == b"SPEC": port.readline(); break
    frame = magic + p.exact(port, 256 + 28)
    frames.append(frame[28:28 + 256])
avg = [sum(f[i] for f in frames) / len(frames) for i in range(256)]
shifted = avg[128:] + avg[:128]          # DC to the middle
print(json.dumps({"frames": len(frames), "median": sorted(avg)[128],
                  "around_dc": [round(v, 1) for v in shifted[120:137]],
                  "coarse_8bin_max": [round(max(shifted[i:i+8])) for i in range(0, 256, 8)]}))
if os.environ.get("RAW_OUT"):
    with open(os.environ["RAW_OUT"], "ab") as f: f.write(bytes(round(v) for v in avg))   # FFT order, DC first
port.write(b"RELEASE\n"); port.readline()
