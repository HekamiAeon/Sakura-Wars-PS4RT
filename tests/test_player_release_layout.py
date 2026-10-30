"""Static packaging safeguards for the portable player and source exporter."""
from pathlib import Path
import importlib.util
import unittest

ROOT = Path(__file__).resolve().parents[1]
spec = importlib.util.spec_from_file_location('sakura_release',
                                              ROOT / 'packaging/linux/release.py')
release = importlib.util.module_from_spec(spec)
spec.loader.exec_module(release)


class ReleaseLayoutTests(unittest.TestCase):
    def test_crypto_build_recipe_is_source_allowed(self):
        self.assertTrue(release.source_allowed('packaging/linux/Containerfile.pkg'))
        self.assertFalse(release.source_allowed('packaging/linux/other.pkg'))
        self.assertFalse(release.source_allowed('game/CUSA16429/update.pkg'))

    def test_no_player_docs_or_asset_readme(self):
        self.assertFalse(any(p.startswith('docs/') for p in release.PLAYER))
        self.assertNotIn('gpu/assets/README.md', release.MEDIA)
        self.assertFalse(release.source_allowed('gpu/assets/README.md'))
        self.assertNotIn('docs/THIRD_PARTY_REVIEW.md', release.PUBLIC_DOCS)

    def test_required_media_is_reviewed_and_allowlisted(self):
        for name in ('gpu/assets/sakura_wars_overlay.png',
                     'gpu/assets/sounds/open.wav',
                     'gpu/assets/sounds/close.wav',
                     'gpu/assets/sounds/scroll.wav'):
            self.assertIn(name, release.MEDIA)
            self.assertTrue(release.source_allowed(name))

    def test_private_user_files_cannot_be_exported(self):
        for name in ('out/CUSA16429/user/savedata/secret.dat',
                     'out/CUSA16429/ps4rt.ini',
                     'game-dumps/CUSA16429/eboot.bin',
                     'docs/THIRD_PARTY_REVIEW.md',
                     'gpu/assets/private-background.png'):
            self.assertFalse(release.source_allowed(name), name)


if __name__ == '__main__':
    unittest.main()
