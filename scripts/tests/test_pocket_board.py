import json
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]
BOARD = ROOT / 'main/boards/custom-esp32-nodemcu-pocket-ai'


class PocketBoardTests(unittest.TestCase):
    def test_native_custom_backend_url_and_device_token_bounds(self):
        compiler = shutil.which('g++')
        if not compiler:
            self.skipTest('native C++ compiler not installed')
        with tempfile.TemporaryDirectory() as directory:
            executable = Path(directory) / 'backend-test.exe'
            subprocess.run([compiler, '-std=c++17', '-Wall', '-Wextra', '-Werror',
                            '-I' + str(BOARD), str(ROOT / 'scripts/tests/pocket_backend_test.cpp'),
                            '-o', str(executable)], check=True)
            subprocess.run([str(executable)], check=True)

    def test_native_hands_free_retry_pause_and_resume(self):
        compiler = shutil.which('g++')
        if not compiler:
            self.skipTest('native C++ compiler not installed')
        with tempfile.TemporaryDirectory() as directory:
            executable = Path(directory) / 'hands-free-test.exe'
            subprocess.run([compiler, '-std=c++17', '-Wall', '-Wextra', '-Werror',
                            '-I' + str(BOARD), str(ROOT / 'scripts/tests/pocket_hands_free_test.cpp'),
                            '-o', str(executable)], check=True)
            subprocess.run([str(executable)], check=True)

    def test_native_wifi_credentials_and_bounded_trials(self):
        compiler = shutil.which('g++')
        if not compiler:
            self.skipTest('native C++ compiler not installed')
        with tempfile.TemporaryDirectory() as directory:
            executable = Path(directory) / 'wifi-test.exe'
            subprocess.run([compiler, '-std=c++17', '-Wall', '-Wextra', '-Werror',
                            '-I' + str(BOARD), str(ROOT / 'scripts/tests/pocket_wifi_test.cpp'),
                            '-o', str(executable)], check=True)
            subprocess.run([str(executable)], check=True)

    def test_native_face_timing_audio_and_bounds(self):
        compiler = shutil.which('g++')
        if not compiler:
            self.skipTest('native C++ compiler not installed')
        with tempfile.TemporaryDirectory() as directory:
            executable = Path(directory) / 'face-test.exe'
            subprocess.run([compiler, '-std=c++17', '-Wall', '-Wextra', '-Werror',
                            '-I' + str(BOARD), str(ROOT / 'scripts/tests/pocket_face_test.cpp'),
                            '-o', str(executable)], check=True)
            subprocess.run([str(executable)], check=True)

    def test_native_audio_and_websocket_bounds(self):
        compiler = shutil.which('g++')
        if not compiler:
            self.skipTest('native C++ compiler not installed')
        with tempfile.TemporaryDirectory() as directory:
            executable = Path(directory) / 'pocket-test.exe'
            subprocess.run([compiler, '-std=c++17', '-Wall', '-Wextra', '-Werror',
                            '-I' + str(BOARD), '-I' + str(ROOT / 'main/protocols'),
                            str(ROOT / 'scripts/tests/pocket_native_test.cpp'),
                            '-o', str(executable)], check=True)
            subprocess.run([str(executable)], check=True)

    def test_unique_identity_and_reduced_configuration(self):
        config = json.loads((BOARD / 'config.json').read_text(encoding='utf-8'))
        self.assertEqual(config['target'], 'esp32')
        self.assertEqual(config['type'], BOARD.name)
        options = config['builds'][0]['sdkconfig_append']
        self.assertIn('CONFIG_ALLOW_FIRMWARE_UPDATES=n', options)
        self.assertIn('CONFIG_SPIRAM=n', options)
        self.assertIn('CONFIG_FLASH_NONE_ASSETS=y', options)
        self.assertIn('CONFIG_AUDIO_TEST_DURATION_MS=2000', options)
        self.assertEqual((BOARD / 'pocket_board.cc').read_text().count('DECLARE_BOARD('), 1)

    def test_partition_bounds(self):
        lines = (ROOT / 'partitions/custom-pocket-4m.csv').read_text().splitlines()
        previous_end = 0x9000
        apps = []
        for line in lines:
            if not line.strip() or line.startswith('#'):
                continue
            name, kind, subtype, offset, size, _ = [x.strip() for x in line.split(',')]
            start, length = int(offset, 0), int(size, 0)
            self.assertGreaterEqual(start, previous_end)
            self.assertLessEqual(start + length, 0x400000)
            previous_end = start + length
            if kind == 'app':
                apps.append((name, subtype, start, length))
        self.assertEqual(apps, [('factory', 'factory', 0x10000, 0x3f0000)])
