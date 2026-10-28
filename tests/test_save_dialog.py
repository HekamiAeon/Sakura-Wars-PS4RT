"""Save-dialog startup lifecycle and existing interactive paths."""
from paths import ROOT
import subprocess
import unittest

EXE = ROOT / 'out/save-dialog-test'


@unittest.skipUnless(EXE.exists(), 'run bash build.sh --test first')
class SaveDialogTests(unittest.TestCase):
    def test_running_acknowledgement_progress_and_selection(self):
        result = subprocess.run([str(EXE)], capture_output=True, text=True, timeout=5)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertIn('Save dialog running, acknowledgement, progress and selection passed', result.stdout)
