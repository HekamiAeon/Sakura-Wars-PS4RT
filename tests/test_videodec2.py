"""Videodec2 ABI validation and software compute reservation lifecycle."""
from paths import ROOT
import subprocess
import unittest

EXE = ROOT / 'out/videodec2-test'


@unittest.skipUnless(EXE.exists(), 'run bash build.sh --test first')
class Videodec2Tests(unittest.TestCase):
    def test_compute_lifecycle(self):
        result = subprocess.run([str(EXE)], capture_output=True, text=True, timeout=5)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertIn('Videodec2 compute lifecycle passed', result.stdout)
