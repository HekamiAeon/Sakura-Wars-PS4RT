"""Check that the pipeline-binary crash has a targeted, non-retrying hint."""
from pathlib import Path
import unittest

SOURCE = Path(__file__).resolve().parents[1] / "src/loader.c"


class PipelineBinaryCrashHintTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.source = SOURCE.read_text(encoding="utf-8")
        begin = cls.source.index("static void fault(int sig, siginfo_t *info, void *context) {")
        end = cls.source.index("_exit(128 + sig);", begin)
        cls.fault = cls.source[begin:end]

    def test_hint_only_for_matching_host_segfault(self):
        self.assertIn("sig == SIGSEGV && where.dli_sname", self.fault)
        self.assertIn('strstr(where.dli_sname, "InitializePipelineBinaryCache")', self.fault)
        self.assertIn("if (pipeline_binary_fault)", self.fault)

    def test_hint_contains_manual_override(self):
        self.assertIn("PS4RT_PIPELINE_BINARIES=0 bash Play-Game.sh", self.fault)
        self.assertIn("--game", self.fault)

    def test_diagnostic_only_without_automatic_retry(self):
        self.assertIn("write(2, hint, sizeof(hint) - 1);", self.fault)
        self.assertNotIn("execve(", self.fault)
        self.assertNotIn("execl(", self.fault)


if __name__ == "__main__":
    unittest.main()
