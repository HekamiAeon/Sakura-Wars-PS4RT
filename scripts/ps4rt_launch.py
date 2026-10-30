#!/usr/bin/env python3
"""Prepare and start a native Linux PS4RT port from the current working template.

Future games require reverse engineering and source changes. Not a general PS4 emulator.
"""
# SPDX-License-Identifier: GPL-2.0-or-later
import argparse
import os
from pathlib import Path
import re
import shlex
import sys
import tempfile
from prepare import load_sfo, prepare
from link_modules import link
from content_profile import prepare as prepare_content
from player_setup import (TITLE_ID, TARGET_VERSION, BASE_VERSION, DLC_INSTALLED_EXIT, SetupError,
                          resolve_game, install_dlc, installed_addons, add_dlc_dialog,
                          choose_setup)
from patch_assembly import incomplete_marker

ROOT = Path(__file__).resolve().parent.parent
TITLE_ID_PATTERN = re.compile(r'CUSA\d{5}\Z')


def _select_game_folder(explicit: Path | None, remembered: Path) -> Path:
    """Resolve the player's game folder without changing remembered state.

    Explicit --game always wins, even if a different valid folder was saved.
    Invalid, moved, or missing folders fail with an actionable first-run hint.
    """
    hint = ('Run: bash Play-Game.sh --game '
            '"/absolute/path/to/CUSA16429"')
    if explicit is None:
        try:
            saved = remembered.read_text(encoding='utf-8').strip()
        except (OSError, UnicodeError):
            saved = ''
        if not saved:
            raise SetupError('Game folder not configured. First launch: ' + hint)
        candidate = Path(saved)
    else:
        candidate = explicit
    try:
        game = candidate.expanduser().resolve(strict=True)
    except (OSError, RuntimeError) as error:
        raise SetupError(f'Game folder does not exist: {candidate}. {hint}') from error
    if not game.is_dir():
        raise SetupError(f'Expected the extracted CUSA16429 directory: {game}. {hint}')
    return game


def _remember_game_folder(destination: Path, game: Path) -> None:
    """Atomically save only a successfully prepared game location.

    Keep this in the portable's existing out/CUSA16429 directory alongside
    saves and settings. Never replace that directory or move player data.
    """
    destination.parent.mkdir(parents=True, exist_ok=True)
    with tempfile.NamedTemporaryFile(mode='w', encoding='utf-8',
                                     prefix='.game-folder-',
                                     dir=destination.parent, delete=False) as stream:
        temporary = Path(stream.name)
        stream.write(str(game) + '\n')
    try:
        temporary.replace(destination)
    finally:
        temporary.unlink(missing_ok=True)


def _restart_after_setup(args, game: Path, *, include_dlc_pkgs: bool, stage: str) -> None:
    """Re-exec the launcher before game startup after changing installed content.

    Update packages never repeat. After an update we forward explicit DLC
    packages once; after DLC installation we omit them to avoid an install loop.
    """
    command = [sys.executable, str(Path(__file__).resolve()), '--game', str(game)]
    if args.dlc is not None:
        command += ['--dlc', str(args.dlc)]
    if include_dlc_pkgs:
        for pkg in args.dlc_pkg:
            command += ['--dlc-pkg', str(pkg)]
    if args.pkg_tool:
        command += ['--pkg-tool', args.pkg_tool]
    if args.no_dialogs:
        command.append('--no-dialogs')
    if args.upscaler is not None:
        command += ['--upscaler', args.upscaler]
    if args.output_resolution is not None:
        command += ['--output-resolution', args.output_resolution]
    print(f'Sakura Wars: {stage}. Restarting clean game launcher...', flush=True)
    os.execv(sys.executable, command)


def _restart_after_update(args, game: Path) -> None:
    _restart_after_setup(args, game, include_dlc_pkgs=True, stage='update verified')


def _restart_after_dlc(args, game: Path) -> None:
    _restart_after_setup(args, game, include_dlc_pkgs=False, stage='DLC installed')


