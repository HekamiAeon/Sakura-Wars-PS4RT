# SPDX-License-Identifier: GPL-2.0-or-later
import importlib.util
from pathlib import Path
import struct
import sys
import tempfile
import unittest
from unittest.mock import patch

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'scripts'))
import trophy_catalog as catalog
import ps4rt_launch as launcher
from trophy_crypto import _words, _encrypt_block, _decrypt_block, _TROPHY_KEY


def encrypted(text, identity):
    schedule = _words(_encrypt_block(identity.encode().ljust(16, b'\0'), _words(_TROPHY_KEY)))
    data = text.encode()
    pad = 16 - len(data) % 16
    data += bytes([pad]) * pad
    previous = bytes(range(16))
    out = previous
    for pos in range(0, len(data), 16):
        previous = _encrypt_block(bytes(a ^ b for a, b in zip(data[pos:pos + 16], previous)), schedule)
        out += previous
    return out


def trp(files):
    offset = 96 + len(files) * 64
    table, payload = b'', b''
    for name, data in files.items():
        table += name.encode().ljust(32, b'\0') + struct.pack('>QQ', offset, len(data)) + bytes(16)
        payload += data
        offset += len(data)
    return struct.pack('>IIQII', 0xDCA24D00, 3, offset, len(files), 64).ljust(96, b'\0') + table + payload


def npbind(identity, number=0):
    fields = b''
    for kind, data in ((0x10, identity.encode()), (0x11, str(number).encode().ljust(12, b'\0')),
                       (0x12, bytes(176)), (0x13, bytes(16))):
        fields += struct.pack('>HH', kind, len(data)) + data
    body = fields.ljust(384, b'\0')
    return struct.pack('>IIQQQ', 0xD294A018, 1, 128 + len(body), len(body), 1).ljust(128, b'\0') + body


