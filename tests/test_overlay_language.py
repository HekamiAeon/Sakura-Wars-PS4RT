"""Static guards for the overlay's Game language row and the F12 screenshot wiring.

The pending-change protocol itself is covered by out/gpu/graphics-settings-test and
tests/test_game_language.py; these guard the overlay integration points.
"""
from pathlib import Path
import re
import unittest

ROOT = Path(__file__).resolve().parents[1]
OVERLAY = ROOT / "gpu/shim/ps4rt_overlay.cpp"
PRESENTER = ROOT / "gpu/shadps4/video_core/renderer_vulkan/vk_presenter.cpp"


def function_body(source: str, signature: str) -> str:
    start = source.index(signature)
    depth, i = 0, source.index("{", start)
    for j in range(i, len(source)):
        depth += {"{": 1, "}": -1}.get(source[j], 0)
        if depth == 0:
            return source[i:j + 1]
    raise AssertionError(signature)


class OverlayLanguageTests(unittest.TestCase):
    def setUp(self):
        self.source = OVERLAY.read_text(encoding="utf-8")

    def test_row_is_in_both_menus(self):
        self.assertIn("LanguageRow();", function_body(self.source, "void CompactGraphics()"))
        self.assertIn("LanguageRow();", function_body(self.source, "void GameTab()"))

    def test_compact_rows_keep_their_order(self):
        """The new row is appended; existing controls keep their positions and focus."""
        body = function_body(self.source, "void CompactGraphics()")
        order = [body.index(label) for label in (
            '"Output resolution"', '"Display mode"', '"Upscaling / anti-aliasing"',
            '"Anisotropic filtering"', '"Outlines"', '"Sharpening"', "LanguageRow();")]
        self.assertEqual(order, sorted(order))

    def test_language_change_is_a_restart_change(self):
        body = function_body(self.source, "int RestartChanges()")
        self.assertIn("s.language != s.startup_language", body)

    def test_compact_restart_applies_pending_changes(self):
        self.assertIn('"Apply and restart###restart_game" : "Restart Game###restart_game"', self.source)
        self.assertIn('ImGui::OpenPopup("Restart game?")', self.source)
        self.assertIn("Ps4RtSettings::Save();\n                    dirty = false;\n                    runtime_restart();",
                      self.source)

    def test_choices_come_from_the_launcher_with_explicit_ids(self):
        self.assertIn('std::getenv("PS4RT_LANGUAGE_CHOICES")', self.source)
        self.assertNotIn("PS4RT_GAME_LANGUAGE", self.source)  # Code Fairy's save-index protocol
        row = function_body(self.source, "void LanguageRow()")
        self.assertIn("c.ids.data()", row)  # PS4 IDs, not row indices


class ScreenshotTests(unittest.TestCase):
    def test_f12_requests_a_game_only_capture(self):
        source = OVERLAY.read_text(encoding="utf-8")
        self.assertRegex(source, r"SDLK_F12\)[^}]*Ps4RtScreenshot::Request\(\)")
        presenter = PRESENTER.read_text(encoding="utf-8")
        blit = presenter.index("MakeImageBlitFit(frame->width, frame->height, extent.width")
        capture = presenter.index("CaptureScreenshot(instance, scheduler, cmdbuf, *frame)")
        overlay = presenter.index("Ps4RtOverlay::Render(cmdbuf, swapchain.ImageView(), extent);", blit)
        self.assertLess(blit, capture)
        self.assertLess(capture, overlay)  # the menu is never in the picture


if __name__ == "__main__":
    unittest.main()
