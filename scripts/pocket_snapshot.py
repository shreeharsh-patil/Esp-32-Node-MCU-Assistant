"""Validate and snapshot an official build.py result before building another variant."""
import argparse
import json
from pathlib import Path
import shutil
from pocket_image_check import inspect

ROOT = Path(__file__).resolve().parents[1]
RELEASE = ROOT / 'pocket-release'


def snapshot(kind):
    description = json.loads((ROOT / 'build/project_description.json').read_text())
    if description.get('target') != 'esp32':
        raise SystemExit('Refusing a build for another microcontroller')
    config = (ROOT / 'sdkconfig').read_text(encoding='utf-8')
    diagnostic = 'CONFIG_POCKET_HARDWARE_DIAGNOSTICS=y' in config
    if diagnostic != (kind == 'diagnostics'):
        raise SystemExit('Diagnostic/conversation variant mismatch')
    if ('CONFIG_POCKET_CUSTOM_BACKEND=y' in config.splitlines()) != (kind == 'conversation'):
        raise SystemExit('Custom backend must be enabled only for conversation')
    for line in ['CONFIG_BOARD_TYPE_CUSTOM_ESP32_NODEMCU_POCKET_AI=y',
                 'CONFIG_ESPTOOLPY_FLASHSIZE_4MB=y', 'CONFIG_FLASH_NONE_ASSETS=y',
                 'CONFIG_WAKE_WORD_DISABLED=y', 'CONFIG_AUDIO_TEST_DURATION_MS=2000']:
        if line not in config.splitlines():
            raise SystemExit(f'Missing required configuration: {line}')
    if 'CONFIG_SPIRAM=y' in config or 'CONFIG_ALLOW_FIRMWARE_UPDATES=y' in config:
        raise SystemExit('PSRAM or firmware updates unexpectedly enabled')
    merged = ROOT / 'build/merged-binary.bin'
    report = inspect(merged)
    if not report['compatible_with_esp32_wroom32']:
        raise SystemExit('Merged image checksum/hash/chip validation failed')
    images = {image['offset']: image for image in report['images']}
    if set(images) != {'0x1000', '0x10000'}:
        raise SystemExit('Unexpected image layout')
    if images['0x10000']['project'] != 'xiaozhi' or not images['0x10000']['idf'].startswith('v6.1'):
        raise SystemExit('Unexpected application identity or SDK')
    if images['0x10000']['image_bytes'] > 0x3f0000 or report['bytes'] > 0x400000:
        raise SystemExit('Firmware exceeds 4 MB partition layout')
    table = (ROOT / 'build/partition_table/partition-table.bin').read_bytes()
    import struct
    entries = []
    for offset in range(0, len(table), 32):
        if table[offset:offset+2] != b'\xaa\x50':
            break
        _, category, subtype, address, size, label, _ = struct.unpack_from('<HBBII16sI', table, offset)
        entries.append((label.split(b'\0')[0].decode(), category, subtype, address, size))
    if entries != [('nvs', 1, 2, 0x9000, 0x6000), ('phy_init', 1, 1, 0xf000, 0x1000),
                   ('factory', 0, 0, 0x10000, 0x3f0000)]:
        raise SystemExit(f'Unexpected generated partition table: {entries}')
    RELEASE.mkdir(exist_ok=True)
    destination = RELEASE / kind
    destination.mkdir(exist_ok=True)
    name = 'pocket-xiaozhi-esp32' + ('-diagnostics' if diagnostic else '') + '.bin'
    shutil.copy2(merged, RELEASE / name)
    for source, target in [('build/xiaozhi.bin', 'xiaozhi.bin'),
                           ('build/bootloader/bootloader.bin', 'bootloader.bin'),
                           ('build/partition_table/partition-table.bin', 'partition-table.bin'),
                           ('build/xiaozhi.elf', 'xiaozhi.elf'),
                           ('build/xiaozhi.map', 'xiaozhi.map'),
                           ('build/flasher_args.json', 'flasher_args.json'),
                           ('sdkconfig', 'sdkconfig.txt'), ('dependencies.lock', 'dependencies.lock')]:
        if (ROOT / source).exists():
            shutil.copy2(ROOT / source, destination / target)
    report['file'] = name
    (destination / 'image-report.json').write_text(json.dumps(report, indent=2)+'\n', encoding='utf-8')
    import subprocess, sys
    subprocess.run([sys.executable, '-m', 'esp_idf_size', '--format', 'json2', '--output-file',
                    str(destination / 'size.json'), str(destination / 'xiaozhi.map')], check=True)
    print(f'{kind}: validated ESP32 boot/app hashes and partition bounds; {report["bytes"]} merged bytes')


if __name__ == '__main__':
    parser = argparse.ArgumentParser()
    parser.add_argument('kind', choices=['conversation', 'diagnostics'])
    snapshot(parser.parse_args().kind)
