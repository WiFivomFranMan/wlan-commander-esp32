#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Exercise an ALREADY flashed development C5. Never flashes or opens BLE."""
import argparse
import json
import time
import zlib


def open_native_usb(name):
    import serial
    import termios

    class NativeUSB(serial.Serial):
        # On native USB, RTS is a reset command. Preserve the existing control
        # lines, including across close, instead of toggling them when opening.
        def _update_dtr_state(self):
            pass

        def _update_rts_state(self):
            pass

        def _reconfigure_port(self, *args, **kwargs):
            super()._reconfigure_port(*args, **kwargs)
            attrs = termios.tcgetattr(self.fd)
            attrs[2] &= ~termios.HUPCL
            termios.tcsetattr(self.fd, termios.TCSANOW, attrs)

    return NativeUSB(name, baudrate=115200, timeout=5, write_timeout=5)


def synchronize(port, timeout=10):
    """Bound startup noise without mistaking a reboot loop for a ready board."""
    old_timeout = port.timeout
    port.timeout = min(.25, timeout)
    output = []
    received = 0
    try:
        port.write(b"MODE?\n")
        deadline = time.monotonic() + timeout
        while time.monotonic() < deadline:
            raw = port.readline()
            if not raw:
                continue
            text = raw.decode("ascii", errors="replace").strip()
            if text == "MODE SNIFFER":
                return output
            if (text.startswith(("MODE ", "ERR ")) or
                any(value in text for value in ("assert failed:", "Guru Meditation", "waiting for download"))):
                raise RuntimeError(f"Board not ready: {text}")
            output.append(text)
            received += len(raw)
            if received > 16384:
                raise RuntimeError("Excessive startup output; board did not become ready")
        raise TimeoutError("No idle sniffer response during startup synchronization")
    finally:
        port.timeout = old_timeout


def exact(port, count):
    data = bytearray()
    while len(data) < count:
        part = port.read(count - len(data))
        if not part:
            raise TimeoutError(f"Short response: {len(data)}/{count}")
        data.extend(part)
    return bytes(data)


def command(port, text):
    port.write((text + "\n").encode("ascii"))
    answer = port.readline().decode("ascii").strip()
    if not answer:
        raise TimeoutError(f"No reply to {text}")
    return answer


def expect(port, text, answer):
    actual = command(port, text)
    if actual != answer:
        raise RuntimeError(f"{text}: expected {answer!r}, got {actual!r}")


def info(port):
    text = command(port, "WLCINFO?")
    if not text.startswith("WLCINFO "):
        raise RuntimeError(text)
    return json.loads(text[8:])


def check_idle(port):
    state = info(port)
    if state["mode"] != "sniffer" or state["run"] or state["ftm"] or not state["ble_initialized"]:
        raise RuntimeError(f"Idle sniffer restoration failed: {state}")
    return state


def capture(port, frequency, bins):
    expect(port, f"FREQ {frequency}", "OK")
    # Snapshot framing follows ESP-SDR's pinned tools/check_spectrum.py.
    header = command(port, f"SPEC 100 1 1 0 0 {bins}").split()
    if header != ["SPEC", str(bins), "80000000", str(bins), str(frequency)]:
        raise RuntimeError(f"Invalid spectrum start: {header}")
    frames, previous = 0, -1
    while True:
        magic = exact(port, 4)
        if magic == b"SPEC":
            end = (magic + port.readline()).decode("ascii").strip().split()
            if len(end) != 13 or end[0] != "SPECEND" or int(end[1]) != 0:
                raise RuntimeError(f"Capture failed: {end}")
            if not frames or int(end[8]) != frames:
                raise RuntimeError(f"Incomplete capture: {frames} frames, {end}")
            return {"frequency_mhz": frequency, "bins": bins, "frames": frames, "end": end}
        if magic != b"SPC1":
            raise RuntimeError(f"Invalid spectrum framing: {magic!r}")
        frame = magic + exact(port, bins + 28)
        if zlib.crc32(frame[:-4]) != int.from_bytes(frame[-4:], "little"):
            raise RuntimeError("Spectrum CRC mismatch")
        index = int.from_bytes(frame[8:16], "little")
        if index <= previous or 1 << frame[26] != bins or frame[27] != 2 or not frame[22] & 8:
            raise RuntimeError("Invalid snapshot shape, time or gap flag")
        if int.from_bytes(frame[4:8], "little") != frames:
            raise RuntimeError("Missing spectrum frame")
        frames += 1
        previous = index


def probe(port, cycles):
    startup = synchronize(port)
    result = {"initial": check_idle(port), "startup_output": startup,
              "cycles": [], "ble_over_air": "not_tested"}
    try:
        for _ in range(cycles):
            expect(port, "MODE SPECTRUM", "OK")
            expect(port, "MODE?", "MODE SPECTRUM")
            state = info(port)
            if state["ble_initialized"]:
                raise RuntimeError("BLE still initialized in spectrum mode")
            expect(port, "SPEC 0 1 1 0 0 256", "ERR spectrum_args")
            expect(port, "RXRUN 256 0 1000 20", "ERR spectrum_args")
            runs = [capture(port, frequency, bins) for frequency in (2412, 5180) for bins in (256, 512)]
            expect(port, "MODE SNIFFER", "OK")
            result["cycles"].append({"spectra": runs, "after": check_idle(port)})
        expect(port, "MODE SPECTRUM", "OK")
        time.sleep(6)  # No commands: exercise automatic lease expiry.
        expect(port, "MODE?", "MODE SNIFFER")
        result["lease_expiry"] = check_idle(port)
        return result
    finally:
        # A newline stops an in-progress bounded capture if a host check failed.
        # The lease expires even if this best-effort release cannot be delivered.
        port.write(b"\nRELEASE\n")


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--port", required=True)
    parser.add_argument("--cycles", type=int, default=10)
    args = parser.parse_args()
    if not 1 <= args.cycles <= 100:
        parser.error("--cycles must be between 1 and 100")
    port = open_native_usb(args.port)
    try:
        print(json.dumps(probe(port, args.cycles), indent=2))
    finally:
        port.close()
