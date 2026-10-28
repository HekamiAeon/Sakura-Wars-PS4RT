"""Static regression guards for the native overlay's DLC restart handshake."""
from pathlib import Path
import unittest

SOURCE = Path(__file__).resolve().parents[1] / "gpu/shim/ps4rt_overlay.cpp"


class DlcOverlayRestartTests(unittest.TestCase):
    def test_installer_distinguishes_added_from_skipped(self):
        source = SOURCE.read_text(encoding="utf-8")
        self.assertIn("WEXITSTATUS(status) == 10 ? 3", source)
        self.assertIn("WEXITSTATUS(status) == 0 ? 2", source)
        self.assertIn("No new DLC installed. Continue playing normally.", source)

    def test_new_content_restart_requires_confirmation(self):
        source = SOURCE.read_text(encoding="utf-8")
        self.assertIn('if (ImGui::Button("Restart to load DLC..."))', source)
        self.assertIn('ImGui::BeginPopupModal("Restart for DLC?"', source)
        self.assertIn("Unsaved game progress will be lost.", source)
        self.assertIn('if (ImGui::Button("Restart now"))', source)
        self.assertIn("runtime_restart();", source)

    def test_cancellation_does_not_close_overlay(self):
        source = SOURCE.read_text(encoding="utf-8")
        self.assertIn('!ImGui::IsPopupOpen("Restart for DLC?")', source)


if __name__ == "__main__":
    unittest.main()
