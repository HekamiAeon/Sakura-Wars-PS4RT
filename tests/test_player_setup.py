"""Offline tests of game-version preflight, cached update assembly and safe DLC import.

No game data or PKG files are required.
"""
from __future__ import annotations

import json
from pathlib import Path
import struct
import sys
import tempfile
import unittest
from unittest.mock import patch

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "scripts"))
from player_setup import SetupError, resolve_game, install_dlc, installed_addons, content_dir


def sfo(values: dict[str, str]) -> bytes:
    keys = bytearray()
    data = bytearray()
    entries = bytearray()
    for key, value in values.items():
        key_offset = len(keys)
        keys.extend(key.encode("ascii") + b"\0")
        raw = value.encode("utf8") + b"\0"
        value_offset = len(data)
        data.extend(raw)
        entries.extend(struct.pack("<HHIII", key_offset, 0x204, len(raw),
                                   len(raw), value_offset))
    keys_offset = 20 + len(entries)
    data_offset = keys_offset + len(keys)
    return (struct.pack("<4sIIII", b"\0PSF", 0x101, keys_offset,
                        data_offset, len(values)) + entries + keys + data)


def game(root: Path, version: str, category: str, name: str = "game") -> Path:
    path = root / name
    (path / "sce_sys").mkdir(parents=True)
    (path / "sce_sys/param.sfo").write_bytes(sfo({
        "TITLE_ID": "CUSA16429", "CATEGORY": category, "APP_VER": version,
        "CONTENT_ID": "EP0177-CUSA16429_00-NEWSAKURAWARSAPP"
    }))
    (path / "eboot.bin").write_bytes((version + category).encode())
    (path / "base-asset.dat").write_text("original")
    return path


