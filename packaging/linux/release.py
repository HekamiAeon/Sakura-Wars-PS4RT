#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Sanitized source exporter and portable Linux release packager.

Use the SAME committed-source filter for container input and public archives.
Never recursively package a development directory or original PS4 files.
"""
import argparse
import hashlib
import io
import json
import os
from pathlib import Path, PurePosixPath
import re
import subprocess
import tarfile

ROOT = Path(__file__).resolve().parents[2]
ROOT_FILES = {'README.md', 'LICENSE', 'build.sh', 'run.sh', '.gitignore', '.gitattributes'}
ROOT_DIRS = {'src', 'gpu', 'scripts', 'third_party', 'tests', 'tools', 'packaging',
             'fsr4_shaders', 'patches'}
PUBLIC_DOCS = {'docs/RUNNING_LINUX.md', 'docs/DISTRIBUTING_LINUX.md',
               'docs/UPSTREAM_CREDITS.md', 'docs/LINUX_BINARY_RELEASE.md',
               'docs/THIRD_PARTY_REVIEW.md', 'docs/PLAYER_PKG_SETUP.md'}
# The public README embeds this reviewed screenshot; no other screenshots are exported.
PUBLIC_MEDIA = {'docs/images/sample.png'}
PRIVATE_PARTS = {'.git', '.agents', '.codex', '.ghidra', 'agents', 'references',
                 'reference', 'original', 'analysis', 'savedata', 'saves', 'user',
                 'out', 'build', 'dist', 'cache', 'caches', 'captures',
                 'screenshots', 'frame-dump', 'cmakefiles', '__pycache__',
                 'templates', 'private', 'secrets', 'credentials', 'game-dumps'}
PRIVATE_NAMES = {'agents.md', 'claude.md', 'lab_readme.md', 'automated_porting.md',
                 'migration_notes.md', 'porting_log.md'}
PRIVATE_PREFIXES = ('porting_log', 'resume', 'next_session', 'handoff',
                    'continuity', 'private_', 'secret_', 'credentials', 'agent_notes')
PRIVATE_EXTS = {'.pkg', '.prx', '.sprx', '.self', '.elf', '.sfo', '.cpk', '.usm',
                '.g1t', '.g1m', '.at9', '.pfs', '.iso', '.o', '.obj', '.a', '.so',
                '.dll', '.exe', '.log', '.trace', '.dump', '.cache', '.pem',
                '.key', '.p12', '.pfx', '.pyc', '.sqlite', '.db', '.zip', '.tar',
                '.gz', '.7z', '.mp4', '.mkv', '.mov'}
APP_LIBS = ('libavcodec.so.62', 'libavformat.so.62', 'libavutil.so.60',
            'libfmt.so.11', 'libgcc_s.so.1', 'libSDL3.so.0', 'libstdc++.so.6',
            'libswresample.so.6', 'libswscale.so.9', 'libxxhash.so.0',
            'libz.so.1', 'libZydis.so.5.0')
BINS = {'bin/ps4rt': '$ORIGIN/../lib:$ORIGIN/gpu',
        'bin/gpu/libps4rtgpu.so': '$ORIGIN/../../lib',
        'bin/ps4rt-pkg': '$ORIGIN/../lib',
        'bin/ps4rt-setup': '$ORIGIN/../lib'}
PLAYER = {'run.sh', 'scripts/ps4rt_launch.py', 'scripts/prepare.py',
          'scripts/link_modules.py', 'scripts/link_libc.py',
          'scripts/content_profile.py', 'scripts/player_setup.py',
          'scripts/patch_assembly.py'}
MEDIA = {'gpu/assets/sakura_wars_overlay.png', 'gpu/assets/sounds/scroll.wav',
         'gpu/assets/sounds/open.wav', 'gpu/assets/sounds/close.wav',
         'gpu/third_party/fonts/DejaVuSans.ttf'}
# Six metadata manifests accompanying the reviewed FSR4 model assets.
FSR4_MANIFESTS = {
    f'fsr4_shaders/fsr4_model_v07_i8_{preset}_shader_manifest.json'
    for preset in ('native', 'quality', 'balanced', 'performance', 'ultraperf', 'drs')
}
REQUIRED = PLAYER | {'build.sh', 'gpu/CMakeLists.txt', 'README.md', 'LICENSE'}


def fail(message):
    raise SystemExit('PS4RT release: ' + message)


def digest(data):
    return hashlib.sha256(data).hexdigest()


def safe_name(name):
    p = PurePosixPath(name)
    if p.is_absolute() or not p.parts or '..' in p.parts:
        return False
    if any(part.lower() in PRIVATE_PARTS for part in p.parts):
        return False
    filename = p.name.lower()
    return not (filename in PRIVATE_NAMES or
                any(filename.startswith(v) for v in PRIVATE_PREFIXES) or
                filename.startswith(('.env', 'id_rsa', 'id_ed25519')))


def source_allowed(name):
    if not safe_name(name):
        return False
    p = PurePosixPath(name)
    parts = p.parts
    # FSR4 v07 requires model weight .bin files plus auxiliary SPIR-V passes.
    # Restrict this binary exception to these exact reviewed shader filenames.
    if parts[0] == 'fsr4_shaders' and len(parts) == 2:
        return (name in FSR4_MANIFESTS or
                (p.name.startswith('fsr4_model_v07_') and
                 p.suffix.lower() in ('.spv', '.bin')) or
                p.name in ('rcas.spv', 'spd_auto_exposure.spv') or
                p.name.lower() in ('readme.md', 'license', 'license-fsr4-v07.txt'))
    if p.suffix.lower() in PRIVATE_EXTS:
        return False
    if name in ROOT_FILES or name in PUBLIC_DOCS or name in PUBLIC_MEDIA or name.startswith('docs/licenses/'):
        return True
    if len(parts) < 2 or parts[0] not in ROOT_DIRS:
        return False
    if parts[:2] == ('tools', 'lab'):
        return False
    if parts[:2] == ('gpu', 'assets'):
        return name in MEDIA or name == 'gpu/assets/README.md'
    # CMake embeds this reviewed Bitstream/DejaVu licensed font into the GPU .so.
    if name == 'gpu/third_party/fonts/DejaVuSans.ttf':
        return True
    # Proven FSR 3 Vulkan shader assets have their own restricted location.
    if name.startswith('gpu/third_party/fsr-vulkan/generated/') and p.suffix == '.spv':
        return True
    if p.suffix.lower() in ('.bin', '.png', '.jpg', '.jpeg', '.webp',
                             '.wav', '.ttf', '.ico', '.spv'):
        return False
    return True


def source_at_head(repo=ROOT):
    revision = subprocess.check_output(
        ['git', '-C', str(repo), 'rev-parse', 'HEAD'], text=True).strip()
    # Scope Git itself to approved roots. Do NOT stream archived references,
    # private onboarding trees or history into Python just to discard them.
    tracked = subprocess.check_output(
        ['git', '-C', str(repo), 'ls-tree', '-r', '-z', '--name-only', 'HEAD'])
    paths = {item.decode() for item in tracked.split(b'\0') if item}
    candidates = ROOT_FILES | ROOT_DIRS | PUBLIC_DOCS | PUBLIC_MEDIA | {'docs/licenses'}
    selected = [p for p in sorted(candidates)
                if p in paths or any(name.startswith(p + '/') for name in paths)]
    raw = subprocess.check_output(
        ['git', '-C', str(repo), 'archive', '--format=tar', 'HEAD', *selected])
    files = {}
    with tarfile.open(fileobj=io.BytesIO(raw), mode='r:') as archive:
        for member in archive:
            if member.isdir() or not source_allowed(member.name):
                continue
            if not member.isfile():
                fail('allowed source contains symlink/submodule: ' + member.name)
            files[member.name] = (archive.extractfile(member).read(),
                                  0o755 if member.mode & 0o111 else 0o644)
    if not REQUIRED.issubset(files):
        fail('missing committed source: ' + ', '.join(sorted(REQUIRED - files.keys())))
    return revision, files


def export_source(destination, repo=ROOT):
    if destination.is_symlink() or not destination.is_dir() or any(destination.iterdir()):
        fail('source destination must be an empty real directory')
    rev, files = source_at_head(repo)
    for name, (data, mode) in sorted(files.items()):
        target = destination / name
        target.parent.mkdir(parents=True, exist_ok=True)
        target.write_bytes(data)
        target.chmod(mode)
    print('Filtered build source:', len(files), 'files at', rev)


def verify_bundle(bundle):
    if bundle.is_symlink() or not bundle.is_dir():
        fail('missing or linked build bundle')
    expected = dict(BINS)
    optional = 'bin/ps4rt-gpu-capabilities'
    if (bundle / optional).is_file():
        expected[optional] = '$ORIGIN/../lib'
    # Crypto++ SONAME is distro-specific. Audit the exact one linked by this build,
    # rather than assuming a developer host's version is universally available.
    crypto = [p.name for p in (bundle / 'lib').iterdir()
              if re.fullmatch(r'libcrypto(?:\+\+|pp)\.so\.\d+', p.name)]
    if len(crypto) != 1:
        fail('missing or ambiguous Crypto++ shared library in portable bundle')
    crypto_name = crypto[0]
    needed = subprocess.check_output(
        ['readelf', '-d', str(bundle / 'bin/ps4rt-pkg')], text=True)
    if crypto_name not in needed:
        fail('bundled Crypto++ SONAME does not match the extractor binary')
    expected.update({'lib/' + name: '$ORIGIN' for name in APP_LIBS})
    expected['lib/' + crypto_name] = '$ORIGIN'
    for name, rpath in expected.items():
        p = bundle / name
        if p.is_symlink() or not p.is_file():
            fail('missing/symlinked ELF: ' + name)
        header = p.open('rb').read(20)
        if len(header) < 20 or header[:6] != b'\x7fELF\x02\x01' or int.from_bytes(header[18:20], 'little') != 62:
            fail('not an x86-64 ELF: ' + name)
        actual = subprocess.check_output(['patchelf', '--print-rpath', str(p)], text=True).strip()
        if actual != rpath:
            fail('unexpected RUNPATH: ' + name)
        versions = subprocess.check_output(['readelf', '-W', '--version-info', str(p)], text=True)
        requirements = [tuple(map(int, v.split('.'))) for v in re.findall(r'GLIBC_(\d+(?:\.\d+)+)', versions)]
        if requirements and max(requirements) > (2, 35):
            fail('glibc requirement >2.35: ' + name)
    env = os.environ.copy()
    env.pop('LD_LIBRARY_PATH', None)
    env.pop('LD_PRELOAD', None)
    for name in BINS.keys() | ({optional} if optional in expected else set()):
        result = subprocess.run(['ldd', str(bundle / name)], capture_output=True, text=True, env=env)
        if result.returncode or 'not found' in result.stdout + result.stderr:
            fail('host dependency check failed: ' + name + '\n' + result.stdout + result.stderr)
        for line in result.stdout.splitlines():
            match = re.match(r'\s*(\S+)\s+=>\s+(/\S+)\s+\(', line)
            if match and match.group(1) in (*APP_LIBS, crypto_name):
                if Path(match.group(2)).resolve() != (bundle / 'lib' / match.group(1)).resolve():
                    fail('dependency escaped bundle: ' + line)
    return expected


def runtime_entries(source, bundle, audited):
    result = {p: source[p] for p in PLAYER}
    result.update({p: data for p, data in source.items()
                   if p in MEDIA or p.startswith('fsr4_shaders/') or
                   (p.startswith('gpu/third_party/fsr-vulkan/generated/') and p.endswith('.spv')) or
                   p in ('LICENSE', 'gpu/third_party/fonts/LICENSE-DejaVu.txt',
                          'third_party/ps4pkg/LICENSE', 'third_party/ps4pkg/README.md')})
    for name in audited:
        result[name] = ((bundle / name).read_bytes(),
                        0o755 if name.startswith('bin/') else 0o644)
    launcher = (b'#!/usr/bin/env bash\nset -euo pipefail\n'
                b'cd -- "$(dirname -- "$0")"\nexec bash ./run.sh "$@"\n')
    result['Play-Game.sh'] = (launcher, 0o755)
    result['PLAYER_README.txt'] = (
        b'PS4RT Linux x86-64 (glibc 2.35+)\n'
        b'Extract and run: bash Play-Game.sh --game "/path/to/CUSA16429"\n'
        b'This port requires game update v1.01. Select your own update PKG when prompted.\n'
        b'Optional player-owned DLC can be installed with the bundled setup interface.\n'
        b'Use your own legally obtained extracted PS4 data.\n'
        b'Full instructions and troubleshooting: https://github.com/HekamiAeon/Sakura-Wars-PS4RT\n'
        b'No game dumps, user saves or private developer documents included.\n', 0o644)
    return result


def write_archive(target, stem, revision, kind, files):
    if target.exists() or target.is_symlink() or target.with_name(target.name + '.part').exists():
        fail('refusing existing archive: ' + str(target))
    payload = dict(files)
    payload['MANIFEST.json'] = ((json.dumps({
        'revision': revision, 'kind': kind,
        'files': {n: digest(data) for n, (data, _) in sorted(files.items())}
    }, indent=2, sort_keys=True) + '\n').encode(), 0o644)
    part = target.with_name(target.name + '.part')
    with tarfile.open(part, 'w:gz') as archive:
        for name, (data, mode) in sorted(payload.items()):
            if not safe_name(name):
                fail('forbidden archive entry: ' + name)
            member = tarfile.TarInfo(stem + '/' + name)
            member.uid = member.gid = 0
            member.uname = member.gname = ''
            member.mode = mode
            member.size = len(data)
            archive.addfile(member, io.BytesIO(data))
    part.rename(target)
    target.with_name(target.name + '.sha256').write_text(
        digest(target.read_bytes()) + '  ' + target.name + '\n')
    print('Wrote archive and SHA256:', target)


def package(output, project, bundle, repo=ROOT):
    if not re.fullmatch(r'[A-Za-z0-9][A-Za-z0-9._-]{0,79}', project):
        fail('invalid project name')
    if subprocess.run(['git', '-C', str(repo), 'diff', '--quiet', 'HEAD']).returncode:
        fail('commit tracked source changes before release')
    revision, source = source_at_head(repo)
    audited = verify_bundle(bundle)
    runtime = runtime_entries(source, bundle, audited)
    output.mkdir(parents=True, exist_ok=True)
    if output.is_symlink():
        fail('symlinked release directory')
    prefix = project + '-' + revision[:7]
    targets = [output / (prefix + '-source.tar.gz'),
               output / (prefix + '-linux-x86_64.tar.gz')]
    for path in targets:
        if (path.exists() or path.is_symlink() or
            path.with_name(path.name + '.sha256').exists() or
            path.with_name(path.name + '.part').exists()):
            fail('release asset exists; refusing overwrite: ' + str(path))
    write_archive(targets[0], prefix + '-source', revision, 'source', source)
    write_archive(targets[1], prefix + '-linux-x86_64', revision, 'linux-x86_64', runtime)
    print('Four release assets created, not published. Inspect and play-test before release.')


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    sub = parser.add_subparsers(dest='action', required=True)
    export = sub.add_parser('export-source')
    export.add_argument('--destination', type=Path, required=True)
    pack = sub.add_parser('package')
    pack.add_argument('--bundle-from', type=Path, required=True)
    pack.add_argument('--output', type=Path, required=True)
    pack.add_argument('--name', required=True)
    args = parser.parse_args()
    if args.action == 'export-source':
        export_source(args.destination)
    else:
        package(args.output, args.name, args.bundle_from)


if __name__ == '__main__':
    main()
