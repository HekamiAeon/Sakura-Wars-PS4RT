# SPDX-License-Identifier: GPL-2.0-or-later
"""Sakura Wars player-owned update/DLC setup. No PKG or game data is distributed."""
from __future__ import annotations

import hashlib
import contextlib
import time
import json
import os
from pathlib import Path
import re
import shutil
import subprocess
import sys
import tempfile

from patch_assembly import PatchError, apply_in_place, incomplete_marker, check_pair, find_patch_root
from prepare import load_sfo
from content_profile import find_addons

TITLE_ID = "CUSA16429"
TARGET_VERSION = "01.01"
BASE_VERSION = "01.00"
# Internal overlay installer status: 0 = no changes, 10 = DLC added, 1 = failure.
DLC_INSTALLED_EXIT = 10


class SetupError(ValueError):
    pass


def _sha(path: Path) -> str:
    h = hashlib.sha256()
    with path.open("rb") as file:
        for chunk in iter(lambda: file.read(4 * 1024 * 1024), b""):
            h.update(chunk)
    return h.hexdigest()


def _atomic_json(path: Path, data: dict) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    with tempfile.NamedTemporaryFile(mode="w", encoding="utf8", dir=path.parent,
                                     prefix=".setup-", delete=False) as stream:
        temp = Path(stream.name)
        json.dump(data, stream, indent=2, sort_keys=True)
        stream.write("\n")
    try:
        temp.replace(path)
    finally:
        temp.unlink(missing_ok=True)


def _read_json(path: Path) -> dict:
    try:
        return json.loads(path.read_text(encoding="utf8"))
    except (OSError, ValueError, TypeError):
        return {}


def _metadata(game: Path) -> dict:
    if not game.is_dir() or not (game / "eboot.bin").is_file():
        raise SetupError(f"Expected extracted game with eboot.bin: {game}")
    info = load_sfo(game / "sce_sys/param.sfo")
    if info.get("TITLE_ID") != TITLE_ID:
        raise SetupError(f"This port requires {TITLE_ID}, got {info.get('TITLE_ID')!r}")
    return info


def _desktop() -> bool:
    return bool(os.environ.get("DISPLAY") or os.environ.get("WAYLAND_DISPLAY"))


SETUP_COPY = {
    "update": (
        "Sakura Wars v1.00 detected. Version 1.01 is required.",
        "Select your own v1.01 update PKG. This changes the extracted CUSA game "
        "folder in place. It will not move your saves or original PKG."
    ),
    "dlc": (
        "Optional Sakura Wars DLC.",
        "Select one or more DLC PKGs you own, or skip. "
        "You can add DLC later through the in-game overlay."
    ),
    "dlc_later": (
        "Install additional Sakura Wars DLC.",
        "Choose one or more DLC PKGs you own. Save and restart the game "
        "to activate newly installed content."
    ),
}


def _setup_executable(root: Path) -> Path:
    """Use the shipped player binary first, or the developer build in out/."""
    portable = root / "bin" / "ps4rt-setup"
    return portable if portable.is_file() else root / "out" / "ps4rt-setup"


def choose_setup(root: Path, kind: str) -> list[Path]:
    """Bundled SDL3/ImGui picker; headless terminal path input is optional.

    The UI prints only selected paths to stdout; return code 1 means cancelled,
    2 means setup failed. There is no Tkinter, KDialog or Zenity dependency.
    """
    if kind not in SETUP_COPY:
        raise SetupError(f"Unknown player setup operation: {kind}")
    if _desktop():
        executable = _setup_executable(root)
        if not executable.is_file() or not os.access(executable, os.X_OK):
            raise SetupError("Sakura Wars setup interface is missing. "
                             "Build the port with bash build.sh, or use the "
                             "packaged portable release.")
        result = subprocess.run([str(executable), "--mode", kind],
                                capture_output=True, text=True, check=False)
        if result.returncode == 1:
            return []  # player skipped or cancelled
        if result.returncode != 0:
            raise SetupError(f"Sakura Wars setup interface failed "
                             f"(code {result.returncode}): {result.stderr.strip()}")
        files = [Path(p).expanduser().resolve() for p in result.stdout.splitlines() if p.strip()]
        if (kind == "update" and len(files) != 1) or not files:
            raise SetupError("The setup interface returned no usable PKG selection.")
        return files
    if not sys.stdin.isatty():
        raise SetupError("A graphical desktop or interactive terminal is required "
                         "to choose a PKG. Alternatively pass --update-pkg "
                         "or --dlc-pkg explicitly.")
    heading, explanation = SETUP_COPY[kind]
    print(f"\n{heading}\n{explanation}\n", flush=True)
    prompt = ("Paste the update PKG path (blank to cancel): " if kind == "update"
              else "Paste a DLC PKG path (blank to finish/skip): ")
    paths = []
    while True:
        value = input(prompt).strip()
        if not value:
            break
        paths.append(Path(value).expanduser().resolve())
        if kind == "update":
            break
    return paths


