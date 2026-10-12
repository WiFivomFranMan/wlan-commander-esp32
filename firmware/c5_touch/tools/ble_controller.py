#!/usr/bin/env python3
"""Explicitly targeted LCD PoC tester. Never flashes, resets, or starts RF TX."""
import argparse
import asyncio
import json
from pathlib import Path
import sys
import time
import subprocess

SERVICE = "57574c43-0001-4a50-8000-0000c5000001"
FRAME = SERVICE.replace("0001-4a50", "0002-4a50")
STATUS = SERVICE.replace("0001-4a50", "0003-4a50")
COMMAND = SERVICE.replace("0001-4a50", "0004-4a50")
DISPLAY = SERVICE.replace("0001-4a50", "0006-4a50")

def bounded(value, minimum, maximum):
    n = int(value)
    if not minimum <= n <= maximum:
        raise argparse.ArgumentTypeError(f"Must be {minimum}..{maximum}")
    return n

def channel_list(value):
    try:
        channels = [int(v) for v in value.split(",")]
    except ValueError as exc:
        raise argparse.ArgumentTypeError("Use comma-separated channel numbers") from exc
    if not 1 <= len(channels) <= 40 or any(not (1 <= n <= 14 or 36 <= n <= 177) for n in channels):
        raise argparse.ArgumentTypeError("Use 1–40 supported 2.4/5 GHz channels")
    return channels

def validate_display(info, board):
    if info.get("version") != 1 or not str(info.get("firmware", "")).startswith("wlc-sensor-") or not info.get("identify"):
        raise RuntimeError("Device does not advertise the touch sensor contract")
    if str(info.get("board", "")).lower() != board.lower():
        raise RuntimeError(f"Wrong board: expected {board}, received {info.get('board')}")
    if not info.get("display"):
        raise RuntimeError("LCD initialization failed; radio firmware may still operate")
    return info

class Reporter:
    def __init__(self, filename):
        self.file = Path(filename).open("a") if filename else None
    def emit(self, kind, **values):
        record = {"at_unix": time.time(), "kind": kind, **values}
        text = json.dumps(record)
        print(text, flush=True)
        if self.file:
            self.file.write(text + "\n"); self.file.flush()
    def close(self):
        if self.file: self.file.close()

async def run_ble(args, reporter):
    from bleak import BleakClient, BleakScanner
    if args.action == "scan":
        for device, adv in (await BleakScanner.discover(timeout=5, return_adv=True)).values():
            if SERVICE in [u.lower() for u in adv.service_uuids]:
                reporter.emit("candidate", address=device.address, name=device.name)
        return
    if not args.address or not args.board:
        raise RuntimeError("BLE operations require --address and --board (USB/Wi-Fi MAC)")
    async with BleakClient(args.address, timeout=15) as client:
        if not client.services.get_characteristic(DISPLAY):
            raise RuntimeError("Selected device has no LCD PoC capability characteristic")
        async def info():
            value = json.loads(bytes(await client.read_gatt_char(DISPLAY)))
            return validate_display(value, args.board)
        reporter.emit("display", **await info())
        if args.action == "inspect": return
        if args.action == "identify":
            await client.write_gatt_char(COMMAND, bytes([9, args.number, args.duration]), response=True)
            await asyncio.sleep(.3)
            after = await info()
            if after.get("identify_number") != args.number:
                raise RuntimeError("Identify was not applied")
            reporter.emit("identify_applied", **after)
            await asyncio.sleep(args.duration + .2)
            after = await info()
            if after.get("identify_number") != 0:
                raise RuntimeError("Identify did not expire")
            reporter.emit("identify_expired", **after)
            return
        statuses = asyncio.Queue()
        notifications, total_bytes = 0, 0
        def frame(_characteristic, data):
            nonlocal notifications, total_bytes
            notifications += 1; total_bytes += len(data)
        def status(_characteristic, data):
            text = bytes(data).decode("ascii", errors="replace")
            fields = dict(item.split("=", 1) for item in text.split() if "=" in item)
            statuses.put_nowait(fields)
            reporter.emit("status", fields=fields)
        await client.start_notify(FRAME, frame)  # Existing firmware gates status on this subscription.
        await client.start_notify(STATUS, status)
        owned = False
        try:
            initial = await asyncio.wait_for(statuses.get(), 5)
            if args.action == "status":
                await asyncio.sleep(args.seconds)
                return
            if initial.get("run") != "0":
                raise RuntimeError("Board is already capturing; tester will not take ownership")
            await client.write_gatt_char(COMMAND, bytes([1, *args.channels]), response=True)
            await client.write_gatt_char(COMMAND, bytes([6, args.width]), response=True)
            await client.write_gatt_char(COMMAND, bytes([5, 3, 0, 0, 0]), response=True)
            # Passive management/control capture only. Disable any previously armed ANQP.
            await client.write_gatt_char(COMMAND, bytes([3, 0]), response=True)
            owned = True  # Attempted start means cleanup is required even on an uncertain write.
            await client.write_gatt_char(COMMAND, bytes([4, 1]), response=True)
            await asyncio.sleep(args.seconds)
            if not client.is_connected:
                raise RuntimeError("BLE disconnected; capture stop cannot be confirmed")
            reporter.emit("stream", notifications=notifications, bytes=total_bytes)
        finally:
            if owned and client.is_connected:
                while not statuses.empty(): statuses.get_nowait()
                await client.write_gatt_char(COMMAND, bytes([4, 0]), response=True)
                end = time.monotonic() + 6
                while True:
                    fields = await asyncio.wait_for(statuses.get(), max(.1, end - time.monotonic()))
                    if fields.get("run") == "0":
                        reporter.emit("stop_confirmed", fields=fields); break
                    if time.monotonic() >= end: raise RuntimeError("Stop was not confirmed")

