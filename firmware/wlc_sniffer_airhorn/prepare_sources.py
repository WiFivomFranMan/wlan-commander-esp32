#!/usr/bin/env python3
"""Generate the GPL combined variant without modifying either source tree."""
import argparse
import hashlib
import json
import os
from pathlib import Path

HERE = Path(__file__).resolve().parent
UPSTREAM = "550fadea4d00a9e26ce921c5832167becb3dc20c"


def verify_dependencies(deps):
    lock = json.loads((HERE / "dependency-lock.json").read_text())
    for relative, expected in lock["sha256"].items():
        path = deps / relative
        if not path.is_file() or hashlib.sha256(path.read_bytes()).hexdigest() != expected:
            raise ValueError(f"Dependency differs from pinned source: {relative}")
    return lock


def replace_once(source, before, after):
    if source.count(before) != 1:
        raise ValueError(f"Source integration point changed: {before[:90]}")
    return source.replace(before, after, 1)


def native_nimble_device(source):
    # A failed SDK host/controller init must not launch a task with a null
    # event queue. NimBLE-Arduino 2.5.1 otherwise ignores this return value.
    return replace_once(source, "        nimble_port_init();",
                        "        if (nimble_port_init() != ESP_OK) return false;")


def generate(deps, output):
    lock = verify_dependencies(deps)
    rf_archive = Path(os.environ["IDF_PATH"]) / "components/esp_phy/lib/esp32c5/librftest.a"
    if hashlib.sha256(rf_archive.read_bytes()).hexdigest() != "618682a7bd6a926bf6bb9534fc2aa331c90041b51e3b63cc507245b06970dff4":
        raise ValueError("Use the independently copied SDK with the verified RF scratch relocation")
    output.mkdir(parents=True, exist_ok=True)
    original = HERE.parent / "wlc_sniffer/wlc_sniffer.ino"
    sniffer = original.read_text()
    sniffer = replace_once(sniffer, "static uint8_t pool[POOL_BYTES];", "static uint8_t* pool = nullptr;")
    sniffer = replace_once(sniffer, "  const uint32_t n = (uint32_t)(rtlen + flen);", "  if (!pool) return false;\n  const uint32_t n = (uint32_t)(rtlen + flen);")
    sniffer = replace_once(sniffer, "void setup() {", "static void sniffer_setup() {")
    sniffer = replace_once(sniffer, "void loop() {", "static void sniffer_loop() {")
    start = sniffer.index('  NimBLEDevice::init("WLC-C5-Sniffer");')
    end = sniffer.index("\n  WiFi.mode(WIFI_MODE_STA);", start)
    ble_body = replace_once(sniffer[start:end],
        '  NimBLEDevice::init("WLC-C5-Sniffer");',
        '  if (!NimBLEDevice::init("WLC-C5-Sniffer")) return false;')
    # The normal sketch never deletes its server. Exclusive mode switching does;
    # keep ownership of its static callback instead of letting NimBLE delete it.
    ble_body = replace_once(ble_body, "srv->setCallbacks(&srvcb);",
                           "srv->setCallbacks(&srvcb, false);")
    ble_body = replace_once(ble_body, "  NimBLEDevice::startAdvertising();",
        "  return NimBLEDevice::startAdvertising();")
    ble_setup = "static bool ble_setup() {\n" + ble_body + "\n}\n"
    sniffer = sniffer[:start] + "  ble_setup();\n" + sniffer[end:]
    sniffer = sniffer.replace("Serial.", "quiet_serial.")
    sniffer = replace_once(sniffer, "    NimBLEDevice::startAdvertising();",
        "    if (!radio_gate.load()) NimBLEDevice::startAdvertising();")
    sniffer = replace_once(sniffer, "    NimBLEAttValue v = c->getValue();",
        "    if (radio_gate.load()) return;\n    NimBLEAttValue v = c->getValue();")
    sniffer = replace_once(sniffer, "static void sniffer_setup() {", ble_setup + "\nstatic void sniffer_setup() {")
    (output / "sniffer.inc").write_text(sniffer)
    source = deps / "esp-sdr/main/families/c5_c6_c61/receiver.c"
    receiver = source.read_text()
    start = receiver.index("void app_main(void) {")
    end = receiver.index("\nstatic void handle_command(char *line) {", start)
    receiver = receiver[:start] + """
void wlc_sdr_reset(void) {
    rx_ready=false; frequency_mhz=2412; rx_channel_mode=0;
    rx_filter=-1; rx_analog_filter=-1; gain_mode=GAIN_HARDWARE; gain_code=40;
}
void wlc_sdr_dispatch(char *line) { handle_command(line); }
""" + receiver[end:]
    (output / "receiver.c").write_text(receiver)
    spectrum = (deps / "esp-sdr/main/common/spectrum.c").read_text()
    spectrum = replace_once(spectrum, "#define MAX_FFT 2048u", "#define MAX_FFT 512u")
    spectrum = replace_once(spectrum, '\\"USB\\",\\"UART\\"', '\\"USB\\"')
    (output / "spectrum.c").write_text(spectrum)
    device = deps / "nimble/src/NimBLEDevice.cpp"
    (output / "nimble_device.cpp").write_text(native_nimble_device(device.read_text()))
    hashes = {}
    for path in (original, source, deps / "esp-sdr/main/common/spectrum.c", device):
        relative = "wlc_sniffer/wlc_sniffer.ino" if path == original else str(path.relative_to(deps))
        hashes[relative] = hashlib.sha256(path.read_bytes()).hexdigest()
    (output / "source-manifest.json").write_text(json.dumps({
        "esp_sdr_commit": UPSTREAM, "input_sha256": hashes,
        "variant": "GPL-3.0-or-later", "fft_max_bins": 512,
        "dependency_commits": lock["commits"],
        "dependency_lock_sha256": hashlib.sha256((HERE / "dependency-lock.json").read_bytes()).hexdigest(),
        "variant_input_sha256": {name: hashlib.sha256((HERE / name).read_bytes()).hexdigest()
            for name in ("CMakeLists.txt", "main/CMakeLists.txt", "main/combined.cpp",
                "main/command_policy.h", "main/generator_policy.h", "main/rf_buffers.ld", "prepare_rf_archive.py", "components/nimble_cpp/CMakeLists.txt",
                "components/nimble_cpp/sdk_host.h",
                "prepare_sources.py", "sdkconfig.defaults", "partitions.csv")},
    }, indent=2) + "\n")


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--deps", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    generate(args.deps.resolve(), args.output.resolve())
