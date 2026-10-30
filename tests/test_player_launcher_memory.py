"""No-game-data regressions for Sakura Wars remembered launch path."""
from contextlib import ExitStack
from pathlib import Path
import sys
import tempfile
import unittest
from unittest.mock import patch

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'scripts'))
import ps4rt_launch as launcher


class ProcessReplaced(Exception):
    pass


class RememberedGameTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)
        self.game = self.root / 'CUSA16429'
        (self.game / 'sce_sys').mkdir(parents=True)
        (self.game / 'sce_sys/param.sfo').write_bytes(b'mocked game metadata')
        (self.game / 'eboot.bin').write_bytes(b'mocked executable')
        (self.root / 'bin').mkdir()
        (self.root / 'bin/ps4rt').write_bytes(b'mocked runtime')
        self.saved = self.root / 'out/CUSA16429/game-folder.txt'

    def call(self, *argv, version='01.01', update_failure=False,
             prepare_failure=False):
        with ExitStack() as stack:
            stack.enter_context(patch.object(launcher, 'ROOT', self.root))
            stack.enter_context(patch.object(launcher, 'load_sfo', return_value={
                'TITLE_ID': 'CUSA16429', 'APP_VER': version}))
            if update_failure:
                stack.enter_context(patch.object(
                    launcher, 'resolve_game', side_effect=launcher.SetupError('update failed')))
            else:
                stack.enter_context(patch.object(
                    launcher, 'resolve_game', return_value=self.game))
            stack.enter_context(patch.object(launcher, 'prepare'))
            stack.enter_context(patch.object(launcher, 'link'))
            stack.enter_context(patch.object(launcher, 'installed_addons', return_value={}))
            if prepare_failure:
                stack.enter_context(patch.object(
                    launcher, 'prepare_content',
                    side_effect=RuntimeError('preparation failed')))
            else:
                stack.enter_context(patch.object(launcher, 'prepare_content'))
            stack.enter_context(patch.object(launcher.os, 'execve', side_effect=ProcessReplaced))
            return launcher.main(list(argv))

    def test_first_launch_requires_explicit_game(self):
        with self.assertRaisesRegex(launcher.SetupError, 'Play-Game.sh --game'):
            self.call()
        self.assertFalse(self.saved.exists())

    def test_first_launch_remembers_and_next_launch_reuses(self):
        with self.assertRaises(ProcessReplaced):
            self.call('--game', str(self.game))
        self.assertEqual(self.saved.read_text().strip(), str(self.game.resolve()))
        with self.assertRaises(ProcessReplaced):
            self.call()

    def test_prepare_only_remembers_without_game_start(self):
        self.assertEqual(self.call('--game', str(self.game), '--prepare-only'), 0)
        self.assertEqual(self.saved.read_text().strip(), str(self.game.resolve()))

    def test_explicit_other_location_replaces_remembered_only_on_success(self):
        self.saved.parent.mkdir(parents=True)
        self.saved.write_text(str(self.game))
        other = self.root / 'other' / 'CUSA16429'
        (other / 'sce_sys').mkdir(parents=True)
        (other / 'sce_sys/param.sfo').write_bytes(b'mocked SFO')
        with self.assertRaises(ProcessReplaced):
            self.call('--game', str(other))
        self.assertEqual(self.saved.read_text().strip(), str(other.resolve()))

    def test_explicit_missing_folder_keeps_previous_path(self):
        self.saved.parent.mkdir(parents=True)
        self.saved.write_text(str(self.game))
        with self.assertRaisesRegex(launcher.SetupError, 'Game folder does not exist'):
            self.call('--game', str(self.root / 'missing'))
        self.assertEqual(self.saved.read_text().strip(), str(self.game))

    def test_stale_remembered_folder_gives_actionable_error(self):
        self.saved.parent.mkdir(parents=True)
        self.saved.write_text(str(self.root / 'moved-CUSA16429'))
        with self.assertRaisesRegex(launcher.SetupError, 'Play-Game.sh --game'):
            self.call()

    def test_failed_preparation_preserves_previous_location(self):
        self.saved.parent.mkdir(parents=True)
        self.saved.write_text('/previous/CUSA16429\n')
        with self.assertRaisesRegex(RuntimeError, 'preparation failed'):
            self.call('--game', str(self.game), prepare_failure=True)
        self.assertEqual(self.saved.read_text(), '/previous/CUSA16429\n')

    def test_failed_update_does_not_remember_folder(self):
        with self.assertRaisesRegex(launcher.SetupError, 'update failed'):
            self.call('--game', str(self.game), version='01.00', update_failure=True)
        self.assertFalse(self.saved.exists())

    def test_failed_update_does_not_replace_existing_folder(self):
        self.saved.parent.mkdir(parents=True)
        self.saved.write_text('/valid/previous/CUSA16429\n')
        with self.assertRaisesRegex(launcher.SetupError, 'update failed'):
            self.call('--game', str(self.game), version='01.00', update_failure=True)
        self.assertEqual(self.saved.read_text(), '/valid/previous/CUSA16429\n')


if __name__ == '__main__':
    unittest.main()
