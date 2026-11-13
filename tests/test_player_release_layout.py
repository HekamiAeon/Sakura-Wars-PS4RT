"""Static packaging safeguards for the portable player and source exporter."""
from pathlib import Path
import importlib.util
import re
import shutil
import subprocess
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

    def test_player_scripts_cover_every_launcher_import(self):
        """The portable must ship each local module the launcher can import,
        including deferred imports such as the trophy catalog and its crypto."""
        import ast
        pending, seen = ['ps4rt_launch'], set()
        while pending:
            name = pending.pop()
            if name in seen:
                continue
            seen.add(name)
            tree = ast.parse((ROOT / 'scripts' / f'{name}.py').read_text(encoding='utf-8'))
            for node in ast.walk(tree):
                modules = ([alias.name for alias in node.names] if isinstance(node, ast.Import)
                           else [node.module] if isinstance(node, ast.ImportFrom) and node.module
                           else [])
                pending += [m for m in modules if (ROOT / 'scripts' / f'{m}.py').is_file()]
        self.assertIn('trophy_catalog', seen)
        self.assertIn('trophy_crypto', seen)
        for name in seen:
            self.assertIn(f'scripts/{name}.py', release.PLAYER, name)
            self.assertTrue(release.source_allowed(f'scripts/{name}.py'), name)

    def test_private_user_files_cannot_be_exported(self):
        for name in ('out/CUSA16429/user/savedata/secret.dat',
                     'out/CUSA16429/ps4rt.ini',
                     'out/CUSA16429/user/console.ini',
                     'out/CUSA16429/user/gpu/screenshots/CUSA16429_20261109_213047_023.png',
                     'out/CUSA16429/user/achievements/1/NPWR17587_00.dat',
                     'game-dumps/CUSA16429/eboot.bin',
                     'docs/THIRD_PARTY_REVIEW.md',
                     'gpu/assets/private-background.png'):
            self.assertFalse(release.source_allowed(name), name)

    def test_source_requires_credits_and_license_but_not_the_readme(self):
        self.assertIn('LICENSE', release.REQUIRED)
        self.assertIn('docs/UPSTREAM_CREDITS.md', release.REQUIRED)
        self.assertNotIn('README.md', release.REQUIRED)


def git(*args):
    return subprocess.run(['git', '-C', str(ROOT), *args], capture_output=True, check=True).stdout


def export_ignored(paths):
    """Whether this checkout's .gitattributes leaves each path out of `git archive`."""
    fields = git('check-attr', '-z', 'export-ignore', '--', *paths).split(b'\0')
    return {fields[i].decode(): fields[i + 2] == b'set' for i in range(0, len(fields) - 2, 3)}


def tracked(*paths):
    return [p.decode() for p in git('ls-files', '-z', '--', *paths).split(b'\0') if p]


# Project documentation: kept out of source archives, except the credits and docs/licenses/.
PROJECT_DOCS = ('README.md', 'docs', 'packaging/linux/README.md')


def project_doc(path):
    return path in PROJECT_DOCS or path.startswith('docs/')


@unittest.skipUnless(shutil.which('git') and (ROOT / '.git').exists(), 'needs a Git checkout')
class SourceArchiveDocsPolicyTests(unittest.TestCase):
    """Project docs stay out of GitHub's generated archives and the release.py source export
    (both use git archive), so they can change after a release is built."""

    def test_player_docs_are_left_out_of_source_archives(self):
        docs = [p for p in tracked(*PROJECT_DOCS)
                if p != 'docs/UPSTREAM_CREDITS.md' and not p.startswith('docs/licenses/')]
        self.assertIn('README.md', docs)
        self.assertIn('packaging/linux/README.md', docs)
        for path, ignored in export_ignored(docs).items():
            self.assertTrue(ignored, path)

    def test_credits_licenses_and_notices_stay_in_source_archives(self):
        notice = re.compile(r'(^|/)(license|notice|copying|readme)[^/]*$', re.IGNORECASE)
        keep = ['docs/UPSTREAM_CREDITS.md', 'docs/licenses', 'docs/licenses/GPL-3.0.txt',
                'docs/licenses/nested/NOTICE.txt',
                *(p for p in tracked() if notice.search(p) and not project_doc(p))]
        self.assertIn('LICENSE', keep)
        self.assertIn('third_party/ps4pkg/README.md', keep)
        for path, ignored in export_ignored(keep).items():
            self.assertFalse(ignored, path)

    def test_required_source_is_never_left_out(self):
        for path, ignored in export_ignored(sorted(release.REQUIRED)).items():
            self.assertFalse(ignored, path)


if __name__ == '__main__':
    unittest.main()
