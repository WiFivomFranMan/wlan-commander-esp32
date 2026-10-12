#!/usr/bin/env python3
"""Save what the touch launcher is showing: screenshot.py PORT out.png [cmd ...]

Optional commands (e.g. "tap 60 120", "wait 1.5") run first, in order.
"""
import base64, os, sys, time
sys.path.insert(0, os.path.join(os.path.dirname(__file__), "../firmware/wlc_sensor"))
from probe_usb import open_native_usb
from PIL import Image

def main(port_name, out, steps):
    sensor = bool(steps and steps[0] == "--sensor")
    if sensor: steps = steps[1:]
    port = open_native_usb(port_name); port.timeout = 3
    time.sleep(0.2); port.reset_input_buffer()
    for step in steps:
        if step.startswith("wait "): time.sleep(float(step.split()[1])); continue
        port.write((step + "\n").encode()); port.flush()
        print(step, "->", port.readline().decode(errors="replace").strip())
    port.reset_input_buffer()
    port.write(b"LCDSHOT?\n" if sensor else b"screenshot\n"); port.flush()
    w = h = None; rows = {}
    deadline = time.time() + 30
    while time.time() < deadline:
        line = port.readline().decode(errors="replace").strip()
        if line.startswith('{"screenshot":'):
            import json; d = json.loads(line)["screenshot"]; w, h = d["w"], d["h"]
        elif line.startswith("R") and w:
            idx, data = line[1:].split(" ", 1); rows[int(idx)] = base64.b64decode(data)
        elif "screenshot_end" in line: break
    if not w or len(rows) != h: sys.exit(f"incomplete screenshot: {len(rows)}/{h} rows")
    im = Image.new("RGB", (w, h))
    px = im.load()
    for y in range(h):
        r = rows[y]
        for x in range(w):
            v = r[2 * x] | (r[2 * x + 1] << 8)
            px[x, y] = (((v >> 11) & 31) * 255 // 31, ((v >> 5) & 63) * 255 // 63, (v & 31) * 255 // 31)
    im.save(out); print("saved", out, w, h)

if __name__ == "__main__":
    main(sys.argv[1], sys.argv[2], sys.argv[3:])
