#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Inspect a linked combined ELF, its actual heap reservation, and build config."""
import argparse
import hashlib
import json
import re
import struct
from pathlib import Path

BANK_START, BANK_END = 0x40820000, 0x40840000


def check_values(symbols, reservation, config, binary_size):
    for name in ("_iram_end", "_data_end", "_bss_end"):
        if name not in symbols or not 0x40800000 <= symbols[name] <= BANK_START:
            raise ValueError(f"Unsafe or missing ELF symbol: {name}")
    if not symbols.get("adctrig"):
        raise ValueError("Required PHY capture function is not defined")
    if reservation != (BANK_START, BANK_END):
        raise ValueError("RF SRAM bank is not excluded from the heap")
    for option in ("ESP_PHY_ENABLE_CERT_TEST", "ESP_PHY_DEBUG", "BT_CTRL_RUN_IN_FLASH_ONLY",
                   "ESP_CONSOLE_NONE", "ESP_CONSOLE_SECONDARY_NONE"):
        enabled = re.findall(rf"^CONFIG_{option}=y$", config, re.M)
        disabled = re.search(rf"^# CONFIG_{option} is not set$", config, re.M)
        if len(enabled) != 1 or disabled:
            raise ValueError(f"Missing required SDK option: {option}")
    # The identified development board's generic flash driver rejects suspend
    # during SDK startup. Keep this profile usable without forcing chip capabilities.
    if (re.search(r"^CONFIG_SPI_FLASH_AUTO_SUSPEND=", config, re.M) or
        len(re.findall(r"^# CONFIG_SPI_FLASH_AUTO_SUSPEND is not set$", config, re.M)) != 1):
        raise ValueError("Flash auto suspend must be disabled for this profile")
    if not 0 < binary_size <= 0x1e0000:
        raise ValueError("Combined image does not fit the existing app slot")
    return {"static_section_ends": {k: hex(symbols[k]) for k in
            ("_iram_end", "_data_end", "_bss_end")},
            "static_margin_bytes": BANK_START - max(symbols[k] for k in
            ("_iram_end", "_data_end", "_bss_end")),
            "adctrig": hex(symbols["adctrig"]),
            "heap_excludes": [hex(x) for x in reservation], "binary_bytes": binary_size,
            "hardware_tests": "not_run"}


def inspect(build, config):
    from elftools.elf.elffile import ELFFile
    elf_path = build / "wlc_sniffer_sdr.elf"
    binary = build / "wlc_sniffer_sdr.bin"
    with elf_path.open("rb") as file:
        elf = ELFFile(file)
        if elf.elfclass != 32 or not elf.little_endian or elf["e_machine"] != "EM_RISCV":
            raise ValueError("Expected ESP32-C5 RISC-V ELF")
        symtab = elf.get_section_by_name(".symtab")
        def symbol(name):
            found = symtab.get_symbol_by_name(name) or []
            if len(found) != 1 or found[0]["st_shndx"] == "SHN_UNDEF":
                raise ValueError(f"Missing/ambiguous defined symbol: {name}")
            return found[0]
        scratch = elf.get_section_by_name(".wlc_rftest")
        if (scratch is None or scratch["sh_type"] != "SHT_NOBITS" or
            scratch["sh_addr"] != BANK_START or scratch["sh_size"] != 44656):
            raise ValueError("Pinned RF-test scratch is not in the shared lower bank")
        symbols = {name: symbol(name)["st_value"] for name in
                   ("_iram_end", "_data_end", "_bss_end", "adctrig")}
        reserved = symbol("reserved_region_c5_rf_dump")
        if reserved["st_size"] != 8:
            raise ValueError("Unexpected RF heap reservation record")
        section = elf.get_section(reserved["st_shndx"])
        offset = reserved["st_value"] - section["sh_addr"]
        reservation = struct.unpack("<II", section.data()[offset:offset+8])
    report = check_values(symbols, reservation, config.read_text(), binary.stat().st_size)
    report["rf_test_scratch"] = {"address": hex(BANK_START), "bytes": 44656, "exclusive": True}
    report["sha256"] = {path.name: hashlib.sha256(path.read_bytes()).hexdigest()
                        for path in (elf_path, binary, config)}
    report["sources"] = json.loads((build / "esp-idf/main/generated/source-manifest.json").read_text())
    for name, expected in report["sources"]["variant_input_sha256"].items():
        actual = hashlib.sha256((Path(__file__).resolve().parent / name).read_bytes()).hexdigest()
        if actual != expected:
            raise ValueError(f"Variant input changed after source generation: {name}")
    return report


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--build", required=True, type=Path)
    parser.add_argument("--config", required=True, type=Path)
    args = parser.parse_args()
    result = inspect(args.build.resolve(), args.config.resolve())
    (args.build / "combined-build-report.json").write_text(json.dumps(result, indent=2) + "\n")
    print(json.dumps(result, indent=2))
