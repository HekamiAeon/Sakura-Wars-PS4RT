"""Game language: the overlay's pending choice becomes the emulated console language.

Sakura Wars reads the PS4 system language at boot, so the launcher only has to choose
PS4RT_LANGUAGE before the runtime starts. No game save is read or written.
"""
from __future__ import annotations

from pathlib import Path
import sys
import tempfile
import unittest
from unittest.mock import patch

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "scripts"))
import ps4rt_launch as launch

LANGUAGES = ((1, "English"), (3, "Spanish"), (0, "Japanese"))


class ConsoleLanguageTests(unittest.TestCase):
    def setUp(self):
        temp = tempfile.TemporaryDirectory()
        self.addCleanup(temp.cleanup)
        self.root = Path(temp.name)
        self.user = self.root / "user"
        self.user.mkdir()
        self.ini = self.root / "ps4rt.ini"
        self.console = self.user / "console.ini"
        languages = patch.object(launch, "CONSOLE_LANGUAGES", LANGUAGES)
        languages.start()
        self.addCleanup(languages.stop)

    def launch(self, env=None):
        env = dict(env or {})
        active = launch._console_language(self.user, self.ini, env)
        self.assertEqual(env["PS4RT_LANGUAGE"], str(active))
        return active, env

    def pending(self):
        return launch._ini_value(self.ini, "language")

    def stored(self):
        return launch._ini_value(self.console, "language")

    def test_first_launch_without_configuration_keeps_english(self):
        active, env = self.launch()
        self.assertEqual(active, 1)
        self.assertEqual(env["PS4RT_LANGUAGE_CHOICES"], "1=English|3=Spanish|0=Japanese")
        self.assertFalse(self.console.exists())  # nothing chosen, nothing written
        self.assertFalse(self.ini.exists())

    def test_pending_choice_applies_before_start_then_persists(self):
        self.ini.write_text("# ps4rt settings\nupscaler=off\nlanguage=3\nanisotropy=16\n")
        active, _ = self.launch()
        self.assertEqual(active, 3)
        self.assertEqual(self.stored(), "3")
        self.assertEqual(self.pending(), "-1")
        # Every other setting is preserved byte for byte.
        self.assertEqual(self.ini.read_text(),
                         "# ps4rt settings\nupscaler=off\nlanguage=-1\nanisotropy=16\n")
        # The next launch, with no pending change, keeps the console language.
        active, _ = self.launch()
        self.assertEqual(active, 3)
        self.ini.write_text("language=0\n")
        self.assertEqual(self.launch()[0], 0)
        self.assertEqual(self.stored(), "0")
        self.ini.write_text("language=1\n")
        self.assertEqual(self.launch()[0], 1)

    def test_unchanged_selection_never_writes(self):
        self.ini.write_text("language=-1\n")
        self.console.write_text("language=0\n")
        before = (self.ini.stat().st_mtime_ns, self.console.stat().st_mtime_ns)
        self.assertEqual(self.launch()[0], 0)
        self.assertEqual((self.ini.stat().st_mtime_ns, self.console.stat().st_mtime_ns), before)

    def test_restart_inherits_this_launchs_value_but_pending_wins(self):
        self.ini.write_text("language=3\n")
        _, env = self.launch({"PS4RT_LANGUAGE": "1"})  # inherited from the previous launch
        self.assertEqual(env["PS4RT_LANGUAGE"], "3")
        _, env = self.launch(env)  # the next restart, nothing pending
        self.assertEqual(env["PS4RT_LANGUAGE"], "3")

    def test_explicit_override_is_for_one_launch_only(self):
        self.console.write_text("language=3\n")
        self.assertEqual(self.launch({"PS4RT_LANGUAGE": "0"})[0], 0)
        self.assertEqual(self.stored(), "3")
        self.assertEqual(self.launch()[0], 3)

    def test_invalid_values_never_switch_the_language(self):
        self.console.write_text("language=3\n")
        for bad in ("x", "-4", "31", "2"):  # 2 is a real PS4 ID this build does not offer
            self.ini.write_text(f"upscaler=off\nlanguage={bad}\n")
            with patch("builtins.print"):
                active, env = self.launch({"PS4RT_LANGUAGE": "zz"})
            self.assertEqual(active, 3, bad)
            self.assertEqual(self.stored(), "3")
            self.assertEqual(self.pending(), "-1", bad)  # never applicable, so cleared
            self.assertEqual(launch._ini_value(self.ini, "upscaler"), "off")
            self.assertNotIn("Other", env["PS4RT_LANGUAGE_CHOICES"])
        self.console.write_text("language=garbage\n")
        self.assertEqual(self.launch()[0], 1)

    def test_unlisted_manual_override_is_shown_not_misreported(self):
        _, env = self.launch({"PS4RT_LANGUAGE": "8"})
        self.assertEqual(env["PS4RT_LANGUAGE"], "8")
        self.assertTrue(env["PS4RT_LANGUAGE_CHOICES"].endswith("|8=Other (system language 8)"))

    def test_store_failure_keeps_the_pending_choice(self):
        self.ini.write_text("language=0\n")
        self.console.mkdir()  # cannot be replaced by a file
        with patch("builtins.print") as printed:
            active, _ = self.launch()
        self.assertEqual(active, 0)  # still used for this launch
        self.assertEqual(self.pending(), "0")  # retried next launch
        self.assertIn("could not store", printed.call_args.args[0])

    def test_trophy_text_follows_the_chosen_language(self):
        self.ini.write_text("language=0\n")
        env = {}
        launch._console_language(self.user, self.ini, env)
        with patch("trophy_catalog.prepare_catalog", return_value=None) as prepare:
            launch._achievements("CUSA16429", self.root / "game", self.user, env)
        self.assertEqual(env["PS4RT_TROPHY_LANGUAGE"], "0")
        self.assertEqual(prepare.call_args.args[2], 0)


class OfferedLanguagesTests(unittest.TestCase):
    def test_offered_languages_are_distinct_ps4_ids_with_labels(self):
        ids = [i for i, _ in launch.CONSOLE_LANGUAGES]
        self.assertEqual(len(ids), len(set(ids)))
        self.assertIn(launch.DEFAULT_CONSOLE_LANGUAGE, ids)
        for i, label in launch.CONSOLE_LANGUAGES:
            self.assertTrue(0 <= i <= launch.MAX_SYSTEM_LANGUAGE)
            self.assertTrue(label and "|" not in label and "=" not in label)


if __name__ == "__main__":
    unittest.main()
