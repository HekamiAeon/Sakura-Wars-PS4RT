"""Boundary tests for the native loader; uses tiny synthetic x86-64 images."""
from paths import ROOT
from pathlib import Path
import os
import struct
import subprocess
import sys
import tempfile
import unittest
EXE = Path(os.environ.get('PS4RT_EXECUTABLE', ROOT / 'out/ps4rt'))
HOST_EXE = Path(os.environ.get('PS4RT_EXECUTABLE', ROOT / ('out/ps4rt' + ('.exe' if sys.platform == 'win32' else ''))))

def package(code, relocs=(), names=(), capabilities=None):
    image = code.ljust(4096, b'\x00')
    header = struct.pack('<8sQQQQQ', b'PS4RTB01' if capabilities is None else b'PS4RTB02', len(image), 0, 1, len(relocs), len(names))
    if capabilities is not None:
        header += struct.pack('<Q', capabilities)
    segment = struct.pack('<QQQ', 0, len(image), 5)
    return header + segment + b''.join((n.encode().ljust(128, b'\x00') for n in names)) + b''.join((struct.pack('<QQqq', *r) for r in relocs)) + image

def native_package(name='fixture-native', binding_address=4112, binding_kind=1, lib_flags=5, init=b'1\xc0\xc3', native=b'\xc3', metadata=None):
    code = b'H\x83\xec\x08\xff\x15\x16\x00\x00\x00H\x83\xc4\x08\xff%\x14\x00\x00\x00'
    image = code.ljust(4096, b'\x00') + init.ljust(16, b'\x00') + native
    image = image.ljust(12288, b'\x00')
    header = struct.pack('<8s6Q', b'PS4RTB03', len(image), 0, 3, 2, 2, 1)
    meta = metadata or (4096, 8192, 4096, 8192, 32, 4, 1, 256)
    return header + struct.pack('<8Q', *meta) + struct.pack('<3Q', 0, binding_address, binding_kind) + struct.pack('<9Q', 0, 4096, 5, 4096, 4096, lib_flags, 8192, 4096, 6) + name.encode().ljust(128, b'\x00') + b'after-native'.ljust(128, b'\x00') + struct.pack('<8Q', 32, 1, 0, 0, 40, 1, 1, 0) + image

