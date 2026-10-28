"""Free physical range queries across fragmentation, alignment and release."""
from paths import ROOT
import subprocess
import unittest

EXE = ROOT / 'out/direct-memory-test'


@unittest.skipUnless(EXE.exists(), 'run bash build.sh --test first')
class DirectMemoryTests(unittest.TestCase):
    def test_available_ranges(self):
        result = subprocess.run([str(EXE)], capture_output=True, text=True, timeout=5)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertIn('Direct memory availability passed', result.stdout)