@contextlib.contextmanager
def _setup_progress(root: Path, *, kind: str, total: int, show_window: bool):
    """Shared SDL3 progress UI, reporting completed stages rather than byte %.

    A missing desktop or setup executable falls back to terminal diagnostics.
    """
    total = max(1, int(total))
    is_dlc = kind == "dlc"
    label = "DLC" if is_dlc else "update"

    def terminal_progress(message: str, completed: int, state: str = "working"):
        print(f"Sakura Wars {label} [{completed}/{total}]: {message}", flush=True)

    executable = _setup_executable(root)
    if (not show_window or not _desktop() or not executable.is_file()
            or not os.access(executable, os.X_OK)):
        yield terminal_progress
        return

    player = content_dir(root)
    player.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(prefix="progress-ui-", dir=player) as directory:
        status_file = Path(directory) / "status"

        def publish(message: str, completed: int, state: str = "working"):
            terminal_progress(message, completed, state)
            message = " ".join(str(message).split())[:300]
            step = max(0, min(total, int(completed)))
            temp = status_file.with_suffix(".next")
            temp.write_text(f"{state}\t{step}\t{total}\t{message}\n",
                            encoding="utf8")
            temp.replace(status_file)

        publish("Preparing DLC installation..." if is_dlc else "Preparing update...", 0)
        viewer = None
        mode = "progress-dlc" if is_dlc else "progress"
        try:
            viewer = subprocess.Popen([str(executable), "--mode", mode,
                                       str(status_file)],
                                      stdin=subprocess.DEVNULL,
                                      stdout=subprocess.DEVNULL,
                                      stderr=subprocess.DEVNULL)
        except OSError as error:
            print(f"Sakura Wars: progress window unavailable ({error}); "
                  "continuing in terminal.", file=sys.stderr, flush=True)
        try:
            yield publish
        except Exception:
            publish("Installation failed. See terminal for details.", 0, "error")
            if viewer is not None and viewer.poll() is None:
                time.sleep(1.5)
            raise
        else:
            publish("DLC installation complete. Restart to activate." if is_dlc
                    else "Version 1.01 is ready!", total, "success")
            if viewer is not None and viewer.poll() is None:
                time.sleep(1.2 if is_dlc else 0.9)
        finally:
            if viewer is not None:
                if viewer.poll() is None:
                    viewer.terminate()
                try:
                    viewer.wait(timeout=2)
                except subprocess.TimeoutExpired:
                    viewer.kill()
                    viewer.wait()


@contextlib.contextmanager
def update_progress(root: Path, show_window: bool = True):
    with _setup_progress(root, kind="update", total=5,
                         show_window=show_window) as notify:
        yield notify


@contextlib.contextmanager
def dlc_progress(root: Path, count: int, show_window: bool = True):
    with _setup_progress(root, kind="dlc", total=3 * count,
                         show_window=show_window) as notify:
        yield notify


def _extractor(root: Path, override: str | None = None) -> str:
    """Use PS4RT's own bundled native parser, never a player-installed extractor.

    An explicit override remains available for developer diagnostic sessions.
    """
    configured = override or os.environ.get("PS4RT_PKG_TOOL")
    if configured:
        found = shutil.which(configured) if "/" not in configured else configured
        if found and Path(found).is_file() and os.access(found, os.X_OK):
            return str(Path(found).resolve())
        raise SetupError(f"Configured developer PKG tool is not executable: {configured}")
    portable = root / "bin" / "ps4rt-pkg"
    native = portable if portable.is_file() else root / "out" / "ps4rt-pkg"
    if native.is_file() and os.access(native, os.X_OK):
        return str(native.resolve())
    raise SetupError("PS4RT native PKG component is missing. "
                     "Build the workspace with bash scripts/build_pkg_native.sh "
                     "or use a portable release with bin/ps4rt-pkg already included.")


