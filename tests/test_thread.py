"""Real pthread lifecycle checks for the guest libc destructor hook."""
from paths import ROOT
import subprocess
import unittest

EXE = ROOT / 'out/thread-test'


@unittest.skipUnless(EXE.exists(), 'run bash build.sh --test first')
class ThreadTests(unittest.TestCase):
    def test_destructors_on_return_exit_replacement_and_clear(self):
        result = subprocess.run([str(EXE)], capture_output=True, text=True, timeout=5)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertIn('Guest thread destructor lifecycle passed', result.stdout)
