"""Run upstream tests, restoring temporary working directories before Windows cleanup.

Several upstream tests restore cwd after TemporaryDirectory.__exit__, which Windows
cannot delete while it is the process cwd. Assertions and tested code are unchanged.
"""
import os
from pathlib import Path
import tempfile
import unittest
import subprocess

ROOT = Path(__file__).resolve().parents[1]
os.chdir(ROOT)
OriginalTemporaryDirectory = tempfile.TemporaryDirectory


class PortableTemporaryDirectory(OriginalTemporaryDirectory):
    def __enter__(self):
        self.previous_directory = Path.cwd()
        return super().__enter__()

    def __exit__(self, *args):
        current = Path.cwd().resolve()
        temporary = Path(self.name).resolve()
        if current == temporary or temporary in current.parents:
            os.chdir(self.previous_directory)
        return super().__exit__(*args)


if __name__ == "__main__":
    tempfile.TemporaryDirectory = PortableTemporaryDirectory
    original_run = subprocess.run
    git_bash = Path(os.environ.get("ProgramFiles", "C:/Program Files")) / "Git/bin/bash.exe"
    def portable_run(args, *positional, **kwargs):
        # Windows CreateProcess searches System32 before PATH and selects the
        # WSL launcher even when no Linux distribution is installed.
        if os.name == "nt" and isinstance(args, list) and args and args[0] == "bash" and git_bash.exists():
            args = [str(git_bash), *args[1:]]
        return original_run(args, *positional, **kwargs)
    subprocess.run = portable_run
    suite = unittest.defaultTestLoader.discover(str(ROOT / "scripts/tests"))
    outcome = unittest.TextTestRunner(verbosity=2).run(suite)
    raise SystemExit(0 if outcome.wasSuccessful() else 1)