def _validate_pkg(path: Path) -> None:
    if not path.is_file() or path.suffix.lower() != ".pkg" or path.is_symlink():
        raise SetupError(f"Expected an existing regular .pkg file: {path}")


def _extract(root: Path, pkg: Path, directory: Path, tool: str | None) -> None:
    _validate_pkg(pkg)
    directory.mkdir(parents=True, exist_ok=False)
    command = [_extractor(root, tool), str(pkg), str(directory)]
    print(f"Extracting {pkg.name} into a private staging directory...", flush=True)
    try:
        subprocess.run(command, check=True)
    except subprocess.CalledProcessError as exc:
        raise SetupError(f"PKG extraction failed ({exc.returncode}): {pkg.name}") from exc


def content_dir(root: Path) -> Path:
    return root / "out" / TITLE_ID / "player-content"


def resolve_game(root: Path, game: Path, *,
                 update_pkg: Path | None = None,
                 update_extracted: Path | None = None,
                 pkg_tool: str | None = None,
                 dialogs: bool = True) -> Path:
    """Upgrade the supplied extracted CUSA folder in place, avoiding a second 40GB copy.

    If interrupted, refuse to boot and allow a reapplication of the same
    update. The original v1.00 files replaced by the patch are not backed up.
    """
    game = game.expanduser().resolve(strict=True)
    version = str(_metadata(game).get("APP_VER", "")).strip()
    marker = incomplete_marker(game)
    interrupted = marker.exists() or marker.is_symlink()
    if version == TARGET_VERSION and not interrupted:
        print(f"Sakura Wars {TITLE_ID} v{TARGET_VERSION} detected.", flush=True)
        return game
    if version != BASE_VERSION and not (version == TARGET_VERSION and interrupted):
        raise SetupError(f"Sakura Wars v{version or 'unknown'} is unsupported. "
                         f"This native port requires v{TARGET_VERSION}.")
    if interrupted:
        print("An interrupted update was detected. "
              "Re-select the same v1.01 update to recover.", file=sys.stderr, flush=True)
    if update_pkg and update_extracted:
        raise SetupError("Select one update source, not both.")
    if not update_pkg and not update_extracted and dialogs:
        selected = choose_setup(root, "update")
        update_pkg = selected[0] if selected else None
    if not update_pkg and not update_extracted:
        raise SetupError("v1.00 cannot launch directly (or an update was interrupted). "
                         "Select your v1.01 update PKG, or pass --update-pkg.")
    player = content_dir(root)
    player.mkdir(parents=True, exist_ok=True)
    with update_progress(root, show_window=dialogs) as notify:
        with tempfile.TemporaryDirectory(prefix="update-stage-", dir=player) as temporary:
            staging = Path(temporary)
            if update_pkg:
                notify("Extracting the selected v1.01 update PKG...", 0)
                _extract(root, update_pkg, staging / "extracted", pkg_tool)
                patch = find_patch_root(staging / "extracted")
            else:
                notify("Checking the extracted v1.01 update...", 0)
                patch = find_patch_root(update_extracted.expanduser().resolve(strict=True))
            notify("Checking update version and game compatibility...", 1)
            info = load_sfo(patch / "sce_sys/param.sfo")
            if info.get("APP_VER") != TARGET_VERSION:
                raise SetupError(f"Selected update is v{info.get('APP_VER')!r}, "
                                 f"expected v{TARGET_VERSION}.")
            if str(info.get("CATEGORY", "")).lower() != "gp":
                raise SetupError("Selected package is not a game update (CATEGORY must be gp).")
            report = check_pair(game, patch, load_sfo, TITLE_ID)
            if not (patch / "eboot.bin").is_file():
                raise SetupError("The update must contain a complete v1.01 eboot.bin.")
            notify("Update verified. Preparing to modify the extracted CUSA folder...", 2)
            report = apply_in_place(game, patch, load_sfo, TITLE_ID,
                                    progress_callback=notify)
            if _metadata(game).get("APP_VER") != TARGET_VERSION:
                raise SetupError("Installed game did not report v1.01; refusing launch.")
            _atomic_json(player / "update.json", {
                "game": str(game),
                "effective_eboot_sha256": _sha(game / "eboot.bin"),
                "version": TARGET_VERSION,
                "installation_mode": "in-place",
                "warnings": report["warnings"],
                "replaced_files": len(report["replaced_paths"]),
                "added_files": len(report["added_paths"]),
            })
    print(f"Updated the existing extracted game to v1.01: {game}", flush=True)
    for warning in report["warnings"]:
        print(f"Update warning: {warning}", file=sys.stderr)
    return game


