import importlib.util
from pathlib import Path
import sys
import tempfile
import unittest
from unittest import mock

ROOT = Path(__file__).resolve().parents[2]
spec = importlib.util.spec_from_file_location('pocket_package', ROOT / 'scripts/pocket_package.py')
package = importlib.util.module_from_spec(spec)
with mock.patch.object(sys, 'path', [str(ROOT / 'scripts'), *sys.path]):
    spec.loader.exec_module(package)


class PocketPackageTests(unittest.TestCase):
    def test_release_inventory_excludes_private_backups_and_unrelated_local_files(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            expected = ['pocket-xiaozhi-esp32.bin', 'conversation/xiaozhi.bin',
                        'conversation/sdkconfig.txt', 'licenses/opus/LICENSE']
            private = ['private-backups/nvs.bin', 'private-backups/flash.bin',
                       'private-backups/restore.log', '.env.local', 'mic-speech.wav',
                       'conversation/private.key', 'notes.txt']
            for name in expected + private:
                path = root / name
                path.parent.mkdir(parents=True, exist_ok=True)
                path.write_bytes(b'local test content')
            actual = [path.relative_to(root).as_posix() for path in package.release_files(root)]
            self.assertEqual(sorted(actual), sorted(expected))


if __name__ == '__main__':
    unittest.main()