def main(argv=None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--game', type=Path,
                        help='Your extracted plaintext CUSA16429 directory '
                             '(remembered after successful preparation)')
    parser.add_argument('--dlc', type=Path,
                        help='Folder of extracted DLC packages (default: dlc/CUSAxxxxx if present)')
    parser.add_argument('--update-pkg', type=Path,
                        help='Optional version 1.01 game-update PKG (prompts if v1.00)')
    parser.add_argument('--update-extracted', type=Path,
                        help='Already extracted v1.01 game-update directory')
    parser.add_argument('--dlc-pkg', type=Path, action='append', default=[],
                        help='Player-owned DLC PKG; repeat for multiple add-ons')
    parser.add_argument('--install-dlc', action='store_true',
                        help='Open the DLC installer (used by the in-game overlay)')
    parser.add_argument('--pkg-tool',
                        help='Override bundled PS4 PKG extractor executable')
    parser.add_argument('--no-dialogs', action='store_true',
                        help='Do not open setup dialogs (scripting/headless)')
    parser.add_argument('--prepare-only', action='store_true')
    parser.add_argument('--upscaler', choices=('off', 'fsr3', 'fsr4'),
                        help='Override the saved graphics setting for this launch')
    parser.add_argument('--output-resolution', choices=('1080', '1440', '4k'))
    args = parser.parse_args(argv)
    remembered = ROOT / 'out' / TITLE_ID / 'game-folder.txt'
    original_game = _select_game_folder(args.game, remembered)
    title_id = str(load_sfo(original_game / 'sce_sys/param.sfo').get('TITLE_ID', ''))
    if not TITLE_ID_PATTERN.fullmatch(title_id) or title_id != TITLE_ID:
        parser.error(f'This port requires {TITLE_ID} game data')
    if args.update_pkg and args.update_extracted:
        parser.error('Provide only one game-update source')
    if args.install_dlc:
        if args.dlc_pkg:
            added = install_dlc(ROOT, args.dlc_pkg, pkg_tool=args.pkg_tool,
                                show_window=not args.no_dialogs and not args.prepare_only)
            print(f'Installed {len(added)} new DLC package(s); restart to activate.')
            return DLC_INSTALLED_EXIT if added else 0
        return add_dlc_dialog(ROOT, pkg_tool=args.pkg_tool)
    # Developer build: out/ps4rt. Audited portable releases: bin/ps4rt.
    portable = ROOT / 'bin/ps4rt'
    runtime = portable if portable.is_file() else ROOT / 'out/ps4rt'
    if not args.prepare_only and not runtime.is_file():
        parser.error('Build the runtime first: bash build.sh')
    # Only a successful v1.00 installation or interrupted-update recovery
    # needs a fresh launcher. Ordinary v1.01 launches retain the old path.
    metadata = load_sfo(original_game / 'sce_sys/param.sfo')
    was_v100 = str(metadata.get('APP_VER', '')).strip() == BASE_VERSION
    needs_update = (str(metadata.get('APP_VER', '')).strip() != TARGET_VERSION
                    or incomplete_marker(original_game).exists()
                    or incomplete_marker(original_game).is_symlink())
    game = resolve_game(ROOT, original_game,
                        update_pkg=args.update_pkg,
                        update_extracted=args.update_extracted,
                        pkg_tool=args.pkg_tool,
                        dialogs=not args.no_dialogs and not args.prepare_only)
    if needs_update and not args.prepare_only:
        # resolve_game only returns after the update is installed, verified,
        # its interruption marker cleared, and the progress UI closed.
        # Offer optional DLC only after a fresh v1.00 -> v1.01 upgrade.
        # Existing v1.01 games, scripted launches, and explicit DLC PKGs
        # never trigger the optional picker.
        if was_v100 and not args.no_dialogs and not args.dlc_pkg:
            optional_dlc = choose_setup(ROOT, 'dlc')
            if optional_dlc:
                install_dlc(ROOT, optional_dlc, pkg_tool=args.pkg_tool,
                            show_window=True)
        # A fresh invocation will detect v1.01 and skip this block, and
        # will discover any DLC installed during the setup step above.
        _restart_after_update(args, game)
        raise RuntimeError('Fresh launcher unexpectedly returned')
    # Normal v1.01 launches never display a DLC setup prompt.
    # Explicit --dlc-pkg and the in-game overlay remain available.
    if args.dlc_pkg:
        added = install_dlc(ROOT, args.dlc_pkg, pkg_tool=args.pkg_tool,
                            show_window=not args.no_dialogs and not args.prepare_only)
        print(f'Installed {len(added)} new DLC package(s).', flush=True)
        if added and not args.prepare_only:
            # Like game updates, do not boot the game in an installer process.
            _restart_after_dlc(args, game)
            raise RuntimeError('Fresh launcher unexpectedly returned after DLC setup')
    out = ROOT / 'out' / title_id
    prepare(game, out)
    link(game, out)
    addons = installed_addons(ROOT, args.dlc)
    prepare_content(game, out, 'full', addons)
    # Persist only after the update, content verification and preparation succeed.
    # The first v1.00 updater invocation restarts before reaching this point.
    _remember_game_folder(remembered, original_game)
    if args.prepare_only:
        return 0
    user_dir = out / 'user'
    gpu_user = user_dir / 'gpu'
    gpu_user.mkdir(parents=True, exist_ok=True)
    env = os.environ.copy()
    # Do not leak compatibility overrides from the caller's shell into a new title.
    for key in ('PS4RT_TRANSITION_SNAPSHOT', 'PS4RT_BC6_FLOAT',
                'PS4RT_WQM_HELPERS', 'PS4RT_ASYNC_SHADERS',
                'PS4RT_OBJECT_MOTION', 'PS4RT_FSR1', 'PS4RT_PARTIAL_HEIGHT_VIEWS',
                'PS4RT_UPSCALER', 'PS4RT_OUTPUT_RES', 'PS4RT_RENDER_RES',
                'PS4RT_UPSCALE_PRESET', 'PS4RT_UPSCALE_BEFORE_CS',
                'PS4RT_UI_TRIGGER_VS', 'PS4RT_PRESET_FILE', 'PS4RT_RCAS',
                'PS4RT_FSR_SHARPNESS', 'PS4RT_JITTER'):
        env.pop(key, None)
    # Start with the known working runtime behavior; future ports adapt it in-source.
    env.update(PS4RT_BC6_FLOAT='unsigned', PS4RT_TRANSITION_SNAPSHOT='1',
               PS4RT_OBJECT_MOTION='0',
               PS4RT_FSR1='0', PS4RT_PARTIAL_HEIGHT_VIEWS='0')
    # Optional menu assets use the same layout as Star Ocean. Caller overrides win.
    art = ROOT / 'gpu/assets/sakura_wars_overlay.png'
    sounds = ROOT / 'gpu/assets/sounds'
    if art.is_file():
        env.setdefault('PS4RT_OVERLAY_BACKGROUND', str(art))
    if sounds.is_dir():
        env.setdefault('PS4RT_MENU_SOUND_DIR', str(sounds))
    # The overlay invokes this same launcher for the optional DLC installer.
    env['PS4RT_DLC_INSTALL_SCRIPT'] = str(Path(__file__).resolve())
    env['PS4RT_ORIGINAL_GAME_DIR'] = str(original_game)
    # The runtime's own Restart Game otherwise re-execs its old --addcont list.
    # Route restart through preflight so freshly installed DLC is discovered.
    restart = [sys.executable, str(Path(__file__).resolve()),
               '--game', str(original_game), '--no-dialogs']
    if args.dlc:
        restart += ['--dlc', str(args.dlc.expanduser().resolve())]
    env['PS4RT_RESTART_COMMAND'] = shlex.join(restart)
    env['PS4RT_GPU_USER_DIR'] = str(gpu_user)
    env['PS4RT_CONFIG'] = str(out / 'ps4rt.ini')
    if args.upscaler is not None:
        env['PS4RT_UPSCALER'] = args.upscaler
    if args.output_resolution is not None:
        env['PS4RT_OUTPUT_RES'] = {
            '1080': '1920x1080', '1440': '2560x1440', '4k': '3840x2160'
        }[args.output_resolution]
    print(f'PS4RT native-port template: {title_id}', flush=True)
    command = [str(runtime), str(out / 'boot-linked.bin'), '--app0', str(game),
               '--user', str(user_dir), '--content-profile', str(out / 'content.bin'),
               '--timeout', '0']
    for label, root in sorted(addons.items()):
        command += ['--addcont', f'{label}={root}']
    os.execve(str(runtime), command, env)


if __name__ == '__main__':
    try:
        sys.exit(main())
    except (OSError, ValueError, KeyError, RuntimeError) as error:
        sys.exit(f'PS4RT: {error}')
