"""Capture ESP32 logs with optional reset and read-only/display serial commands."""
import argparse
from pathlib import Path
import time
import serial
from esptool.reset import HardReset


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--port', required=True)
    parser.add_argument('--seconds', type=int, default=45)
    parser.add_argument('--reset', action='store_true')
    parser.add_argument('--command', action='append', default=[],
                        choices=['diag', 'info', 'colors', 'home'])
    parser.add_argument('--output', required=True)
    args = parser.parse_args()
    if not 1 <= args.seconds <= 120:
        parser.error('--seconds must be between 1 and 120')
    path = Path(args.output)
    path.parent.mkdir(parents=True, exist_ok=True)
    port = serial.Serial(port=None, baudrate=115200, timeout=0.1)
    port.dtr = False
    port.rts = False
    port.port = args.port
    with port, path.open('wb') as log:
        if args.reset:
            HardReset(port).reset()
        start = time.monotonic()
        sent = 0
        while time.monotonic() - start < args.seconds:
            elapsed = time.monotonic() - start
            if sent < len(args.command) and elapsed >= 5 + 4 * sent:
                command = args.command[sent]
                marker = f'\n# HOST command {command}\n'.encode()
                log.write(marker)
                port.write((command + '\n').encode())
                port.flush()
                sent += 1
            data = port.read(port.in_waiting or 1)
            if data:
                log.write(data)
                log.flush()
    print(f'Captured {args.seconds}s from {args.port}: {path.resolve()}')


if __name__ == '__main__':
    main()
