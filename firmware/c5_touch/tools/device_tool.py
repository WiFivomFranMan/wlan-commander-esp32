#!/usr/bin/env python3
"""Mac-only, identity-checked backup/flash/restore helper for the Waveshare ESP32-C5 touch boards.

Derived from wlc-c5-lcd-poc/tools/lcd-controller/device_tool.py. The board is chosen by USB
serial (its Wi-Fi MAC), never by tty number, and the ROM MAC must agree before anything runs.
"""
import argparse
import datetime
import hashlib
import json
from pathlib import Path
import plistlib
import re
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[2]

# Flash capacity identifies the board model; anything else is refused.
BOARDS = {32 * 1024 * 1024: "ESP32-C5-Touch-LCD-2.8", 16 * 1024 * 1024: "ESP32-C5-Touch-LCD-1.69"}

def digest(path): return hashlib.sha256(Path(path).read_bytes()).hexdigest()

def inventory():
    if sys.platform != "darwin": raise RuntimeError("Use this helper on the Mac connected to the development board")
    tree = plistlib.loads(subprocess.check_output(["ioreg", "-r", "-c", "IOUSBHostDevice", "-a", "-l"]))
    result = {}
    def walk(node, board=None):
        if node.get("idVendor") == 0x303a and node.get("USB Serial Number"):
            board = {"serial": node.get("USB Serial Number"), "product": node.get("USB Product Name"), "location": node.get("locationID")}
        if board and node.get("IOCalloutDevice"):
            item = dict(board, port=node["IOCalloutDevice"])
            result[(item["serial"], item["port"])] = item
        for child in node.get("IORegistryEntryChildren", []): walk(child, board)
    for node in tree: walk(node)
    return list(result.values())

def target(serial):
    matches = [item for item in inventory() if str(item.get("serial", "")).lower() == serial.lower()]
    if len(matches) != 1: raise RuntimeError(f"Expected exactly one native USB port for {serial}; found {matches}")
    port = matches[0]["port"]
    busy = subprocess.run(["lsof", "-t", port], capture_output=True, text=True)
    if busy.stdout.strip(): raise RuntimeError("Target port is open in another process; do not interrupt it")
    return port

