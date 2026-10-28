# SPDX-License-Identifier: GPL-2.0-or-later
# Adapted from PS4RT-Lab's validated patch assembly helper.
"""Build a separate effective PS4 installation from base and update extractions.

Input directories must be fully extracted PKGs. This assembles file overlays,
not raw delta streams, and never modifies either source.
"""
from __future__ import annotations

import errno
import hashlib
import json
import os
from pathlib import Path
import shutil
import subprocess
import tempfile


class PatchError(ValueError):
    pass


def _manifest(root: Path) -> dict[str, Path]:
    if not root.is_dir() or root.is_symlink():
        raise PatchError(f"Not a normal extracted directory: {root}")
    result = {}
    for path in root.rglob('*'):
        if path.is_symlink() or not (path.is_dir() or path.is_file()):
            raise PatchError(f"Unsupported filesystem entry: {path}")
        if path.is_file():
            result[path.relative_to(root).as_posix()] = path
    return result


def find_patch_root(path: Path) -> Path:
    """Accept the patch root or its extractor-created CUSAxxxxx-patch subdirectory."""
    if (path / 'sce_sys' / 'param.sfo').is_file():
        return path
    candidates = [p.parent.parent for p in path.glob('*/sce_sys/param.sfo')]
    if len(candidates) != 1:
        raise PatchError(f"Expected one update installation under {path}; found {len(candidates)}")
    return candidates[0]