def install_dlc(root: Path, pkgs: list[Path], *, pkg_tool: str | None = None,
                show_window: bool = True) -> list[str]:
    """Validate and install player DLC with the same native progress UI as updates.

    Stage count is three per PKG: extracted, validated, and installed/skipped.
    Existing DLC and source PKGs remain untouched. Earlier successful DLC are
    preserved if a later selected PKG fails.
    """
    player = content_dir(root)
    destination = player / "dlc"
    installed = []
    for pkg in pkgs:
        _validate_pkg(pkg)
    if not pkgs:
        return []
    player.mkdir(parents=True, exist_ok=True)
    with dlc_progress(root, len(pkgs), show_window=show_window) as notify:
        for index, pkg in enumerate(pkgs):
            start = index * 3
            count = len(pkgs)
            name = f"{pkg.name} ({index + 1}/{count})"
            with tempfile.TemporaryDirectory(prefix="dlc-stage-", dir=player) as temporary:
                stage = Path(temporary)
                notify(f"Extracting DLC {name}...", start)
                _extract(root, pkg, stage / "extracted", pkg_tool)
                notify(f"Checking DLC ownership and title: {name}...", start + 1)
                addons = find_addons(stage / "extracted")
                if len(addons) != 1:
                    raise SetupError(
                        f"{pkg.name}: expected exactly one DLC package, found {len(addons)}.")
                label, extracted = next(iter(addons.items()))
                data = load_sfo(extracted / "sce_sys/param.sfo")
                content_id = str(data.get("CONTENT_ID", ""))
                expected_content = rf"[A-Za-z0-9]{{6}}-{TITLE_ID}_[0-9]{{2}}-[A-Za-z0-9]{{16}}"
                if (not re.fullmatch(expected_content, content_id) or
                        label != content_id[-16:]):
                    raise SetupError(f"{pkg.name}: add-on is not for {TITLE_ID}.")
                if str(data.get("CATEGORY", "")).lower() not in ("ac", "ad"):
                    raise SetupError(f"{pkg.name}: not a DLC add-on.")
                for file in extracted.rglob("*"):
                    if file.is_symlink() or not (file.is_file() or file.is_dir()):
                        raise SetupError(f"{pkg.name}: unsafe extracted entry: {file}")
                final = destination / label
                notify(f"Installing DLC {name}...", start + 2)
                if final.exists() or final.is_symlink():
                    print(f"DLC {label} already installed; unchanged.", flush=True)
                    notify(f"DLC already installed; skipped {name}.", start + 3)
                    continue
                destination.mkdir(parents=True, exist_ok=True)
                candidate = stage / "validated"
                shutil.copytree(extracted, candidate, symlinks=False)
                candidate.rename(final)
                installed.append(label)
                print(f"Installed {label}. Restart Sakura Wars to activate.", flush=True)
                notify(f"DLC installed: {name}.", start + 3)
    return installed


def installed_addons(root: Path, explicit: Path | None = None) -> dict:
    """Combine optional pre-extracted DLC and player-managed PKGs."""
    folder = root / "dlc" / TITLE_ID if explicit is None else explicit.expanduser().resolve()
    if explicit is not None and not folder.is_dir():
        raise SetupError(f"DLC folder does not exist: {folder}")
    addons = find_addons(folder)
    addons.update(find_addons(content_dir(root) / "dlc"))
    return addons



def add_dlc_dialog(root: Path, *, pkg_tool: str | None = None) -> int:
    try:
        if not _desktop():
            raise SetupError("In-game DLC setup requires a graphical desktop.")
        pkgs = choose_setup(root, "dlc_later")
        if not pkgs:
            print("DLC selection cancelled; nothing changed.", flush=True)
            return 0
        installed = install_dlc(root, pkgs, pkg_tool=pkg_tool)
        if installed:
            print(f"Installed {len(installed)} add-on(s). Save and restart the game to activate.", flush=True)
            return DLC_INSTALLED_EXIT
        return 0
    except (OSError, ValueError, subprocess.CalledProcessError) as error:
        print(f"Sakura Wars DLC installer: {error}", file=sys.stderr, flush=True)
        return 1
