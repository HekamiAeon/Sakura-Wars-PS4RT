"""Internal resolution: the overlay's saved choice becomes PS4RT_INTERNAL_RES on the next launch.

The loader (src/loader.c) turns it into the engine's own --res/--renderres options; the
settings and FSR output rules are covered by out/gpu/graphics-settings-test.
"""
from __future__ import annotations

from pathlib import Path
import re
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "scripts"))
import ps4rt_launch as launch


class InternalResolutionTests(unittest.TestCase):
    def setUp(self):
        temp = tempfile.TemporaryDirectory()
        self.addCleanup(temp.cleanup)
        self.ini = Path(temp.name) / "ps4rt.ini"

    def launch(self, ini_text=None, env=None):
        if ini_text is not None:
            self.ini.write_text(ini_text)
        env = dict(env or {})
        launch._internal_resolution(self.ini, env)
        return env

    def test_native_and_missing_configuration_set_nothing(self):
        self.assertNotIn("PS4RT_INTERNAL_RES", self.launch())
        self.assertNotIn("PS4RT_INTERNAL_RES", self.launch("render_resolution=native\n"))
        self.assertNotIn("PS4RT_INTERNAL_RES", self.launch("render_resolution=bogus\n"))

    def test_saved_choice_applies(self):
        env = self.launch("upscaler=off\nrender_resolution=5120x1440\n")
        self.assertEqual(env["PS4RT_INTERNAL_RES"], "5120x1440")
        self.assertEqual(env["PS4RT_INTERNAL_RES_FROM_INI"], "1")
        self.assertEqual(self.launch("render_resolution=540p\n")["PS4RT_INTERNAL_RES"], "1024x576")

    def test_restart_rereads_the_ini(self):
        """"Apply and restart" inherits this launch's value; the newly saved choice wins."""
        first = self.launch("render_resolution=2160p\n")
        second = self.launch("render_resolution=native\n", first)
        self.assertNotIn("PS4RT_INTERNAL_RES", second)
        third = self.launch("render_resolution=3440x1440\n", first)
        self.assertEqual(third["PS4RT_INTERNAL_RES"], "3440x1440")

    def test_explicit_caller_value_wins(self):
        env = self.launch("render_resolution=2160p\n", {"PS4RT_INTERNAL_RES": "2560x1440"})
        self.assertEqual(env["PS4RT_INTERNAL_RES"], "2560x1440")
        self.assertNotIn("PS4RT_INTERNAL_RES_FROM_INI", env)

    def test_launcher_keys_match_the_menu(self):
        graphics = (ROOT / "gpu/shim/ps4rt_graphics.h").read_text(encoding="utf-8")
        keys = re.search(r"Resolutions = \{([^}]*)\}", graphics).group(1)
        widths = re.search(r"ResolutionWidths = \{([^}]*)\}", graphics).group(1)
        heights = re.search(r"ResolutionHeights = \{([^}]*)\}", graphics).group(1)
        keys = re.findall(r'"([^"]+)"', keys)
        sizes = [f"{w.strip()}x{h.strip()}" for w, h in zip(widths.split(","), heights.split(","))]
        self.assertEqual(keys[0], "native")
        self.assertEqual(dict(zip(keys[1:], sizes[1:])), launch.RENDER_RESOLUTIONS)


class LoaderResolutionTests(unittest.TestCase):
    def test_loader_builds_engine_options_from_the_internal_resolution(self):
        source = (ROOT / "src/loader.c").read_text(encoding="utf-8")
        self.assertIn('getenv("PS4RT_INTERNAL_RES")', source)
        for option in ("--res_x=%u", "--res_y=%u", "--renderres_x=%u", "--renderres_y=%u"):
            self.assertIn(option, source)
        # The pool, heap and UI anchors follow the same switch as the engine options.
        self.assertIn("if (resolution_mode()) {", source)


if __name__ == "__main__":
    unittest.main()