class TrophyCatalogTest(unittest.TestCase):
    def test_trophy_text_follows_console_language_without_touching_saves(self):
        """Sakura Wars reads the console language; no game save selects trophy text."""
        with tempfile.TemporaryDirectory() as directory:
            base = Path(directory)
            user = base / 'user'
            save = user / 'savedata/1/CUSA16429/SYSTEM/fixture.bin'
            save.parent.mkdir(parents=True)
            save.write_bytes(bytes(range(256)))
            stamp = save.stat().st_mtime_ns
            for system, expected in (('0', 0), ('1', 1), ('3', 3), ('20', 20)):
                env = {'PS4RT_LANGUAGE': system}
                with patch.object(catalog, 'prepare_catalog', return_value=None) as prepare:
                    launcher._achievements('CUSA16429', base / 'game', user, env)
                self.assertEqual(env['PS4RT_TROPHY_LANGUAGE'], str(expected))
                self.assertEqual(prepare.call_args.args[2], expected)
                self.assertEqual(env['PS4RT_ACHIEVEMENTS_DIR'], str(user / 'achievements'))
                self.assertNotIn('PS4RT_TROPHY_CATALOG', env)
            self.assertEqual(catalog.trophy_language('CUSA16429', user, 99), 1)
            self.assertEqual(catalog.trophy_language('CUSA16429', user, -1), 1)
            self.assertEqual(save.read_bytes(), bytes(range(256)))
            self.assertEqual(save.stat().st_mtime_ns, stamp)

    def test_missing_or_damaged_metadata_never_blocks_launch(self):
        with tempfile.TemporaryDirectory() as directory:
            base = Path(directory)
            game, user = base / 'game', base / 'user'
            (game / 'sce_sys').mkdir(parents=True)
            # A stale catalog from an earlier launch must not leak into this one.
            env = {'PS4RT_TROPHY_CATALOG': '/stale/catalog.tsv'}
            launcher._achievements('CUSA16429', game, user, env)
            self.assertNotIn('PS4RT_TROPHY_CATALOG', env)
            (game / 'sce_sys/npbind.dat').write_bytes(b'damaged')
            launcher._achievements('CUSA16429', game, user, env)
            self.assertNotIn('PS4RT_TROPHY_CATALOG', env)
            (game / 'sce_sys/npbind.dat').write_bytes(npbind('NPWR99999_00'))
            launcher._achievements('CUSA16429', game, user, env)  # archive missing
            self.assertNotIn('PS4RT_TROPHY_CATALOG', env)
            env['PS4RT_LANGUAGE'] = 'not-a-number'
            launcher._achievements('CUSA16429', game, user, env)
            self.assertNotIn('PS4RT_TROPHY_CATALOG', env)

    def test_aes_known_answer(self):
        key = _words(bytes.fromhex('000102030405060708090a0b0c0d0e0f'))
        plain = bytes.fromhex('00112233445566778899aabbccddeeff')
        cipher = bytes.fromhex('69c4e0d86a7b0430d8cdb78070b4c55a')
        self.assertEqual(_encrypt_block(plain, key), cipher)
        self.assertEqual(_decrypt_block(cipher, key), plain)

    def test_binding_id_and_archive_are_read_from_metadata(self):
        self.assertEqual(catalog.bindings(npbind('NPWR99999_00', 7)), [(0, 'NPWR99999_00', 7)])
        for damaged in (b'', npbind('NPWR99999_00')[:-1], npbind('../bad')):
            with self.assertRaises(ValueError):
                catalog.bindings(damaged)

    def test_language_hidden_grade_and_icons_without_game_data(self):
        identity = 'NPWR99999_00'
        config = '<trophyconf><trophy id="002" ttype="B" hidden="yes" pid="-1" gid="003"/></trophyconf>'
        def text(title):
            return f'<trophyconf><title-name>Fixture</title-name><trophy id="002"><name>{title}</name><detail>A description</detail></trophy></trophyconf>'
        png = b'\x89PNG\r\n\x1a\nfixture icon bytes'
        files = {'TROPCONF.ESFM': encrypted(config, identity), 'TROP.ESFM': encrypted(text('日本語'), identity),
                 'TROP_01.ESFM': encrypted(text('English'), identity), 'TROP002.PNG': png}
        with tempfile.TemporaryDirectory() as directory:
            base = Path(directory)
            game, user = base / 'game', base / 'user'
            trophy = game / 'sce_sys/trophy'
            trophy.mkdir(parents=True)
            binding = game / 'sce_sys/npbind.dat'
            binding.write_bytes(npbind(identity))
            source = trp(files)
            (trophy / 'trophy00.trp').write_bytes(source)
            english = catalog.prepare_catalog(game, user, 1)
            self.assertIn('2\tB\t-1\t3\t1\tEnglish\tA description', english.read_text())
            self.assertEqual((english.parent / identity / '002.png').read_bytes(), png)
            stamp = english.stat().st_mtime_ns
            self.assertEqual(catalog.prepare_catalog(game, user, 1), english)
            self.assertEqual(english.stat().st_mtime_ns, stamp)
            default = catalog.prepare_catalog(game, user, 0)
            self.assertIn('日本語', default.read_text())
            fallback = catalog.prepare_catalog(game, user, 9)
            self.assertIn('日本語', fallback.read_text())
            self.assertNotEqual(english, default)
            self.assertEqual((trophy / 'trophy00.trp').read_bytes(), source)
            self.assertEqual(binding.read_bytes(), npbind(identity))

    def test_archive_bounds_duplicates_paths_and_invalid_xml(self):
        source = trp({'TROP.SFM': b'<trophyconf/>'})
        for damaged in (b'', source[:-1], source[:96] + bytes(64),
                        trp({'../TROP.SFM': b'<trophyconf/>'})):
            with self.assertRaises(ValueError):
                catalog.entries(damaged)
        with self.assertRaises(ValueError):
            catalog.xml(b'not XML', 'NPWR99999_00', False)
        with self.assertRaises(ValueError):
            catalog.xml(encrypted('<wrong/>', 'NPWR99999_00'), 'NPWR99999_00')

    def test_no_trophy_files_is_optional(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            self.assertIsNone(catalog.prepare_catalog(root, root / 'user'))
            self.assertFalse((root / 'user').exists())


if __name__ == '__main__':
    unittest.main()