def esptool(port, before, after, *command):
    args = [sys.executable, "-m", "esptool", "--chip", "esp32c5", "--port", port,
            "--baud", "115200", "--before", before, "--after", after, *map(str, command)]
    result = subprocess.run(args, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
    print(result.stdout, end="", flush=True)
    if result.returncode: raise RuntimeError("esptool failed; stop and inspect the board before retrying")
    return result.stdout

def chip(port, serial, before):
    output = esptool(port, before, "no-reset", "flash-id")
    addresses = re.findall(r"(?im)^\s*(?:MAC|Base MAC):\s*([0-9a-f:]{17})", output)
    if serial.lower() not in [value.lower() for value in addresses]:
        raise RuntimeError("ROM MAC does not match the selected USB identity")
    size = re.search(r"Detected flash size:\s*(\d+)MB", output)
    if not size: raise RuntimeError("Could not determine complete flash size")
    size = int(size[1]) * 1024 * 1024
    if size not in BOARDS: raise RuntimeError(f"Unexpected flash size {size}; not one of the touch boards")
    print(f"Board model by flash size: {BOARDS[size]}", flush=True)
    return size

def safe_backup_path(path):
    path = Path(path).expanduser().resolve()
    if path == Path("/tmp").resolve() or Path("/tmp").resolve() in path.parents:
        raise RuntimeError("Backups must be persistent, outside /tmp")
    return path

def validate_backup(path, serial):
    path = safe_backup_path(path)
    meta = json.loads((path / "backup.json").read_text())
    if meta.get("serial", "").lower() != serial.lower(): raise RuntimeError("Backup belongs to another board")
    first, second = path / "original-1.bin", path / "original-2.bin"
    if first.stat().st_size != meta["flash_bytes"] or second.stat().st_size != meta["flash_bytes"]:
        raise RuntimeError("Backup is incomplete")
    if digest(first) != meta["sha256"] or digest(second) != meta["sha256"]:
        raise RuntimeError("Both independent backups must match the verified hash")
    return meta, first

def artifacts(image_dir):
    """An ESP-IDF build output directory: flasher_args.json plus the files it names."""
    base = Path(image_dir).resolve()
    settings = json.loads((base / "flasher_args.json").read_text())
    size_arg = settings["write_flash_args"][settings["write_flash_args"].index("--flash_size") + 1]
    settings["flash_bytes"] = int(size_arg.rstrip("MB")) * 1024 * 1024
    files = {}
    for offset, name in settings["flash_files"].items():
        path = (base / name).resolve()
        if not path.is_relative_to(base): raise RuntimeError(f"Flash file escapes the image directory: {name}")
        files[offset] = path
    print(json.dumps({off: [str(f.relative_to(base)), digest(f)] for off, f in files.items()}, indent=2))
    return files, settings

def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument("action", choices=["inventory", "identify", "backup", "flash", "restore"])
    p.add_argument("--board", help="Exact USB serial/MAC from inventory")
    p.add_argument("--backup-dir", help="Persistent directory for this board's two full backups")
    p.add_argument("--image", help="ESP-IDF build output directory to flash (flash action)")
    p.add_argument("--slot", choices=["ota_0", "ota_1"], help="Flash only the image's app binary into this slot, "
                   "leaving bootloader, partition table, otadata, NVS and the other app slot untouched")
    p.add_argument("--before", choices=["usb-reset", "default-reset", "no-reset"], default="usb-reset",
                   help="Use no-reset after manually entering download mode")
    args = p.parse_args()
    if args.action == "inventory": print(json.dumps(inventory(), indent=2)); return 0
    if args.action == "identify":
        if not args.board: p.error("--board is required")
        port = target(args.board)
        size = chip(port, args.board, args.before)
        esptool(port, "no-reset", "hard-reset", "chip-id")
        print(json.dumps({"serial": args.board.lower(), "flash_bytes": size, "model": BOARDS[size]})); return 0
    if not args.board or not args.backup_dir: p.error("--board and --backup-dir are required")
    backup = safe_backup_path(args.backup_dir)
    if args.action == "backup":
        if backup.exists(): raise RuntimeError("Use a new backup directory; existing evidence is preserved")
    else:
        meta, original = validate_backup(backup, args.board)
    if args.action == "flash":
        if not args.image: p.error("--image is required for flash")
        files, settings = artifacts(args.image)
        if args.slot:
            # Both images share one partition table; ota_1 starts at 0x720000 (firmware/*/partitions.csv).
            files = {"0x720000" if args.slot == "ota_1" else "0x20000": files["0x20000"]}
    port = target(args.board)
    size = chip(port, args.board, args.before)
    if args.action == "backup":
        backup.mkdir(parents=True)
        for name in ("original-1.bin", "original-2.bin"):
            esptool(port, "no-reset", "no-reset", "read-flash", "0", str(size), backup / name)
        first, second = backup / "original-1.bin", backup / "original-2.bin"
        if first.stat().st_size != size or second.stat().st_size != size or digest(first) != digest(second):
            raise RuntimeError("Full flash readbacks differ; no flash is permitted")
        record = {"serial": args.board.lower(), "flash_bytes": size, "sha256": digest(first),
                  "recorded_utc": datetime.datetime.now(datetime.timezone.utc).isoformat()}
        record["model"] = BOARDS[size]
        (backup / "backup.json").write_text(json.dumps(record, indent=2) + "\n")
        esptool(port, "no-reset", "hard-reset", "chip-id")  # return the board to its own firmware
        print(json.dumps(record, indent=2)); return 0
    if size != meta["flash_bytes"]: raise RuntimeError("Current flash capacity differs from the backup")
    if args.action == "flash":
        if settings.get("flash_bytes") != size: raise RuntimeError("Build targets a different flash size than this board")
        pairs = [str(v) for off, f in files.items() for v in (off, f)]
        options = [value.replace("_", "-") if value.startswith("--") else value for value in settings["write_flash_args"]]
        esptool(port, "no-reset", "no-reset", "write-flash", *options, *pairs)
        esptool(port, "no-reset", "watchdog-reset", "verify-flash", *pairs)
    else:
        esptool(port, "no-reset", "no-reset", "write-flash", "--flash-mode", "keep", "--flash-freq", "keep", "--flash-size", "keep", "0", original)
        readback = backup / ("restored-" + datetime.datetime.now().strftime("%Y%m%d-%H%M%S") + ".bin")
        esptool(port, "no-reset", "watchdog-reset", "read-flash", "0", str(size), readback)
        if digest(readback) != meta["sha256"]: raise RuntimeError("Full restored image differs from original")
        print("Full restoration hash verified")
    return 0

if __name__ == "__main__":
    try: raise SystemExit(main())
    except (RuntimeError, OSError, ValueError, KeyError) as exc:
        print(f"ERROR: {exc}", file=sys.stderr); raise SystemExit(1)
