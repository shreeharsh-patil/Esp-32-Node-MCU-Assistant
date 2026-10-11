"""Capture logs and export a complete, indexed mic PCM recording as a WAV."""
import argparse
from pathlib import Path
import re
import math
import struct
import time
import wave
import zlib


class PcmSerialTransport:
    """Follow complete firmware baud markers, including across serial reads."""
    def __init__(self, port):
        self.port = port
        self.pending = b''

    def feed(self, data):
        self.pending += data
        while b'\n' in self.pending:
            line, self.pending = self.pending.split(b'\n', 1)
            match = re.fullmatch(rb'PCM_UART baud=(921600|115200)\r?', line)
            if match and self.port.baudrate != int(match[1]):
                self.port.baudrate = int(match[1])
                if self.port.baudrate == 921600:
                    time.sleep(0.05)  # Let the device finish its old-speed marker.
                    self.port.write(b'\x06')
                    self.port.flush()
        self.pending = self.pending[-4096:]


def extract_pcm(log: bytes) -> tuple[bytes, dict]:
    """Reject missing/reordered/truncated PCM; logs may interleave between rows."""
    text = log.decode('ascii', errors='replace')
    begin = list(re.finditer(r'^PCM_BEGIN rate=16000 samples=(16000|80000)\r?$', text, re.M))
    if not begin:
        raise ValueError('No microphone PCM export found; use the repaired firmware and mic command')
    recording = text[begin[-1].end():]
    expected = int(begin[-1][1])
    end = re.search(r'^PCM_END samples=(\d+) rx_errors=(\d+) overruns=(\d+) clipped=(\d+)(?: crc32=([0-9a-f]{8}))?\r?$',
                    recording, re.M)
    if not end:
        raise ValueError('Incomplete PCM export: PCM_END missing')
    pcm = bytearray()
    for row in re.finditer(r'^PCM (\d+) ([0-9a-f]{256})\r?$', recording[:end.start()], re.M):
        if int(row[1]) != len(pcm) // 2:
            raise ValueError('PCM rows missing, duplicated or out of order')
        pcm.extend(bytes.fromhex(row[2]))
    if len(pcm) != expected * 2 or int(end[1]) != expected:
        raise ValueError(f'Incomplete PCM export: got {len(pcm) // 2} of {expected} samples')
    if expected == 80000 and not end[5]:
        raise ValueError('Five-second PCM export is missing its CRC32')
    if end[5] and zlib.crc32(pcm) != int(end[5], 16):
        raise ValueError('PCM CRC32 mismatch: serial data corrupted')
    samples = struct.unpack(f'<{expected}h', pcm)
    metrics = dict(frames=expected, rx_errors=int(end[2]), overruns=int(end[3]), clipped=int(end[4]),
                   clipping_percent=round(100 * int(end[4]) / expected, 2),
                   rms=round(math.sqrt(sum(x * x for x in samples) / len(samples)), 2),
                   peak=max(abs(x) for x in samples))
    return bytes(pcm), metrics


def write_wav(log: bytes, path: Path) -> dict:
    pcm, metrics = extract_pcm(log)
    path.parent.mkdir(parents=True, exist_ok=True)
    with wave.open(str(path), 'wb') as wav:
        wav.setnchannels(1)
        wav.setsampwidth(2)
        wav.setframerate(16000)
        wav.writeframes(pcm)
    return metrics


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--port')
    parser.add_argument('--seconds', type=int, default=45)
    parser.add_argument('--reset', action='store_true')
    parser.add_argument('--command', action='append', default=[],
                        choices=['diag', 'info', 'colors', 'home', 'mic', 'tone', 'tone-long', 'tone-loud', 'testaudio'])
    parser.add_argument('--output', required=True)
    parser.add_argument('--wav', type=Path, help='Export the last complete mic recording to WAV')
    parser.add_argument('--extract-only', action='store_true', help='Read --output as an existing log')
    args = parser.parse_args()
    if not 1 <= args.seconds <= 120:
        parser.error('--seconds must be between 1 and 120')
    path = Path(args.output)
    if args.extract_only:
        if not args.wav:
            parser.error('--extract-only requires --wav')
        try:
            print(f'WAV {args.wav.resolve()}: {write_wav(path.read_bytes(), args.wav)}')
        except ValueError as error:
            parser.error(str(error))
        return
    if not args.port:
        parser.error('--port is required for serial capture')
    import serial
    from esptool.reset import HardReset
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
        next_command_at = 5
        transport = PcmSerialTransport(port)
        while time.monotonic() - start < args.seconds:
            elapsed = time.monotonic() - start
            if sent < len(args.command) and elapsed >= next_command_at and port.baudrate == 115200:
                command = args.command[sent].replace('tone-long', 'tone long').replace('tone-loud', 'tone loud')
                marker = f'\n# HOST command {command}\n'.encode()
                log.write(marker)
                port.write((command + '\n').encode())
                port.flush()
                sent += 1
                # mic streams five seconds at high speed, then restores 115200.
                next_command_at = elapsed + (12 if command == 'mic' else 7 if command in ('tone long', 'tone loud')
                                             else 6 if command == 'testaudio' else 4)
            data = port.read(port.in_waiting or 1)
            if data:
                log.write(data)
                log.flush()
                transport.feed(data)
    print(f'Captured {args.seconds}s from {args.port}: {path.resolve()}')
    if args.wav:
        try:
            metrics = write_wav(path.read_bytes(), args.wav)
        except ValueError as error:
            parser.error(str(error))
        print(f'WAV {args.wav.resolve()}: {metrics}; speech intelligibility requires listening')


if __name__ == '__main__':
    main()
