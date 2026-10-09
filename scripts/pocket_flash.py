"""Explicit COM-port flashing with checksum, ESP32 chip, and >=4MB flash checks."""
import argparse
import hashlib
import importlib.util
from pathlib import Path
import re
import subprocess
import sys
from pocket_image_check import inspect

ROOT = Path(__file__).resolve().parents[1]


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--port', help='Explicit Windows COM port or serial path')
    parser.add_argument('--baud', type=int, choices=[115200, 230400, 460800, 921600], default=115200,
                        help='Serial speed; defaults to the verified 115200 baud')
    parser.add_argument('--diagnostics', action='store_true')
    parser.add_argument('--erase-all', action='store_true', help='Erase all old firmware/settings')
    parser.add_argument('--verify-only', action='store_true', help='No serial access or writes')
    args = parser.parse_args()
    name = 'pocket-xiaozhi-esp32' + ('-diagnostics' if args.diagnostics else '') + '.bin'
    image = ROOT / 'pocket-release' / name
    report = inspect(image)
    if not report['compatible_with_esp32_wroom32'] or report['bytes'] > 0x400000:
        raise SystemExit('Refusing incompatible/unverified image')
    lines = (ROOT / 'pocket-release/SHA256SUMS.txt').read_text().splitlines()
    hashes = {filename: digest for digest, filename in (line.split('  ', 1) for line in lines)}
    if hashlib.sha256(image.read_bytes()).hexdigest() != hashes.get(name):
        raise SystemExit('Release checksum mismatch')
    kind = 'diagnostics' if args.diagnostics else 'conversation'
    parts = [(0x1000, 'bootloader.bin'), (0x8000, 'partition-table.bin'),
             (0x10000, 'xiaozhi.bin')]
    for offset, filename in parts:
        path = ROOT / 'pocket-release' / kind / filename
        if hashlib.sha256(path.read_bytes()).hexdigest() != hashes.get(f'{kind}/{filename}'):
            raise SystemExit(f'Release checksum mismatch: {filename}')
        if filename != 'partition-table.bin' and not inspect(path)['compatible_with_esp32_wroom32']:
            raise SystemExit(f'Incompatible component: {filename}')
    print(f'Validated {name}: classic ESP32, boot/table/app checksums match')
    if args.verify_only:
        return
    if not args.port:
        parser.error('--port is required for chip probing/flashing')
    if importlib.util.find_spec('esptool') is None:
        raise SystemExit('Activate the ESP-IDF 6.1 environment first (esptool is required).')
    base = [sys.executable, '-m', 'esptool', '--chip', 'esp32', '--port', args.port,
            '--baud', str(args.baud)]
    probe = subprocess.run([*base, 'flash-id'], capture_output=True, text=True, check=True)
    print(probe.stdout)
    match = re.search(r'Detected flash size:\s*(\d+)\s*MB', probe.stdout, re.I)
    if not match or int(match.group(1)) < 4:
        raise SystemExit('Detected flash capacity is missing or smaller than 4MB; refusing write')
    command = [*base, 'write-flash', '--flash-mode', 'dio', '--flash-freq', '40m',
               '--flash-size', '4MB']
    if args.erase_all:
        command.append('--erase-all')
    # Separate parts leave the NVS sectors untouched on later re-flashes.
    for offset, filename in parts:
        command.extend([hex(offset), str(ROOT / 'pocket-release' / kind / filename)])
    subprocess.run(command, check=True)


if __name__ == '__main__':
    main()