class PlayerSetupTests(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.root = Path(self.tmp.name)
        self.original = game(self.root, "01.00", "gd", "original")
        self.update = game(self.root, "01.01", "gp", "update")
        (self.update / "base-asset.dat").write_text("patched")
        (self.update / "added.dat").write_text("new")

    def tearDown(self):
        self.tmp.cleanup()

    def test_v101_uses_original_without_copy(self):
        actual = resolve_game(self.root, self.update, dialogs=False)
        self.assertEqual(actual, self.update.resolve())
        self.assertFalse(content_dir(self.root).exists())

    def test_v100_requires_update(self):
        with self.assertRaisesRegex(SetupError, "v1.00 cannot launch"):
            resolve_game(self.root, self.original, dialogs=False)

    def test_native_setup_launches_one_bundled_picker(self):
        from player_setup import choose_setup
        ui = self.root / "out" / "ps4rt-setup"
        ui.parent.mkdir()
        ui.write_text("#!/bin/sh\nexit 0\n")
        ui.chmod(0o755)
        pkg = self.root / "update.pkg"
        pkg.write_bytes(b"placeholder")
        with patch("player_setup._desktop", return_value=True), \
             patch("player_setup.subprocess.run") as launch:
            launch.return_value.returncode = 0
            launch.return_value.stdout = str(pkg) + "\n"
            paths = choose_setup(self.root, "update")
        self.assertEqual(paths, [pkg])
        launch.assert_called_once_with([str(ui), "--mode", "update"],
                                       capture_output=True, text=True, check=False)

    def test_portable_picker_prefers_shipped_setup_binary(self):
        from player_setup import choose_setup
        portable = self.root / "bin" / "ps4rt-setup"
        portable.parent.mkdir()
        portable.write_text("#!/bin/sh\nexit 0\n")
        portable.chmod(0o755)
        pkg = self.root / "update.pkg"
        pkg.write_bytes(b"placeholder")
        with patch("player_setup._desktop", return_value=True), \
             patch("player_setup.subprocess.run") as launch:
            launch.return_value.returncode = 0
            launch.return_value.stdout = str(pkg) + "\n"
            self.assertEqual(choose_setup(self.root, "update"), [pkg])
        launch.assert_called_once_with([str(portable), "--mode", "update"],
                                       capture_output=True, text=True, check=False)

    def test_portable_update_progress_uses_shipped_binary(self):
        from player_setup import update_progress
        portable = self.root / "bin" / "ps4rt-setup"
        portable.parent.mkdir()
        portable.write_text("#!/bin/sh\nexit 0\n")
        portable.chmod(0o755)
        with patch("player_setup._desktop", return_value=True), \
             patch("player_setup.subprocess.Popen") as launch:
            with update_progress(self.root, show_window=True) as notify:
                command = launch.call_args.args[0]
                self.assertEqual(command[:3], [str(portable), "--mode", "progress"])
                self.assertTrue(Path(command[3]).is_file())
                notify("Testing portable update...", 2)
                self.assertIn("working\t2\t5\tTesting portable update...",
                              Path(command[3]).read_text())
            launch.assert_called_once()

    def test_native_setup_cancel_does_not_require_gui_toolkits(self):
        from player_setup import choose_setup
        ui = self.root / "out" / "ps4rt-setup"
        ui.parent.mkdir()
        ui.write_text("#!/bin/sh\nexit 1\n")
        ui.chmod(0o755)
        with patch("player_setup._desktop", return_value=True), \
             patch("player_setup.subprocess.run") as launch:
            launch.return_value.returncode = 1
            self.assertEqual(choose_setup(self.root, "update"), [])
        self.assertFalse(content_dir(self.root).exists())

    def test_native_setup_missing_executable_has_actionable_error(self):
        from player_setup import choose_setup
        with patch("player_setup._desktop", return_value=True):
            with self.assertRaisesRegex(SetupError, "bash build.sh"):
                choose_setup(self.root, "update")

    def test_native_extractor_is_bundled_without_external_installs(self):
        from player_setup import _extractor
        native = self.root / "out/ps4rt-pkg"
        native.parent.mkdir(parents=True)
        native.write_bytes(b"built by release builder")
        native.chmod(0o755)
        self.assertEqual(_extractor(self.root), str(native.resolve()))

    def test_native_extractor_missing_has_build_instructions(self):
        from player_setup import _extractor
        with patch.dict("os.environ", {"PS4RT_PKG_TOOL": ""}):
            with self.assertRaisesRegex(SetupError, "build_pkg_native.sh"):
                _extractor(self.root)

    def test_native_extractor_nonzero_return_is_failure(self):
        from player_setup import _extract
        pkg = self.root / "broken.pkg"
        pkg.write_bytes(b"bad")
        binary = self.root / "out/ps4rt-pkg"
        binary.parent.mkdir(parents=True)
        binary.write_bytes(b"mock binary")
        binary.chmod(0o755)
        import subprocess
        with patch("player_setup.subprocess.run",
                   side_effect=subprocess.CalledProcessError(1, [str(binary)])):
            with self.assertRaisesRegex(SetupError, "PKG extraction failed"):
                _extract(self.root, pkg, self.root / "staging", None)

    def test_explicit_dlc_only_no_startup_marker(self):
        from player_setup import installed_addons
        self.assertEqual(installed_addons(self.root), {})
        self.assertFalse((content_dir(self.root) / "dlc-offered.json").exists())

    def test_overlay_dlc_installer_reports_changed_vs_skipped(self):
        from player_setup import add_dlc_dialog, DLC_INSTALLED_EXIT
        pkg = self.root / "addon.pkg"
        with patch("player_setup._desktop", return_value=True), \
             patch("player_setup.choose_setup", return_value=[pkg]), \
             patch("player_setup.install_dlc", return_value=["COSTUME000000001"]):
            self.assertEqual(add_dlc_dialog(self.root), DLC_INSTALLED_EXIT)
        with patch("player_setup._desktop", return_value=True), \
             patch("player_setup.choose_setup", return_value=[]):
            self.assertEqual(add_dlc_dialog(self.root), 0)

    def test_update_progress_reports_actual_completed_stages(self):
        """No invented byte percentage: update callbacks report completed steps."""
        events = []
        from contextlib import contextmanager
        @contextmanager
        def capture_progress(_root, show_window=True):
            def callback(message, completed, state="working"):
                events.append((message, completed))
            yield callback

        with patch("player_setup.update_progress", side_effect=capture_progress):
            resolve_game(self.root, self.original,
                         update_extracted=self.update, dialogs=False)
        stages = [number for _, number in events]
        self.assertEqual(stages[0], 0)
        self.assertIn(1, stages)
        self.assertIn(2, stages)
        self.assertIn(3, stages)
        self.assertIn(4, stages)
        self.assertEqual(stages[-1], 4)  # successful exit sets 5 in real context
        self.assertEqual(stages, sorted(stages))

    def test_update_in_place_no_second_game_tree(self):
        effective = resolve_game(self.root, self.original,
                                 update_extracted=self.update, dialogs=False)
        self.assertEqual(effective, self.original.resolve())
        self.assertEqual((self.original / "added.dat").read_text(), "new")
        self.assertEqual((self.original / "base-asset.dat").read_text(), "patched")
        self.assertEqual((self.original / "sce_sys/param.sfo").is_file(), True)
        self.assertEqual((self.update / "base-asset.dat").read_text(), "patched")
        self.assertFalse((content_dir(self.root) / "effective-v1.01").exists())
        self.assertEqual(resolve_game(self.root, self.original, dialogs=False), effective)
        self.assertTrue((content_dir(self.root) / "update.json").is_file())
        self.assertFalse((self.original / ".ps4rt-v101-update-incomplete.json").exists())

    def test_reject_same_cusa_different_content_id_before_modification(self):
        (self.update / "sce_sys/param.sfo").write_bytes(sfo({
            "TITLE_ID": "CUSA16429", "APP_VER": "01.01", "CATEGORY": "gp",
            "CONTENT_ID": "UP0177-CUSA16429_00-NEWSAKURAWARSAPP",
        }))
        before = (self.original / "base-asset.dat").read_bytes()
        with self.assertRaisesRegex(ValueError, "CONTENT_ID mismatch"):
            resolve_game(self.root, self.original,
                         update_extracted=self.update, dialogs=False)
        self.assertEqual((self.original / "base-asset.dat").read_bytes(), before)
        self.assertFalse((self.original / ".ps4rt-v101-update-incomplete.json").exists())

    def test_reject_wrong_update_category(self):
        (self.update / "sce_sys/param.sfo").write_bytes(sfo({
            "TITLE_ID": "CUSA16429", "APP_VER": "01.01", "CATEGORY": "ac"
        }))
        with self.assertRaisesRegex(SetupError, "not a game update"):
            resolve_game(self.root, self.original,
                         update_extracted=self.update, dialogs=False)

    def test_reject_wrong_title_without_touching_base(self):
        (self.update / "sce_sys/param.sfo").write_bytes(sfo({
            "TITLE_ID": "CUSA99999", "APP_VER": "01.01", "CATEGORY": "gp"
        }))
        with self.assertRaisesRegex(ValueError, "mismatch"):
            resolve_game(self.root, self.original,
                         update_extracted=self.update, dialogs=False)
        self.assertEqual((self.original / "base-asset.dat").read_text(), "original")

    def test_reject_update_without_replacement_eboot(self):
        (self.update / "eboot.bin").unlink()
        with self.assertRaisesRegex(SetupError, "complete v1.01 eboot.bin"):
            resolve_game(self.root, self.original,
                         update_extracted=self.update, dialogs=False)

    def test_interruption_blocks_boot_and_reapply_repairs_in_place(self):
        from patch_assembly import incomplete_marker
        import os
        original_replace = os.replace

        def fail_once(source, destination):
            if Path(destination).name == "base-asset.dat":
                raise OSError("Simulated power failure during patch replacement")
            return original_replace(source, destination)

        with patch("patch_assembly.os.replace", side_effect=fail_once):
            with self.assertRaisesRegex(OSError, "Simulated power failure"):
                resolve_game(self.root, self.original,
                             update_extracted=self.update, dialogs=False)
        self.assertTrue(incomplete_marker(self.original).is_file())
        with self.assertRaisesRegex(SetupError, "interrupted"):
            resolve_game(self.root, self.original, dialogs=False)
        effective = resolve_game(self.root, self.original,
                                 update_extracted=self.update, dialogs=False)
        self.assertEqual(effective, self.original.resolve())
        self.assertFalse(incomplete_marker(self.original).exists())
        self.assertEqual((self.original / "base-asset.dat").read_text(), "patched")

    def test_wrong_update_does_not_modify_base(self):
        original = (self.original / "base-asset.dat").read_bytes()
        (self.update / "sce_sys/param.sfo").write_bytes(sfo({
            "TITLE_ID": "CUSA99999", "APP_VER": "01.01", "CATEGORY": "gp"
        }))
        with self.assertRaises(ValueError):
            resolve_game(self.root, self.original,
                         update_extracted=self.update, dialogs=False)
        self.assertEqual((self.original / "base-asset.dat").read_bytes(), original)
        self.assertFalse((self.original / ".ps4rt-v101-update-incomplete.json").exists())

    def test_reject_cusa16404_dlc_without_installing(self):
        pkg = self.root / "different-region.pkg"
        pkg.write_bytes(b"fake")
        from player_setup import install_dlc

        def fake_extract(_root, _pkg, output, _tool):
            path = output / "DIFFERENTDLC0001"
            (path / "sce_sys").mkdir(parents=True)
            (path / "sce_sys/param.sfo").write_bytes(sfo({
                "CATEGORY": "ac",
                "CONTENT_ID": "UP0177-CUSA16404_00-DIFFERENTDLC0001"
            }))
            (path / "addon.dat").write_bytes(b"do not install")

        with patch("player_setup._extract", fake_extract):
            with self.assertRaisesRegex(SetupError, "not for CUSA16429"):
                install_dlc(self.root, [pkg], show_window=False)
        self.assertFalse((content_dir(self.root) / "dlc").exists())
        self.assertTrue(pkg.exists())

    def test_multi_dlc_progress_and_duplicate_skip(self):
        pkg1 = self.root / "first.pkg"
        pkg2 = self.root / "second.pkg"
        pkg1.write_bytes(b"1")
        pkg2.write_bytes(b"2")
        events = []
        from contextlib import contextmanager

        @contextmanager
        def capture_progress(_root, count, show_window=True):
            self.assertEqual(count, 2)
            def notify(message, completed, state="working"):
                events.append((completed, message))
            yield notify

        def fake_extract(_root, _pkg, output, _tool):
            # Both PKGs contain the same DLC ID, so the second is skipped.
            path = output / "NEWSAKURADLC0001"
            (path / "sce_sys").mkdir(parents=True)
            (path / "sce_sys/param.sfo").write_bytes(sfo({
                "CATEGORY": "ac",
                "CONTENT_ID": "EP0177-CUSA16429_00-NEWSAKURADLC0001"
            }))
            (path / "costume.dat").write_bytes(b"addon")

        with patch("player_setup._extract", fake_extract), \
             patch("player_setup.dlc_progress", side_effect=capture_progress):
            self.assertEqual(install_dlc(self.root, [pkg1, pkg2],
                                         show_window=False), ["NEWSAKURADLC0001"])
        self.assertEqual([x[0] for x in events], [0, 1, 2, 3, 3, 4, 5, 6])
        self.assertIn("skipped", events[-1][1])

    def test_dlc_staging_and_idempotent_reinstall(self):
        pkg = self.root / "dlc.pkg"
        pkg.write_bytes(b"fake")

        def fake_extract(_root, _pkg, output, _tool):
            path = output / "NEWSAKURADLC0001"
            (path / "sce_sys").mkdir(parents=True)
            (path / "sce_sys/param.sfo").write_bytes(sfo({
                "CATEGORY": "ac",
                "CONTENT_ID": "EP0177-CUSA16429_00-NEWSAKURADLC0001"
            }))
            (path / "costume.dat").write_bytes(b"addon")

        with patch("player_setup._extract", fake_extract):
            self.assertEqual(install_dlc(self.root, [pkg]), ["NEWSAKURADLC0001"])
            self.assertEqual(install_dlc(self.root, [pkg]), [])
        self.assertEqual(set(installed_addons(self.root)), {"NEWSAKURADLC0001"})
        self.assertFalse((self.root / "original" / "costume.dat").exists())


if __name__ == "__main__":
    unittest.main()