def run_usb(args, reporter):
    if args.action not in ("inspect", "identify"):
        raise RuntimeError("USB tester supports inspect/identify; use BLE for capture/status")
    if not args.board: raise RuntimeError("USB operations require --board")
    busy = subprocess.run(["lsof", "-t", args.port], capture_output=True, text=True)
    if busy.stdout.strip(): raise RuntimeError("USB port is open in another process; do not interrupt it")
    import importlib.util
    probe_path = Path(__file__).resolve().parents[2] / "firmware/wlc_sniffer_airhorn/probe_usb.py"
    spec = importlib.util.spec_from_file_location("usb_probe", probe_path)
    probe = importlib.util.module_from_spec(spec); spec.loader.exec_module(probe)
    # Run only on the explicitly identified, otherwise unused development port.
    with probe.open_native_usb(args.port) as port:
        probe.synchronize(port)
        state = probe.info(port)
        if state.get("board", "").lower() != args.board.lower(): raise RuntimeError("Wrong USB board")
        def info():
            text = probe.command(port, "LCDINFO?")
            if not text.startswith("LCDINFO "): raise RuntimeError(text)
            return validate_display(json.loads(text[8:]), args.board)
        reporter.emit("display", **info())
        if args.action == "inspect": return
        probe.expect(port, f"IDENTIFY {args.number} {args.duration}", "OK")
        time.sleep(.3)
        after = info()
        if after.get("identify_number") != args.number: raise RuntimeError("Identify was not applied")
        reporter.emit("identify_applied", **after)
        time.sleep(args.duration + .2)
        after = info()
        if after.get("identify_number") != 0: raise RuntimeError("Identify did not expire")
        reporter.emit("identify_expired", **after)

def parser():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument("--address", help="Explicit BLE address/UUID from scan")
    p.add_argument("--port", help="Explicit native USB port, exclusively available to this tester")
    p.add_argument("--board", help="Expected board USB/Wi-Fi MAC identity")
    p.add_argument("--log", help="Append JSONL evidence")
    sub = p.add_subparsers(dest="action", required=True)
    sub.add_parser("scan"); sub.add_parser("inspect")
    identify = sub.add_parser("identify")
    identify.add_argument("number", type=lambda x: bounded(x, 1, 99))
    identify.add_argument("--duration", type=lambda x: bounded(x, 1, 15), default=5)
    for action in ("capture", "status"):
        command = sub.add_parser(action)
        command.add_argument("--seconds", type=lambda x: bounded(x, 1, 600), default=15)
        if action == "capture":
            command.add_argument("--channels", type=channel_list, default=[1, 36])
            command.add_argument("--width", type=int, choices=[0, 1, 2], default=0)
    return p

def main():
    args = parser().parse_args(); reporter = Reporter(args.log)
    try:
        if args.port: run_usb(args, reporter)
        else: asyncio.run(run_ble(args, reporter))
        return 0
    except Exception as exc:
        reporter.emit("error", message=str(exc)); return 1
    except KeyboardInterrupt:
        reporter.emit("interrupted"); return 130
    finally: reporter.close()

if __name__ == "__main__": raise SystemExit(main())