@unittest.skipUnless(EXE.exists(), 'build native loader first')
class LoaderTests(unittest.TestCase):

    def test_invalid_content_profile_is_rejected(self):
        with tempfile.TemporaryDirectory() as tmp:
            path = Path(tmp) / 'content.bin'
            for data in (b'PS4RTC01', struct.pack('<8s5I', b'PS4RTC01', 2, 0, 0, 0, 0), struct.pack('<8s5I', b'PS4RTC01', 3, 0, 0, 0, 0) + b'extra'):
                with self.subTest(data=data):
                    path.write_bytes(data)
                    r = self.run_image(package(b'\xc3'), '--content-profile', str(path))
                    self.assertEqual(r.returncode, 1, r.stdout + r.stderr)
                    self.assertIn('content profile', r.stderr)

    def run_image(self, data, *options):
        with tempfile.TemporaryDirectory() as tmp:
            path = Path(tmp) / 'boot.bin'
            path.write_bytes(data)
            return subprocess.run([str(EXE.resolve()), str(path), '--cpu-only', *options], capture_output=True, text=True, timeout=5)

    def test_original_instruction_reaches_named_import(self):
        r = self.run_image(package(b'\xff%\x02\x00\x00\x00\x90\x90', [(8, 1, 0, 0)], ['fixture-import']))
        self.assertEqual(r.returncode, 20, r.stderr)
        self.assertIn('first unsupported PS4 import: fixture-import', r.stdout)

    def test_no_guest_sanitizer_allocator_is_installed(self):
        # Call the query; a non-NULL replacement table branches to UD2.
        code = bytes.fromhex('4883ec08 ff1536000000 4885c0 750a 4883c408 ff252f000000 0f0b')
        data = package(code, [(64, 1, 0, 0), (72, 1, 1, 0)],
                       ['py6L8jiVAN8#libkernel', 'after-sanitizer-query'], 1)
        r = self.run_image(data)
        self.assertEqual(r.returncode, 20, r.stdout + r.stderr)
        self.assertIn('first unsupported PS4 import: after-sanitizer-query', r.stdout)

    def test_native_initializer_and_export_return(self):
        r = self.run_image(native_package())
        self.assertEqual(r.returncode, 20, r.stdout + r.stderr)
        self.assertIn('Module 0 initializer returned 0', r.stdout)
        self.assertIn('first unsupported PS4 import: after-native', r.stdout)

    def test_host_contract_takes_priority_over_native_export(self):
        r = self.run_image(native_package(name='bzQExy189ZI#libc', native=b'\x0f\x0b'))
        self.assertEqual(r.returncode, 20, r.stdout + r.stderr)
        self.assertIn('_init_env returned', r.stdout)
        self.assertIn('first unsupported PS4 import: after-native', r.stdout)

    def test_strict_mode_skips_native_initialization_and_binding(self):
        r = self.run_image(native_package(init=b'\x0f\x0b'), '--strict-imports')
        self.assertEqual(r.returncode, 20, r.stdout + r.stderr)
        self.assertIn('first unsupported PS4 import: fixture-native', r.stdout)
        self.assertNotIn('Starting native libc', r.stdout)

    def test_native_metadata_and_bindings_are_validated(self):
        for kwargs, message in [({'binding_address': 12288}, 'invalid native binding'), ({'binding_kind': 2}, 'native export kind/range mismatch'), ({'lib_flags': 4}, 'native function is not executable'), ({'metadata': (4096, 8192, 4096, 8192, 32, 33, 1, 256)}, 'invalid linked module metadata'), ({'metadata': (4096, 8192, 4096, 8192, 32, 4, 1, 12280)}, 'unmapped procparam')]:
            with self.subTest(kwargs=kwargs):
                r = self.run_image(native_package(**kwargs))
                self.assertEqual(r.returncode, 1, r.stdout + r.stderr)
                self.assertIn(message, r.stderr)

    def test_failed_native_initializer_does_not_enter_game(self):
        r = self.run_image(native_package(init=b'\xb8\x01\x00\x00\x00\xc3'))
        self.assertEqual(r.returncode, 1, r.stdout + r.stderr)
        self.assertIn('module initializer failed', r.stderr)
        self.assertNotIn('Entering original', r.stdout)

    def test_base_relative_address_is_relocated(self):
        code = b'\xff%\x02\x00\x00\x00\x90\x90' + b'\x00' * 8 + b'\xff%\x02\x00\x00\x00\x90\x90'
        r = self.run_image(package(code, [(8, 0, 16, 0), (24, 1, 0, 0)], ['after-relative-jump']))
        self.assertEqual(r.returncode, 20, r.stderr)
        self.assertIn('after-relative-jump', r.stdout)

    def test_out_of_bounds_relocation_is_rejected(self):
        r = self.run_image(package(b'\xc3', [(4092, 0, 0, 0)]))
        self.assertEqual(r.returncode, 1)
        self.assertIn('bad relocation', r.stderr)

    def test_truncated_image_is_rejected(self):
        r = self.run_image(package(b'\xc3')[:-1])
        self.assertEqual(r.returncode, 1)
        self.assertIn('incorrect memory image size', r.stderr)

    def test_illegal_instruction_reports_guest_offset(self):
        r = self.run_image(package(b'\x0f\x0b'))
        self.assertEqual(r.returncode, 132)
        self.assertIn('at guest offset 0x0,', r.stderr)

    def test_watchdog_reports_guest_stack_before_exit(self):
        r = self.run_image(package(bytes.fromhex('ebfe')), '--timeout', '1')
        self.assertEqual(r.returncode, 142, r.stdout + r.stderr)
        self.assertIn('STOP: watchdog timeout; thread stacks:', r.stderr)
        self.assertIn('guest+0x0', r.stderr)

    def test_runtime_returns_from_verified_init_env(self):
        code = b'H\x83\xec\x08\xff\x15\x16\x00\x00\x00H\x83\xc4\x08\xff%\x14\x00\x00\x00'
        data = package(code, [(32, 1, 0, 0), (40, 1, 1, 0)], ['bzQExy189ZI#libc', 'after-init'], 1)
        r = self.run_image(data)
        self.assertEqual(r.returncode, 20, r.stderr)
        self.assertIn('first unsupported PS4 import: after-init', r.stdout)
        self.assertIn('_init_env=1', r.stdout)
        strict = self.run_image(data, '--strict-imports')
        self.assertEqual(strict.returncode, 20, strict.stderr)
        self.assertIn('first unsupported PS4 import: bzQExy189ZI#libc', strict.stdout)

    def test_check_imports_lists_missing_without_running(self):
        code = b'\x0f\x0b'
        data = package(code, [(32, 1, 0, 0), (40, 1, 1, 0)], ['bzQExy189ZI#libc', 'missing#libSceFixture'], 1)
        r = self.run_image(data, '--check-imports')
        self.assertEqual(r.returncode, 1, r.stderr)
        self.assertIn('MISSING: missing#libSceFixture', r.stdout)
        self.assertNotIn('bzQExy189ZI', r.stdout)
        ok = self.run_image(package(code, [(32, 1, 0, 0)], ['bzQExy189ZI#libc'], 1), '--check-imports')
        self.assertEqual(ok.returncode, 0, ok.stderr)
        self.assertIn('Imports: 0 missing', ok.stdout)

    def test_unverified_runtime_stays_disabled(self):
        r = self.run_image(package(b'\xff%\x02\x00\x00\x00\x90\x90', [(8, 1, 0, 0)], ['bzQExy189ZI#libc'], 0))
        self.assertEqual(r.returncode, 20, r.stderr)
        self.assertIn('_init_env=0', r.stdout)

    def test_unknown_capabilities_rejected(self):
        r = self.run_image(package(b'\xc3', capabilities=2))
        self.assertEqual(r.returncode, 1)
        self.assertIn('unknown runtime capabilities', r.stderr)

def host_env():
    env = dict(os.environ)
    if sys.platform == 'win32' and (not (HOST_EXE.parent / 'SDL3.dll').is_file()):
        clang64 = Path(env.get('MSYS2_ROOT', 'C:\\msys64')) / 'clang64' / 'bin'
        env['PATH'] = f"{clang64}{os.pathsep}{env.get('PATH', '')}"
    return env
if __name__ == '__main__':
    unittest.main()
