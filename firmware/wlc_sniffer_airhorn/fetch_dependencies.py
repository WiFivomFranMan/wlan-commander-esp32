#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Fetch pinned sources into a NEW directory. Never installs tools or flashes."""
import argparse
import json
import subprocess
from pathlib import Path

HERE = Path(__file__).resolve().parent
REPOS = {
    "idf": "https://github.com/espressif/esp-idf.git",
    "arduino": "https://github.com/espressif/arduino-esp32.git",
    "nimble": "https://github.com/h2zero/NimBLE-Arduino.git",
    "esp-sdr": "https://github.com/ESPARGOS/esp-sdr.git",
}


def fetch(output):
    # Refuse existing output rather than reset somebody else's checkout.
    output.mkdir(parents=True, exist_ok=False)
    lock = json.loads((HERE / "dependency-lock.json").read_text())
    for name, url in REPOS.items():
        path = output / name
        subprocess.run(["git", "init", str(path)], check=True)
        subprocess.run(["git", "-C", str(path), "remote", "add", "origin", url], check=True)
        subprocess.run(["git", "-C", str(path), "fetch", "--depth=1", "origin",
                        lock["commits"][name]], check=True)
        subprocess.run(["git", "-C", str(path), "checkout", "--detach", "FETCH_HEAD"], check=True)
        if name in ("idf", "esp-sdr"):
            subprocess.run(["git", "-C", str(path), "submodule", "update", "--init",
                            "--recursive", "--depth=1"], check=True)
    from prepare_sources import verify_dependencies
    verify_dependencies(output)
    print(f"Pinned dependencies verified at {output}")


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--output", required=True, type=Path)
    args = parser.parse_args()
    fetch(args.output.resolve())
