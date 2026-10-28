"""Regression tests for first-install re-exec and unchanged ordinary launch."""
from __future__ import annotations

from pathlib import Path
import sys
import tempfile
import unittest
from unittest.mock import patch

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "scripts"))
import ps4rt_launch as launch
from player_setup import SetupError


class ProcessReplaced(Exception):
    """An intercepted exec call, which must not return to its caller."""


class FreshLaunchTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)
        self.game = self.root / "CUSA16429"
        (self.game / "sce_sys").mkdir(parents=True)
        (self.game / "sce_sys/param.sfo").write_bytes(b"mock SFO")
        (self.root / "bin").mkdir()
        (self.root / "bin/ps4rt").write_bytes(b"mock executable")

    def _mocks(self, version: str):
        """Patch game-dependent work, retaining real CLI branching."""
        return [
            patch.object(launch, "ROOT", self.root),
            patch.object(launch, "load_sfo", return_value={
                "TITLE_ID": "CUSA16429", "APP_VER": version
            }),
            patch.object(launch, "resolve_game", return_value=self.game),
            patch.object(launch, "prepare"),
            patch.object(launch, "link"),
            patch.object(launch, "installed_addons", return_value={}),
            patch.object(launch, "prepare_content"),
        ]

    def test_verified_update_restarts_normal_launcher_once(self):
        from contextlib import ExitStack
        with ExitStack() as stack:
            for target in self._mocks("01.00"):
                stack.enter_context(target)
            execv = stack.enter_context(patch.object(
                launch.os, "execv", side_effect=ProcessReplaced))
            execve = stack.enter_context(patch.object(launch.os, "execve"))
            with self.assertRaises(ProcessReplaced):
                launch.main([
                    "--game", str(self.game),
                    "--update-extracted", str(self.root / "patch"),
                    "--no-dialogs", "--upscaler", "fsr3",
                    "--output-resolution", "1440",
                    "--dlc-pkg", str(self.root / "costume.pkg")
                ])
        execv.assert_called_once()
        executable, command = execv.call_args.args
        self.assertEqual(executable, sys.executable)
        self.assertEqual(command[:4],
                         [sys.executable, str(Path(launch.__file__).resolve()),
                          "--game", str(self.game)])
        self.assertNotIn("--update-extracted", command)
        self.assertNotIn("--update-pkg", command)
        self.assertEqual(command.count("--game"), 1)
        for forwarded in ("--no-dialogs", "--upscaler", "fsr3",
                          "--output-resolution", "1440", "--dlc-pkg"):
            self.assertIn(forwarded, command)
        execve.assert_not_called()  # No game started in the installer image.

    def test_v101_never_launches_optional_dlc_picker(self):
        # This is a real startup path, without --no-dialogs, on a fresh
        # portable. No DLC prompt, no marker, no re-exec before game launch.
        from contextlib import ExitStack
        with ExitStack() as stack:
            for target in self._mocks("01.01"):
                stack.enter_context(target)
            picker = stack.enter_context(patch.object(
                launch, "add_dlc_dialog"))
            installer = stack.enter_context(patch.object(
                launch, "install_dlc"))
            execv = stack.enter_context(patch.object(launch.os, "execv"))
            execve = stack.enter_context(patch.object(
                launch.os, "execve", side_effect=ProcessReplaced))
            with self.assertRaises(ProcessReplaced):
                launch.main(["--game", str(self.game)])
        picker.assert_not_called()
        installer.assert_not_called()
        execv.assert_not_called()
        execve.assert_called_once()
        self.assertFalse((self.root / "out/CUSA16429/player-content/dlc-offered.json").exists())

    def test_existing_v101_boots_without_restart(self):
        from contextlib import ExitStack
        with ExitStack() as stack:
            for target in self._mocks("01.01"):
                stack.enter_context(target)
            execv = stack.enter_context(patch.object(launch.os, "execv"))
            execve = stack.enter_context(patch.object(
                launch.os, "execve", side_effect=ProcessReplaced))
            with self.assertRaises(ProcessReplaced):
                launch.main(["--game", str(self.game), "--no-dialogs"])
        execv.assert_not_called()
        execve.assert_called_once()
        self.assertEqual(execve.call_args.args[1][2], "--app0")

    def test_cli_dlc_install_reexec_does_not_repeat_pkg(self):
        from contextlib import ExitStack
        pkg = self.root / "addon.pkg"
        with ExitStack() as stack:
            for target in self._mocks("01.01"):
                stack.enter_context(target)
            installer = stack.enter_context(patch.object(
                launch, "install_dlc", return_value=["COSTUME000000001"]))
            execv = stack.enter_context(patch.object(
                launch.os, "execv", side_effect=ProcessReplaced))
            execve = stack.enter_context(patch.object(launch.os, "execve"))
            with self.assertRaises(ProcessReplaced):
                launch.main(["--game", str(self.game), "--dlc-pkg", str(pkg),
                             "--no-dialogs", "--output-resolution", "4k"])
        installer.assert_called_once()
        execve.assert_not_called()
        command = execv.call_args.args[1]
        self.assertNotIn("--dlc-pkg", command)
        self.assertIn("--no-dialogs", command)
        self.assertIn("--output-resolution", command)
        self.assertIn("4k", command)

    def test_overlay_install_reports_additions_to_parent(self):
        from player_setup import DLC_INSTALLED_EXIT
        with patch.object(launch, "load_sfo", return_value={
            "TITLE_ID": "CUSA16429", "APP_VER": "01.01"
        }), patch.object(launch, "ROOT", self.root), \
             patch.object(launch, "add_dlc_dialog", return_value=DLC_INSTALLED_EXIT):
            self.assertEqual(launch.main(["--game", str(self.game),
                                          "--install-dlc"]), DLC_INSTALLED_EXIT)

    def test_restart_command_is_fresh_and_never_opens_setup(self):
        # Executing restart from the running overlay must relaunch Python's
        # preflight, not reuse old --addcont mounts or open setup dialogs.
        import shlex
        from contextlib import ExitStack
        with ExitStack() as stack:
            for target in self._mocks("01.01"):
                stack.enter_context(target)
            execve = stack.enter_context(patch.object(
                launch.os, "execve", side_effect=ProcessReplaced))
            with self.assertRaises(ProcessReplaced):
                launch.main(["--game", str(self.game)])
        runtime, argv, env = execve.call_args.args
        self.assertTrue(runtime.endswith("/bin/ps4rt"))
        command = shlex.split(env["PS4RT_RESTART_COMMAND"])
        self.assertEqual(command[:2],
                         [sys.executable, str(Path(launch.__file__).resolve())])
        self.assertEqual(command[2:5],
                         ["--game", str(self.game), "--no-dialogs"])
        self.assertNotIn("--install-dlc", command)
        self.assertNotIn("--update-pkg", command)
        self.assertNotIn("--dlc-pkg", command)

    def test_prepare_only_does_not_restart_or_launch(self):
        from contextlib import ExitStack
        with ExitStack() as stack:
            for target in self._mocks("01.00"):
                stack.enter_context(target)
            execv = stack.enter_context(patch.object(launch.os, "execv"))
            execve = stack.enter_context(patch.object(launch.os, "execve"))
            result = launch.main([
                "--game", str(self.game), "--prepare-only",
                "--update-extracted", str(self.root / "patch")
            ])
        self.assertEqual(result, 0)
        execv.assert_not_called()
        execve.assert_not_called()

    def test_failed_update_never_restarts_or_launches(self):
        with patch.object(launch, "ROOT", self.root), \
             patch.object(launch, "load_sfo", return_value={
                 "TITLE_ID": "CUSA16429", "APP_VER": "01.00"
             }), \
             patch.object(launch, "resolve_game",
                          side_effect=SetupError("update rejected")), \
             patch.object(launch.os, "execv") as execv, \
             patch.object(launch.os, "execve") as execve:
            with self.assertRaisesRegex(SetupError, "update rejected"):
                launch.main(["--game", str(self.game), "--no-dialogs"])
        execv.assert_not_called()
        execve.assert_not_called()

    def test_interrupted_v101_recovery_restarts_after_success(self):
        from contextlib import ExitStack
        marker = self.game / ".ps4rt-v101-update-incomplete.json"
        marker.write_text("{}")
        with ExitStack() as stack:
            for target in self._mocks("01.01"):
                stack.enter_context(target)
            execv = stack.enter_context(patch.object(
                launch.os, "execv", side_effect=ProcessReplaced))
            execve = stack.enter_context(patch.object(launch.os, "execve"))
            with self.assertRaises(ProcessReplaced):
                launch.main([
                    "--game", str(self.game), "--update-extracted",
                    str(self.root / "patch"), "--no-dialogs"
                ])
        execv.assert_called_once()
        execve.assert_not_called()


if __name__ == "__main__":
    unittest.main()
