"""Regression tests for one-time optional DLC prompts following a v1.00 update.

Run with: python3 -m unittest discover -s tests -p test_launch_dlc_flow.py
No PS4 game data, PKGs, GPU, or setup executable are required.
"""
from pathlib import Path
import sys
import tempfile
import unittest
from unittest.mock import patch

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "scripts"))
import ps4rt_launch as launcher


class Restarted(Exception):
    pass


class Booted(Exception):
    pass


class LauncherDlcFlowTests(unittest.TestCase):
    def run_flow(self, version, *, selected=(), flags=(), fails_update=False):
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            game = root / "CUSA16429"
            (game / "sce_sys").mkdir(parents=True)
            (root / "bin").mkdir()
            (root / "bin/ps4rt").write_bytes(b"mock portable")
            events = []

            def update(_root, original, **kwargs):
                events.append("update")
                if fails_update:
                    raise launcher.SetupError("simulated update failure")
                return original

            def picker(_root, kind):
                events.append("picker:" + kind)
                return list(selected)

            def install(_root, packages, **kwargs):
                events.append("install")
                self.assertEqual(packages, list(selected))
                return ["DLC"] if packages else []

            def restart(*args, **kwargs):
                events.append("restart")
                raise Restarted()

            def execve(*args):
                events.append("boot")
                raise Booted()

            with patch.object(launcher, "ROOT", root), \
                 patch.object(launcher, "load_sfo", return_value={
                     "TITLE_ID": "CUSA16429", "APP_VER": version
                 }), \
                 patch.object(launcher, "resolve_game", side_effect=update), \
                 patch.object(launcher, "incomplete_marker",
                              return_value=game / ".incomplete"), \
                 patch.object(launcher, "choose_setup", side_effect=picker), \
                 patch.object(launcher, "install_dlc", side_effect=install), \
                 patch.object(launcher, "_restart_after_update", side_effect=restart), \
                 patch.object(launcher, "prepare"), \
                 patch.object(launcher, "link"), \
                 patch.object(launcher, "installed_addons", return_value={}), \
                 patch.object(launcher, "prepare_content"), \
                 patch.object(launcher, "os") as mocked_os:
                mocked_os.environ = {}
                mocked_os.execve.side_effect = execve
                argv = ["--game", str(game), *flags]
                if fails_update:
                    with self.assertRaisesRegex(launcher.SetupError, "simulated update failure"):
                        launcher.main(argv)
                elif version == "01.00":
                    with self.assertRaises(Restarted):
                        launcher.main(argv)
                else:
                    with self.assertRaises(Booted):
                        launcher.main(argv)
            return events

    def test_v100_shows_dlc_picker_after_successful_update(self):
        self.assertEqual(self.run_flow("01.00", selected=["a.pkg"]),
                         ["update", "picker:dlc", "install", "restart"])

    def test_v100_cancel_dlc_still_starts(self):
        self.assertEqual(self.run_flow("01.00"),
                         ["update", "picker:dlc", "restart"])

    def test_preupdated_v101_has_no_prompts(self):
        self.assertEqual(self.run_flow("01.01"), ["update", "boot"])

    def test_headless_v100_skips_picker(self):
        self.assertEqual(self.run_flow("01.00", flags=["--no-dialogs"]),
                         ["update", "restart"])

    def test_explicit_dlc_skips_picker(self):
        self.assertEqual(self.run_flow("01.00", flags=["--dlc-pkg", "a.pkg"]),
                         ["update", "restart"])

    def test_failed_update_never_offers_dlc(self):
        self.assertEqual(self.run_flow("01.00", fails_update=True), ["update"])


if __name__ == "__main__":
    unittest.main()
