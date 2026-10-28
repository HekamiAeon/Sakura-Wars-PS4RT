from paths import ROOT
from pathlib import Path
import struct
import subprocess
import tempfile
import unittest
from unittest.mock import patch
import content_profile
import shutil

EXE=ROOT/'out/content-test'

class ProfileTests(unittest.TestCase):
    def test_sfo_parameters_and_explicit_trial_profile(self):
        with tempfile.TemporaryDirectory() as tmp:
            p=Path(tmp);(p/'sce_sys').mkdir();(p/'sce_sys/param.sfo').write_bytes(b'fixture')
            with patch.object(content_profile,'sfo',return_value={'USER_DEFINED_PARAM_1':13,'USER_DEFINED_PARAM_4':0xffffffff}):
                content_profile.prepare(p,p,'trial')
            self.assertEqual(struct.unpack('<8s5I',(p/'content.bin').read_bytes()),(b'PS4RTC01',1,13,0,0,0xffffffff))

    def test_invalid_sfo_parameter_rejected(self):
        with tempfile.TemporaryDirectory() as tmp:
            p=Path(tmp);(p/'sce_sys').mkdir();(p/'sce_sys/param.sfo').write_bytes(b'fixture')
            with patch.object(content_profile,'sfo',return_value={'USER_DEFINED_PARAM_1':'13'}):
                with self.assertRaisesRegex(ValueError,'invalid user-defined'):
                    content_profile.prepare(p,p)

    def test_dlc_folders_found_by_content_id(self):
        # On disk under out/, not a RAM-backed temporary directory.
        p=ROOT/'out/test-content-dlc'
        shutil.rmtree(p,ignore_errors=True)
        try:
            for name in ('a/sce_sys','b/Image0/sce_sys','c'):
                (p/name).mkdir(parents=True)
            (p/'a/sce_sys/param.sfo').write_bytes(b'a')
            (p/'b/Image0/sce_sys/param.sfo').write_bytes(b'b')
            ids={b'a':'EP0000-CUSA00000_00-AAAAAAAAAAAAAA01',b'b':'EP0000-CUSA00000_00-AAAAAAAAAAAAAA02'}
            with patch.object(content_profile,'sfo',side_effect=lambda data:{'CONTENT_ID':ids[data]}):
                addons=content_profile.find_addons(p)
            self.assertEqual(addons,{'AAAAAAAAAAAAAA01':(p/'a').resolve(),
                                     'AAAAAAAAAAAAAA02':(p/'b/Image0').resolve()})
            self.assertEqual(content_profile.find_addons(p/'missing'),{})
        finally:
            shutil.rmtree(p,ignore_errors=True)

@unittest.skipUnless(EXE.exists(),'run bash build.sh --test first')
class ContentTests(unittest.TestCase):
    def run_case(self,*args):
        return subprocess.run([str(EXE.resolve()),*args],capture_output=True,text=True,timeout=5)

    def test_lifecycle_and_output_boundaries(self):
        r=self.run_case();self.assertEqual(r.returncode,0,r.stdout+r.stderr)

    def test_missing_profile_does_not_report_loaded(self):
        r=self.run_case('--missing');self.assertEqual(r.returncode,21)
        self.assertIn('needs --content-profile',r.stderr)

    def test_addons_listed_and_mounted(self):
        r=self.run_case('--addons');self.assertEqual(r.returncode,0,r.stdout+r.stderr)

    def test_other_modules_are_counted_independently(self):
        r=self.run_case('--unknown');self.assertEqual(r.returncode,0,r.stdout+r.stderr)
