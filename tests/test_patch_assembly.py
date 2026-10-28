"""Offline tests for patch-aware PS4RT game assembly; no copyrighted data needed."""
import importlib.util
import json
from pathlib import Path
import tempfile
import unittest


PATCH_ASSEMBLY = Path(__file__).resolve().parents[1] / 'scripts/patch_assembly.py'
if not PATCH_ASSEMBLY.is_file():
    raise unittest.SkipTest('patch assembly tool is not shipped in this repository')

SPEC = importlib.util.spec_from_file_location('patch_assembly', PATCH_ASSEMBLY)
overlay = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(overlay)


def sfo(path):
    return json.loads(path.read_text())


def fixture(root, name, title='CUSA16429', category='gd', version='01.00'):
    p = root / name
    (p / 'sce_sys').mkdir(parents=True)
    (p / 'sce_sys/param.sfo').write_text(json.dumps(
        {'TITLE_ID': title, 'CATEGORY': category, 'APP_VER': version,
         'CONTENT_ID': f'EP0177-{title}_00-NEWSAKURAWARSAPP'}
    ))
    return p


class PatchAssemblyTests(unittest.TestCase):
    def setUp(self):
        tmp = tempfile.TemporaryDirectory()
        self.addCleanup(tmp.cleanup)
        self.root = Path(tmp.name)
        self.base = fixture(self.root, 'base')
        self.patch = fixture(self.root, 'patch', category='gp', version='01.01')
        (self.base / 'eboot.bin').write_bytes(b'base-eboot')
        (self.patch / 'eboot.bin').write_bytes(b'patch-eboot')
        (self.base / 'music.dat').write_bytes(b'keep')
        (self.patch / 'new.dat').write_bytes(b'added')

    def test_overlay_replaces_and_preserves(self):
        report = overlay.assemble(
            self.base, self.patch, self.root / 'out', sfo, 'CUSA16429',
            report_path=self.root / 'report.json'
        )
        self.assertEqual(report['effective_file_count'], 4)
        self.assertEqual((self.root / 'out/eboot.bin').read_bytes(), b'patch-eboot')
        self.assertEqual((self.root / 'out/music.dat').read_bytes(), b'keep')
        self.assertEqual((self.root / 'out/new.dat').read_bytes(), b'added')
        self.assertEqual((self.base / 'eboot.bin').read_bytes(), b'base-eboot')
        self.assertEqual(json.loads((self.root / 'report.json').read_text())['title_id'], 'CUSA16429')

    def test_observed_ep_and_up_game_content_ids(self):
        # Real game IDs observed from Star Ocean 5 and Valkyria Revolution.
        examples = [
            ("CUSA03219", "EP0082-CUSA03219_00-STAROCEAN5PS4000"),
            ("CUSA06810", "UP0177-CUSA06810_00-ARVALKYRIE000100"),
        ]
        for idx, (title, content_id) in enumerate(examples):
            with self.subTest(title=title):
                game_root = fixture(self.root, f"other-base-{idx}",
                                    title=title, category="gd", version="01.00")
                patch_root = fixture(self.root, f"other-patch-{idx}",
                                     title=title, category="gp", version="01.01")
                for directory, category in ((game_root, "gd"), (patch_root, "gp")):
                    (directory / "sce_sys/param.sfo").write_text(json.dumps({
                        "TITLE_ID": title,
                        "CONTENT_ID": content_id,
                        "CATEGORY": category,
                        "APP_VER": "01.00" if category == "gd" else "01.01",
                    }))
                    (directory / "eboot.bin").write_bytes(category.encode("ascii"))
                report = overlay.check_pair(game_root, patch_root, sfo, title)
                self.assertEqual(report["content_id"], content_id)

    def test_rejects_same_cusa_different_region_content_id(self):
        original = (self.base / 'eboot.bin').read_bytes()
        (self.patch / 'sce_sys/param.sfo').write_text(json.dumps({
            'TITLE_ID': 'CUSA16429', 'CATEGORY': 'gp', 'APP_VER': '01.01',
            'CONTENT_ID': 'UP0177-CUSA16429_00-NEWSAKURAWARSAPP',
        }))
        with self.assertRaisesRegex(overlay.PatchError, 'CONTENT_ID mismatch'):
            overlay.assemble(self.base, self.patch, self.root / 'out', sfo)
        self.assertFalse((self.root / 'out').exists())
        self.assertEqual((self.base / 'eboot.bin').read_bytes(), original)

    def test_rejects_missing_update_content_id(self):
        (self.patch / 'sce_sys/param.sfo').write_text(json.dumps({
            'TITLE_ID': 'CUSA16429', 'CATEGORY': 'gp', 'APP_VER': '01.01',
        }))
        with self.assertRaisesRegex(overlay.PatchError, 'supply CONTENT_ID'):
            overlay.check_pair(self.base, self.patch, sfo)

    def test_collision_refuses_overwrite(self):
        (self.root / 'out').mkdir()
        with self.assertRaisesRegex(overlay.PatchError, 'already exists'):
            overlay.assemble(self.base, self.patch, self.root / 'out', sfo)

    def test_mismatched_cusa_fails_without_writing(self):
        (self.patch / 'sce_sys/param.sfo').write_text(json.dumps(
            {'TITLE_ID': 'CUSA99999', 'CATEGORY': 'gp'}
        ))
        with self.assertRaisesRegex(overlay.PatchError, 'mismatch'):
            overlay.assemble(self.base, self.patch, self.root / 'out', sfo)
        self.assertFalse((self.root / 'out').exists())

    def test_rejects_dlc_in_update_slot(self):
        (self.patch / 'sce_sys/param.sfo').write_text(json.dumps(
            {'TITLE_ID': 'CUSA16429', 'CATEGORY': 'ac'}
        ))
        with self.assertRaisesRegex(overlay.PatchError, 'DLC'):
            overlay.check_pair(self.base, self.patch, sfo)

    def test_asset_only_patch_keeps_base_eboot(self):
        (self.patch / 'eboot.bin').unlink()
        report = overlay.assemble(self.base, self.patch, self.root / 'out', sfo)
        self.assertEqual((self.root / 'out/eboot.bin').read_bytes(), b'base-eboot')
        self.assertIn('does not replace eboot', ' '.join(report['warnings']))

    def test_unsafe_symlink_rejected(self):
        (self.patch / 'bad-link').symlink_to('/etc/passwd')
        with self.assertRaisesRegex(overlay.PatchError, 'Unsupported filesystem entry'):
            overlay.assemble(self.base, self.patch, self.root / 'out', sfo)
        self.assertFalse((self.root / 'out').exists())

    def test_extract_subdirectory_detected(self):
        extracted = self.root / 'extracted'
        (extracted / 'CUSA16429-patch/sce_sys').mkdir(parents=True)
        (extracted / 'CUSA16429-patch/sce_sys/param.sfo').write_bytes(b'dummy')
        self.assertEqual(overlay.find_patch_root(extracted),
                         extracted / 'CUSA16429-patch')

    def test_existing_report_rejected_without_copy(self):
        (self.root / 'report.json').write_text('do not overwrite')
        with self.assertRaisesRegex(overlay.PatchError, 'Report already exists'):
            overlay.assemble(
                self.base, self.patch, self.root / 'out', sfo,
                report_path=self.root / 'report.json'
            )
        self.assertFalse((self.root / 'out').exists())


if __name__ == '__main__':
    unittest.main()
