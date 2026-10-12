from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]
BOARD = ROOT / 'main/boards/custom-esp32-nodemcu-pocket-ai'


class PocketClockTests(unittest.TestCase):
    def test_production_clock_intents_state_speech_gate_and_midnight(self):
        compiler = shutil.which('g++')
        if not compiler:
            self.skipTest('native C++ compiler not installed')
        with tempfile.TemporaryDirectory() as directory:
            executable = Path(directory) / 'clock-test.exe'
            subprocess.run([compiler, '-std=c++17', '-Wall', '-Wextra', '-Werror',
                            '-I' + str(BOARD), str(ROOT / 'scripts/tests/pocket_clock_test.cpp'),
                            '-o', str(executable)], check=True)
            subprocess.run([str(executable)], check=True)
