#!/usr/bin/env python3
"""Verify touchscreen bundles, corresponding source, signatures and browser parts."""
import argparse
import hashlib
import json
from pathlib import Path
import subprocess
import tarfile
import zipfile

ROOT = Path(__file__).resolve().parents[1]

def sha(data):
    return hashlib.sha256(data).hexdigest()

def verify(assets, public=None):
    meta = json.loads((ROOT / 'touchscreen.json').read_text())
    assert (assets / 'touchscreen.json').read_bytes() == (ROOT / 'touchscreen.json').read_bytes()
    assert (assets / '01-release-2026.pub').read_bytes() == (ROOT / 'keys/01-release-2026.pub').read_bytes()
    assert len(meta['models']) == 2
    for model in meta['models']:
        item = model['download']
        image = assets / item['file']
        assert image.stat().st_size == item['bytes'] and sha(image.read_bytes()) == item['sha256']
        assert model['chip'] == 'ESP32-C5' and len(model['regions']) == 5
        with zipfile.ZipFile(image) as archive:
            end = 0
            for region in sorted(model['regions'], key=lambda r:r['offset']):
                data = archive.read(region['file'])
                assert len(data) == region['bytes'] and sha(data) == region['sha256']
                assert region['offset'] >= end
                end = region['offset'] + len(data)
            assert end <= model['physical_flash_bytes']
            assert archive.read('wlc_sensor.bin')[0x30:0x50].rstrip(b'\0') == b'touch-airhorn-v4'
            assert archive.read('wlc_sensor.bin')[0x50:0x70].rstrip(b'\0') == b'wlc_sensor'
            if public:
                short = '2.8' if model['physical_flash_bytes'] == 33554432 else '1.69'
                browser = json.loads((public / f'sensors/esp32-c5/flash/{short}.json').read_text())
                assert browser['version'] == model['version']
                assert browser['flash_sizes'] == [model['physical_flash_bytes']]
                for part, region in zip(browser['builds'][0]['parts'],model['regions'],strict=True):
                    data = (public / part['path'].lstrip('/')).read_bytes()
                    assert data == archive.read(region['file'])
                    assert part['offset'] == region['offset'] and part['sha256'] == sha(data)
                    assert part['bytes'] == len(data)
    item = meta['source']; source = assets / item['file']
    assert source.stat().st_size == item['bytes'] and sha(source.read_bytes()) == item['sha256']
    inputs = json.loads((assets / 'touch-source-inputs.json').read_text())
    with tarfile.open(source, 'r:xz') as archive:
        for name, expected in inputs.items():
            assert sha((ROOT / 'firmware/c5_touch' / name).read_bytes()) == expected, name
            assert sha(archive.extractfile('c5-touch-source/' + name).read()) == expected, name
        names = archive.getnames()
        assert not any('/evidence/' in n or '/artifacts/' in n or '/.git/' in n or 'signing-key' in n for n in names)
        for size in ['2_8','1_69']:
            for config in ['sdkconfig','sensor-sdkconfig']:
                assert archive.extractfile(f'c5-touch-source/release-config/{size}/{config}')
    for path in assets.iterdir():
        if path.suffix in ['.zip','.xz','.json','.bin']:
            assert (assets / (path.name+'.sha256')).read_text().split()[0] == sha(path.read_bytes())
            subprocess.run(['openssl','dgst','-sha256','-verify',str(ROOT/'keys/01-release-2026.pub'),'-signature',str(path)+'.sig',str(path)],check=True,stdout=subprocess.DEVNULL)
    assert not (ROOT / '.github/workflows').exists(), 'GitHub Actions are forbidden'
    print('PASS: touchscreen image parts, app identity, corresponding source, signatures and browser bytes.')

if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--assets', type=Path, required=True)
    parser.add_argument('--public', type=Path)
    args = parser.parse_args()
    verify(args.assets.resolve(), args.public.resolve() if args.public else None)
