# SPDX-License-Identifier: GPL-2.0-or-later
"""Build a local trophy catalog from installed game files, without changing them.

The archive layout/decryption follows shadPS4's core/file_format/trp.cpp and
DiamondDogz's trophy_trp.py. Achievement conditions remain in the game.
"""
from __future__ import annotations

import hashlib
import os
from pathlib import Path
import re
import struct
import tempfile
import xml.etree.ElementTree as ET

from trophy_crypto import _words, _encrypt_block, _decrypt_block, _TROPHY_KEY

CATALOG_VERSION = 1
COMM_ID = re.compile(r"NPWR[0-9]{5}_[0-9]{2}\Z")


def trophy_language(title_id: str, user: Path, default: int = 1) -> int:
    """Trophy text language for this launch, as a PS4 system language ID.

    A PS4 shows trophy text in the console language. Sakura Wars (CUSA16429)
    reads that same system parameter, so its trophy text follows the emulated
    console language directly; no game save is read or changed. Archives use
    TROP_<system id> names; prepare_catalog falls back to the default TROP
    text when a language has no localized copy.
    """
    del title_id, user  # Console-language titles need no per-game source.
    return default if 0 <= default <= 30 else 1


def bindings(data: bytes) -> list[tuple[int, str, int]]:
    """Service label, communication ID and trophy archive number, from npbind.dat."""
    if len(data) < 128:
        raise ValueError("Truncated npbind header")
    magic, version, size, stride, count = struct.unpack_from(">IIQQQ", data)
    if (magic != 0xD294A018 or version != 1 or size != len(data) or
            not 16 <= stride <= 65536 or count > 64 or 128 + count * stride > size):
        raise ValueError("Invalid npbind header")
    result = []
    for label in range(count):
        pos, end = 128 + label * stride, 128 + (label + 1) * stride
        fields = {}
        for _ in range(4):
            if pos + 4 > end:
                raise ValueError("Truncated npbind entry")
            kind, length = struct.unpack_from(">HH", data, pos)
            pos += 4
            if pos + length > end or kind in fields:
                raise ValueError("Invalid npbind field")
            fields[kind] = data[pos:pos + length].rstrip(b"\0")
            pos += length
        identity = fields.get(0x10, b"").decode("ascii")
        number = fields.get(0x11, b"").decode("ascii")
        if not COMM_ID.fullmatch(identity) or not number.isdecimal() or int(number) > 99:
            raise ValueError("Invalid trophy binding")
        result.append((label, identity, int(number)))
    return result


def entries(blob: bytes) -> dict[str, bytes]:
    if len(blob) < 96 or blob[:4] != b"\xdc\xa2\x4d\0":
        raise ValueError("Invalid TRP header")
    size, count, stride = struct.unpack_from(">QII", blob, 8)
    if size != len(blob) or stride != 64 or count > 4096 or 96 + count * stride > size:
        raise ValueError("Invalid TRP table")
    result = {}
    for i in range(count):
        pos = 96 + i * stride
        name = blob[pos:pos + 32].split(b"\0", 1)[0].decode("ascii")
        start, length = struct.unpack_from(">QQ", blob, pos + 32)
        if (not re.fullmatch(r"[A-Z0-9_.]+", name) or name in result or
                start < 96 + count * stride or start + length > size):
            raise ValueError("Invalid TRP entry")
        result[name] = blob[start:start + length]
    return result


def xml(blob: bytes, identity: str, encrypted: bool = True) -> ET.Element:
    if encrypted:
        if len(blob) < 32 or len(blob) % 16:
            raise ValueError("Invalid encrypted trophy XML")
        key = _encrypt_block(identity.encode().ljust(16, b"\0"), _words(_TROPHY_KEY))
        schedule = _words(key)
        previous, output = blob[:16], bytearray()
        for offset in range(16, len(blob), 16):
            block = blob[offset:offset + 16]
            output.extend(a ^ b for a, b in zip(_decrypt_block(block, schedule), previous))
            previous = block
        # Archives use padded AES blocks; XML ends at its final closing bracket.
        blob = bytes(output[:output.rfind(b">") + 1])
    try:
        root = ET.fromstring(blob.decode("utf-8"))
    except ET.ParseError as error:
        raise ValueError("Invalid trophy XML") from error
    if root.tag != "trophyconf":
        raise ValueError("Invalid trophy XML root")
    return root


