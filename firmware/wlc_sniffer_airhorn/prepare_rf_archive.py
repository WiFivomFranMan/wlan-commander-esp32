#!/usr/bin/env python3
"""Relocate only the pinned C5 RF-test scratch BSS in a PRIVATE SDK copy."""
import argparse
import hashlib
import json
import subprocess
import tempfile
from pathlib import Path

ORIGINAL = "0f9680b41612762d854accf2334412e3e01b206a3ec290bbbb232842fdaec7ba"
BUFFERS = {
    "RX_ampdu_buff0_buf": 0x4000, "RX_ampdu_entry0_buf": 0x4b0,
    "RX_ampdu_entrysd0_buf": 0x190, "RX_aplenbk": 0x240,
    "RX_aplkbk": 0x6f0, "RX_bufflk": 0x300, "txbuffer": 0x5c00,
}

def prepare(archive, tool_prefix):
    original = hashlib.sha256(archive.read_bytes()).hexdigest()
    if original != ORIGINAL:
        raise ValueError("RF archive differs from pinned original; never patch an already patched SDK")
    with tempfile.TemporaryDirectory() as name:
        work = Path(name)
        subprocess.run([tool_prefix + "ar", "x", str(archive), "mac_common.o"], cwd=work, check=True)
        from elftools.elf.elffile import ELFFile
        with (work / "mac_common.o").open("rb") as f:
            elf = ELFFile(f)
            for section, size in BUFFERS.items():
                item = elf.get_section_by_name(".bss." + section)
                if item is None or item["sh_type"] != "SHT_NOBITS" or item["sh_size"] != size:
                    raise ValueError("Unexpected RF scratch section: " + section)
        command = [tool_prefix + "objcopy"]
        for section in BUFFERS:
            command += ["--rename-section", ".bss." + section + "=.wlc_rftest." + section]
        subprocess.run(command + ["mac_common.o"], cwd=work, check=True)
        subprocess.run([tool_prefix + "ar", "r", str(archive), "mac_common.o"], cwd=work, check=True)
    manifest = {"original_sha256": original, "patched_sha256": hashlib.sha256(archive.read_bytes()).hexdigest(),
                "relocated_scratch_bytes": sum(BUFFERS.values()), "sections": BUFFERS,
                "changes": "BSS section names only; code, symbols, relocations and sizes preserved"}
    archive.with_suffix(".airhorn.json").write_text(json.dumps(manifest, indent=2) + "\n")
    return manifest

if __name__ == "__main__":
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument("--archive", type=Path, required=True)
    p.add_argument("--tool-prefix", required=True)
    args = p.parse_args()
    print(json.dumps(prepare(args.archive.resolve(), args.tool_prefix), indent=2))
