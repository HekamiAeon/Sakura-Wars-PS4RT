"""FSR4 asset integrity and release validation regressions."""
import importlib.util
import json
from pathlib import Path
import unittest

ROOT = Path(__file__).resolve().parents[1]
SCRIPT = ROOT / "packaging/linux/release.py"

spec = importlib.util.spec_from_file_location("sakura_release", SCRIPT)
release = importlib.util.module_from_spec(spec)
spec.loader.exec_module(release)


class FSR4AssetTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.source = {
            f"fsr4_shaders/{p.name}": (p.read_bytes(), 0o644)
            for p in (ROOT / "fsr4_shaders").iterdir()
            if p.is_file()
        }
        cls.lock_path = "packaging/fsr4-v07-assets.json"
        cls.source[cls.lock_path] = (
            (ROOT / cls.lock_path).read_bytes(), 0o644
        )
        cls.lock = json.loads(cls.source[cls.lock_path][0])
        cls.target = (
            "fsr4_shaders/"
            "fsr4_model_v07_i8_quality_4320_pass12.spv"
        )

    def test_complete_inventory(self):
        self.assertEqual(
            set(self.lock["files"]),
            set(self.source) - {self.lock_path}
        )
        self.assertEqual(
            self.lock["output_tiers"], ["1080", "2160", "4320"]
        )
        release.verify_fsr4_assets(self.source)

    def test_corrupted_shader_rejected(self):
        source = dict(self.source)
        source[self.target] = (b"tampered", 0o644)
        with self.assertRaises(SystemExit):
            release.verify_fsr4_assets(source)

    def test_missing_shader_rejected(self):
        source = dict(self.source)
        del source[self.target]
        with self.assertRaises(SystemExit):
            release.verify_fsr4_assets(source)

    def test_unexpected_shader_rejected(self):
        source = dict(self.source)
        source["fsr4_shaders/unreviewed.spv"] = (b"extra", 0o644)
        with self.assertRaises(SystemExit):
            release.verify_fsr4_assets(source)

    def test_incomplete_tier_rejected(self):
        source = dict(self.source)
        lock = json.loads(source[self.lock_path][0])
        del lock["files"][self.target]
        del source[self.target]
        source[self.lock_path] = (
            json.dumps(lock).encode(), 0o644
        )
        with self.assertRaises(SystemExit):
            release.verify_fsr4_assets(source)


if __name__ == "__main__":
    unittest.main()
