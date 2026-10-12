#!/usr/bin/env python3
"""Drive the touch image's USB console: send commands, print the JSON replies.

usage: touch_cli.py PORT "cmd" [--wait SECONDS] "cmd" ...
The port is opened without toggling DTR/RTS, so the board is not reset.
"""
import json, os, sys, time
sys.path.insert(0, os.path.join(os.path.dirname(__file__), "../firmware/wlc_sensor"))
from probe_usb import open_native_usb  # opens without the DTR/RTS pulse that resets the chip

def main(argv):
    port, steps = argv[1], argv[2:]
    s = open_native_usb(port)
    s.timeout = 0.1
    time.sleep(0.3); s.reset_input_buffer()
    i = 0
    while i < len(steps):
        if steps[i] == "--wait":
            time.sleep(float(steps[i + 1])); i += 2; continue
        s.write((steps[i] + "\n").encode()); s.flush()
        deadline, got = time.time() + 2, False
        while time.time() < deadline:
            line = s.readline().decode("utf-8", "replace").strip()
            if line.startswith("{"):
                try: print(json.dumps({"cmd": steps[i], "reply": json.loads(line)}))
                except ValueError: print(json.dumps({"cmd": steps[i], "raw": line}))
                got = True
                break
        if not got: print(json.dumps({"cmd": steps[i], "error": "no reply"}))
        i += 1
    s.close()

if __name__ == "__main__":
    main(sys.argv)