def _text(value: str | None) -> str:
    return " ".join((value or "").replace("\0", "").split())


def _atomic_write(path: Path, content: bytes) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    fd, name = tempfile.mkstemp(prefix=".trophy-", dir=path.parent)
    try:
        with os.fdopen(fd, "wb") as stream:
            stream.write(content)
            stream.flush()
            os.fsync(stream.fileno())
        os.replace(name, path)
    finally:
        if os.path.exists(name):
            os.unlink(name)


def prepare_catalog(game: Path, user: Path, language: int = 1) -> Path | None:
    """Cache text/icons only in this user's writable directory; return catalog path.

    Numbered trophy text uses PS4 system language IDs, unlike some games' own
    language menus. Callers translate that menu's language into a system ID.
    """
    system = Path(game) / "sce_sys"
    binding = system / "npbind.dat"
    if not binding.is_file():
        return None
    raw = binding.read_bytes()
    sources = []
    for label, identity, number in bindings(raw):
        trp = system / "trophy" / f"trophy{number:02d}.trp"
        if not trp.is_file() or trp.stat().st_size > 128 * 1024 * 1024:
            raise ValueError("Missing or oversized trophy archive")
        sources.append((label, identity, trp, trp.read_bytes()))
    if not sources:
        return None
    digest = hashlib.sha256(raw + f"{CATALOG_VERSION}:{language}".encode())
    for label, identity, trp, data in sources:
        digest.update(data)
    token = digest.hexdigest()
    folder = Path(user) / "achievements" / "catalog"
    # Immutable per-content directories let another running instance retain its
    # language/icons when this launch selects another language or updated assets.
    folder /= token
    catalog = folder / "catalog.tsv"
    if catalog.is_file():
        return catalog
    lines = ["PS4RT-TROPHIES\t1"]
    for label, identity, trp, data in sources:
        files = entries(data)

        def read(name: str) -> ET.Element:
            if name + ".ESFM" in files:
                return xml(files[name + ".ESFM"], identity)
            return xml(files[name + ".SFM"], identity, False)

        conf = read("TROPCONF")
        candidate = f"TROP_{language:02d}"
        localized = read(candidate if any(candidate + suffix in files for suffix in (".ESFM", ".SFM")) else "TROP")
        copy = {int(n.attrib["id"]): n for n in localized.findall("trophy")}
        lines.append("\t".join(("S", str(label), identity, _text(localized.findtext("title-name")),
                                 _text(localized.findtext("title-detail")))))
        seen = set()
        for item in conf.findall("trophy"):
            number = int(item.attrib["id"])
            grade = item.attrib["ttype"]
            if number in seen or not 0 <= number < 128 or grade not in "PGSB" or len(grade) != 1:
                raise ValueError("Invalid trophy definition")
            seen.add(number)
            text = copy.get(number, item)
            lines.append("\t".join(("T", str(label), str(number), grade,
                                     str(int(item.attrib.get("pid", "-1"))),
                                     str(int(item.attrib.get("gid", "-1"))),
                                     "1" if item.attrib.get("hidden") == "yes" else "0",
                                     _text(text.findtext("name")), _text(text.findtext("detail")))))
            icon = files.get(f"TROP{number:03d}.PNG", b"")
            if icon.startswith(b"\x89PNG\r\n\x1a\n") and len(icon) <= 4 * 1024 * 1024:
                _atomic_write(folder / identity / f"{number:03d}.png", icon)
        if not seen:
            raise ValueError("Empty trophy set")
    _atomic_write(catalog, ("\n".join(lines) + "\n").encode("utf-8"))
    return catalog
