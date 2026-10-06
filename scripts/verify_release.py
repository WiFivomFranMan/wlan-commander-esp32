#!/usr/bin/env python3
"""Verify the exact experimental bundle, corresponding source and release signatures."""
import argparse
import hashlib
import json
import subprocess
import tarfile
import zipfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]

def digest(data):
    return hashlib.sha256(data).hexdigest()

def verify(assets):
    meta = json.loads((ROOT / 'experimental.json').read_text())
    assert meta['experimental'] and not meta['automatic_update']
    assert (assets / 'experimental.json').read_bytes() == (ROOT / 'experimental.json').read_bytes()
    assert (assets / '01-release-2026.pub').read_bytes() == (ROOT / 'keys/01-release-2026.pub').read_bytes()
    names = ['experimental.json']
    for item in [*meta['downloads'].values(), meta['application']]:
        path = assets / item['file']
        assert path.stat().st_size == item['bytes'], path.name
        assert digest(path.read_bytes()) == item['sha256'], path.name
        names.append(path.name)
    with zipfile.ZipFile(assets / meta['downloads']['image']['file']) as z:
        report = json.loads(z.read('combined-build-report.json'))
        for region in meta['regions']:
            data = z.read(region['file'])
            assert len(data) == region['bytes'] and digest(data) == region['sha256'], region['file']
        assert report['sha256']['wlc_sniffer_sdr.bin'] == meta['app_sha256']
        assert report['static_margin_bytes'] == meta['static_margin_bytes'] == 928
        assert digest(z.read('sdkconfig')) == report['sha256']['sdkconfig']
        for name, expected in report['sources']['variant_input_sha256'].items():
            assert digest((ROOT / 'firmware/wlc_sniffer_airhorn' / name).read_bytes()) == expected, name
        assert digest((ROOT / 'firmware/wlc_sniffer/wlc_sniffer.ino').read_bytes()) == report['sources']['input_sha256']['wlc_sniffer/wlc_sniffer.ino']
        with tarfile.open(assets / meta['downloads']['source']['file'], 'r:xz') as t:
            for name, expected in report['sources']['variant_input_sha256'].items():
                f = t.extractfile('firmware/wlc_sniffer_airhorn/' + name)
                assert f is not None and digest(f.read()) == expected, name
            f = t.extractfile('firmware/wlc_sniffer/wlc_sniffer.ino')
            assert f is not None and digest(f.read()) == report['sources']['input_sha256']['wlc_sniffer/wlc_sniffer.ino']
    for name in names:
        subprocess.run(['openssl', 'dgst', '-sha256', '-verify', str(ROOT / 'keys/01-release-2026.pub'),
                        '-signature', str(assets / (name + '.sig')), str(assets / name)], check=True)
        assert (assets / (name + '.sha256')).read_text().split()[0] == digest((assets / name).read_bytes())
    stable = json.loads((ROOT / 'ota.json').read_text())
    assert stable['sha'] == '1876915'
    assert digest((ROOT / 'firmware/wlc_sniffer.ino.bin').read_bytes()) == stable['sha256']
    assert json.loads((ROOT / 'manifest.json').read_text())['version'] == '2026.09.07+1876915'
    assert not (ROOT / '.github/workflows').exists(), 'GitHub Actions are forbidden'
    print('PASS: source and archive inputs, flash regions, SRAM report, signatures and retained stable firmware.')

if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--assets', required=True, type=Path)
    verify(parser.parse_args().assets.resolve())