def _sha256(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open('rb') as handle:
        for block in iter(lambda: handle.read(4 * 1024 * 1024), b''):
            digest.update(block)
    return digest.hexdigest()


def check_pair(base: Path, patch: Path, read_sfo, expected_id: str | None = None) -> dict:
    old = _manifest(base)
    new = _manifest(patch)
    for root, which in ((base, 'base'), (patch, 'update')):
        if not (root / 'sce_sys' / 'param.sfo').is_file():
            raise PatchError(f"Missing {which} sce_sys/param.sfo: {root}")
    base_sfo = read_sfo(base / 'sce_sys' / 'param.sfo')
    patch_sfo = read_sfo(patch / 'sce_sys' / 'param.sfo')
    bid = str(base_sfo.get('TITLE_ID', ''))
    pid = str(patch_sfo.get('TITLE_ID', ''))
    if not bid.startswith('CUSA') or len(bid) != 9 or not bid[4:].isdigit():
        raise PatchError(f"Invalid base TITLE_ID: {bid!r}")
    if bid != pid or (expected_id is not None and bid != expected_id):
        raise PatchError(f"Update/base TITLE_ID mismatch: {bid!r} vs {pid!r} (expected {expected_id!r})")
    category = str(patch_sfo.get('CATEGORY', '')).lower()
    if category in {'ac', 'ad'}:
        raise PatchError(f"DLC/additional content is not a game update (CATEGORY={category})")
    # Check the whole region/product CONTENT_ID of the base and patch,
    # not just the CUSA title number. Missing metadata fails closed.
    base_content_id = str(base_sfo.get('CONTENT_ID') or '').strip()
    patch_content_id = str(patch_sfo.get('CONTENT_ID') or '').strip()
    if not base_content_id or not patch_content_id:
        raise PatchError('Base game and update must both supply CONTENT_ID in param.sfo')
    if f'-{bid}_' not in base_content_id or f'-{pid}_' not in patch_content_id:
        raise PatchError('Base/update CONTENT_ID does not identify the expected CUSA title')
    if base_content_id != patch_content_id:
        raise PatchError(
            f'Update/base CONTENT_ID mismatch: {base_content_id!r} vs {patch_content_id!r}'
        )
    if not (base / 'eboot.bin').is_file():
        raise PatchError(f"Base missing eboot.bin: {base}")
    if not new:
        raise PatchError('Extracted update is empty')
    shared = sorted(old.keys() & new.keys())
    added = sorted(new.keys() - old.keys())
    report = {
        'title_id': bid,
        'content_id': base_content_id,
        'base_version': str(base_sfo.get('APP_VER', 'unknown')),
        'update_version': str(patch_sfo.get('APP_VER', 'unknown')),
        'update_category': category or 'unknown',
        'base_file_count': len(old),
        'update_file_count': len(new),
        'replaced_paths': shared,
        'added_paths': added,
        'effective_file_count': len(old) + len(added),
        'warnings': [],
    }
    if category not in ('gp', ''):
        report['warnings'].append(f"Unrecognized update CATEGORY={category}; verify package type")
    if 'sce_sys/origin-deltainfo.dat' in new or 'sce_discmap_patch.plt' in new:
        report['warnings'].append(
            'PS4 delta/discmap metadata found; verify extracted update has materialized complete replacement files'
        )
    if 'eboot.bin' not in new:
        report['warnings'].append(
            'Update does not replace eboot.bin; effective installation retains base executable'
        )
    for label, collection in (('base', old), ('update', new)):
        if 'eboot.bin' in collection:
            report[f'{label}_eboot_sha256'] = _sha256(collection['eboot.bin'])
    return report


def assemble(base: Path, patch: Path, dest: Path, read_sfo,
             expected_id: str | None = None, report_path: Path | None = None,
             progress_callback=None) -> dict:
    """Overlay a patch into a *new* installation, refusing existing destinations."""
    base = base.resolve(strict=True)
    patch = patch.resolve(strict=True)
    dest = dest.expanduser().absolute()
    if dest.exists() or dest.is_symlink():
        raise PatchError(f"Destination already exists; refusing overwrite: {dest}")
    if dest == base or dest == patch or base in dest.parents or patch in dest.parents:
        raise PatchError('Destination must be separate from both source directories')
    report = check_pair(base, patch, read_sfo, expected_id)
    if report_path and (report_path.exists() or report_path.is_symlink()):
        raise PatchError(f"Report already exists: {report_path}")
    dest.parent.mkdir(parents=True, exist_ok=True)
    dest.mkdir()
    if progress_callback:
        progress_callback('Copying original game files...', 2)
    # Fedora/Btrfs can clone 43GB of assets without rewriting the data blocks.
    # Do not use --update: patch contents always win regardless of timestamps.
    subprocess.run(['cp', '-a', '--reflink=auto', f'{base}/.', str(dest)], check=True)
    if progress_callback:
        progress_callback('Applying version 1.01 replacement files...', 3)
    subprocess.run(['cp', '-a', '--reflink=auto', '--remove-destination',
                    f'{patch}/.', str(dest)], check=True)
    if progress_callback:
        progress_callback('Verifying the prepared installation...', 4)
    actual = _manifest(dest)
    if len(actual) != report['effective_file_count']:
        raise PatchError(
            f"Unexpected assembled file count: {len(actual)} vs {report['effective_file_count']}"
        )
    selected_hash = report.get('update_eboot_sha256') or report['base_eboot_sha256']
    if _sha256(dest / 'eboot.bin') != selected_hash:
        raise PatchError('Effective eboot.bin SHA-256 does not match the selected executable')
    report['effective_eboot_sha256'] = selected_hash
    report['effective_path'] = str(dest)
    report['base_path'] = str(base)
    report['update_path'] = str(patch)
    if report_path:
        report_path.parent.mkdir(parents=True, exist_ok=True)
        report_path.write_text(json.dumps(report, indent=2, sort_keys=True) + '\n')
    return report

# Kept alongside the player's extracted game, not the optional update PKG.
# If the process crashes, this marker blocks boot and enables reapplication.
INCOMPLETE_MARKER = ".ps4rt-v101-update-incomplete.json"


def incomplete_marker(base: Path) -> Path:
    return base / INCOMPLETE_MARKER


def _sync_directory(path: Path) -> None:
    """Persist metadata where supported; some FUSE filesystems reject directory fsync."""
    try:
        fd = os.open(path, os.O_RDONLY | getattr(os, "O_DIRECTORY", 0))
    except OSError as error:
        if error.errno in (errno.EINVAL, errno.EOPNOTSUPP, errno.ENOTSUP):
            return
        raise
    try:
        try:
            os.fsync(fd)
        except OSError as error:
            if error.errno not in (errno.EINVAL, errno.EOPNOTSUPP, errno.ENOTSUP):
                raise
    finally:
        os.close(fd)


def apply_in_place(base: Path, patch: Path, read_sfo,
                   expected_id: str = "CUSA16429", progress_callback=None) -> dict:
    """Patch an extracted v1.00 game in place, using only the update's disk footprint.

    Each replacement is copied to a temporary sibling and atomically renamed,
    never truncated in place. On interruption a marker remains; safely reapply
    the SAME complete update to resume. No 40 GB duplicate game tree is made.
    """
    base = base.resolve(strict=True)
    patch = patch.resolve(strict=True)
    if base == patch or base in patch.parents or patch in base.parents:
        raise PatchError("Game and update must be separate directories")
    if base.is_symlink() or patch.is_symlink():
        raise PatchError("Refusing symlinked game/update root")
    report = check_pair(base, patch, read_sfo, expected_id)
    original = read_sfo(base / "sce_sys/param.sfo")
    update = read_sfo(patch / "sce_sys/param.sfo")
    if str(update.get("APP_VER", "")) != "01.01" or str(update.get("CATEGORY", "")).lower() != "gp":
        raise PatchError("Update must be the matching v1.01 game patch")
    source_files = _manifest(patch)
    if "eboot.bin" not in source_files or "sce_sys/param.sfo" not in source_files:
        raise PatchError("A full updated eboot.bin and param.sfo are required")
    marker = incomplete_marker(base)
    fingerprint = _sha256(source_files["eboot.bin"])
    if marker.exists() or marker.is_symlink():
        if marker.is_symlink() or not marker.is_file():
            raise PatchError("Invalid incomplete-update marker, refusing modification")
        try:
            prior = json.loads(marker.read_text(encoding="utf8"))
        except (OSError, ValueError) as error:
            raise PatchError("Incomplete-update marker is damaged; manual inspection required") from error
        if (prior.get("title_id"), prior.get("version"), prior.get("update_eboot_sha256")) != (
                expected_id, "01.01", fingerprint):
            raise PatchError("Interrupted update belongs to another package. "
                             "Re-select the original v1.01 update PKG.")
    elif str(original.get("APP_VER", "")) != "01.00":
        raise PatchError("Refusing to overwrite a game that is not v1.00")
    # Check disk and directory layout before modifying even one original file.
    # _manifest rejects all symlinks, including parent paths already in the game.
    _manifest(base)
    for relative in source_files:
        target = base / relative
        if target.is_symlink():
            raise PatchError(f"Refusing symlinked update destination: {relative}")
        if target.exists() and not target.is_file():
            raise PatchError(f"Update file would replace a directory: {relative}")
        parent = target.parent
        while parent != base:
            if parent.exists() and (parent.is_symlink() or not parent.is_dir()):
                raise PatchError(f"Unsafe update destination parent: {parent}")
            parent = parent.parent
    # Atomic sibling replacements require enough free space for the largest
    # single updated file, but never a second copy of the entire game.
    largest_replacement = max(p.stat().st_size for p in source_files.values())
    available = shutil.disk_usage(base).free
    if available < largest_replacement + 64 * 1024 * 1024:
        raise PatchError(f"Insufficient free space for safe in-place replacement: "
                         f"need about {largest_replacement + 64 * 1024 * 1024} bytes, "
                         f"have {available}")
    if progress_callback:
        progress_callback("Installing updated files into the original CUSA folder...", 2)

    if not marker.exists():
        with marker.open("x", encoding="utf8") as stream:
            json.dump({"title_id": expected_id, "version": "01.01",
                       "update_eboot_sha256": fingerprint}, stream)
            stream.write("\n")
            stream.flush()
            os.fsync(stream.fileno())
        _sync_directory(base)
    # param.sfo is committed last; the interruption marker is only removed
    # after all replacement files and metadata pass verification.
    ordering = sorted(x for x in source_files if x != "sce_sys/param.sfo")
    ordering.append("sce_sys/param.sfo")
    if progress_callback:
        progress_callback("Applying update files safely, one by one...", 3)
    touched_directories = {base}
    for relative in ordering:
        source = source_files[relative]
        target = base / relative
        target.parent.mkdir(parents=True, exist_ok=True)
        parent = target.parent
        while parent != base:
            touched_directories.add(parent)
            parent = parent.parent
        temporary = None
        try:
            with tempfile.NamedTemporaryFile(prefix=".ps4rt-patch-",
                                             dir=target.parent, delete=False) as stream:
                temporary = Path(stream.name)
                with source.open("rb") as original_file:
                    shutil.copyfileobj(original_file, stream, length=4 * 1024 * 1024)
                stream.flush()
                os.fsync(stream.fileno())
            shutil.copymode(source, temporary, follow_symlinks=False)
            if temporary.stat().st_size != source.stat().st_size:
                raise PatchError(f"Short update copy: {relative}")
            os.replace(temporary, target)
            temporary = None
        finally:
            if temporary is not None:
                temporary.unlink(missing_ok=True)
    # Flush directory entries before declaring the update complete.
    for directory in sorted(touched_directories, key=lambda p: len(p.parts), reverse=True):
        _sync_directory(directory)
    if progress_callback:
        progress_callback("Verifying updated game executable and version...", 4)
    updated = read_sfo(base / "sce_sys/param.sfo")
    if (str(updated.get("TITLE_ID")), str(updated.get("APP_VER"))) != (expected_id, "01.01"):
        raise PatchError("Update did not produce the expected v1.01 game metadata")
    if _sha256(base / "eboot.bin") != fingerprint:
        raise PatchError("Updated executable failed SHA-256 verification")
    marker.unlink()
    _sync_directory(base)
    report["effective_eboot_sha256"] = fingerprint
    report["effective_path"] = str(base)
    report["installation_mode"] = "in-place"
    return report


