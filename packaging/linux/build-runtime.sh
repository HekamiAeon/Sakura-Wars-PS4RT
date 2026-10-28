#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0-or-later
# Game-neutral starter for glibc 2.35 ELF bundles from committed source.
# Reuses the proven Ubuntu 22.04 GCC14 / FFmpeg8 developer image.
# Game-specific launchers, media and release packaging remain each title's job.
set -euo pipefail

fail() { printf 'ERROR: %s\n' "$*" >&2; exit 1; }
ROOT="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/../.." && pwd -P)"
cd "$ROOT"
[[ -f build.sh && -f gpu/CMakeLists.txt ]] || fail 'Run in a PS4RT port checkout with build.sh and gpu/CMakeLists.txt.'
for cmd in git podman tar patchelf readelf ldd python3; do
  command -v "$cmd" >/dev/null || fail "Missing required command: $cmd"
done

IMAGE='localhost/ps4rt-builder:linux-pkg'
podman image exists "$IMAGE" || fail "PKG-enabled builder missing. First build the common image with packaging/linux/build-images.sh, then run packaging/linux/build-pkg-image.sh"

# One predictable, disposable build workspace per independent game repository.
# NEVER delete arbitrary directories. Only reset one that has our sentinel.
DEFAULT_PARENT="$HOME/Documents/PS4RT-Builds"
PARENT="${PS4RT_LINUX_WORK_PARENT:-$DEFAULT_PARENT}"
PROJECT="${PS4RT_BUILD_ID:-$(basename "$ROOT")}"
[[ "$PROJECT" =~ ^[a-zA-Z0-9][a-zA-Z0-9._-]{0,79}$ ]] ||
    fail "Unsafe project folder name. Set PS4RT_BUILD_ID to a short alphanumeric ID."
mkdir -p -- "$PARENT"
WORK="$PARENT/$PROJECT"
MARKER="$WORK/.ps4rt-build-workspace"
if [[ -e "$WORK" || -L "$WORK" ]]; then
  [[ -d "$WORK" && ! -L "$WORK" && -f "$MARKER" && ! -L "$MARKER" ]] ||
    fail "Refusing to reuse unrecognized build directory: $WORK"
  [[ "$(cat "$MARKER")" == "PS4RT-BUILD:$PROJECT" ]] ||
    fail "Build directory marker mismatch: $MARKER"
  for entry in source bundle; do
    [[ ! -L "$WORK/$entry" ]] || fail "Refusing linked work path: $WORK/$entry"
    rm -rf -- "$WORK/$entry"
  done
  rm -f -- "$WORK/build.log" "$WORK/elf-audit.txt" "$WORK/source-revision.txt"
else
  mkdir -- "$WORK"
  printf 'PS4RT-BUILD:%s\n' "$PROJECT" > "$MARKER"
fi
SOURCE="$WORK/source"
STAGE="$WORK/bundle"
mkdir -p "$SOURCE" "$STAGE/bin/gpu" "$STAGE/lib"
REVISION="$(git rev-parse HEAD)"
printf '%s\n' "$REVISION" > "$WORK/source-revision.txt"

# Building a git archive ensures generated linked game executables, saves, and
# shader caches never enter the container or its output. Source is HEAD only.
# The same explicit allowlist protects build input and final source releases.
python3 "$ROOT/packaging/linux/release.py" export-source --destination "$SOURCE"
[[ -f "$SOURCE/build.sh" && -f "$SOURCE/gpu/CMakeLists.txt" ]] || fail 'Committed source archive is incomplete.'

echo "=== PS4RT PORTABLE LINUX BUILD ==="
echo "Source revision: $REVISION"
echo "Builder: $IMAGE"
echo "Workspace: $WORK"
echo "Log: $WORK/build.log"
echo "Reuse policy: only this project's sentinel-marked build outputs are replaced."
echo "The checkout and previous working bundles remain unchanged."

# Compiled from the committed source, using PS4RT_PORTABLE_BUILD for the
# Ubuntu toolchain. Game code and the user's original checkout are unchanged.

# Feed the container script on stdin; -i is required for its heredoc.
if ! podman run --rm -i --security-opt label=disable \
     --env PS4RT_BUILD_JOBS="${PS4RT_BUILD_JOBS:-4}" \
     --mount "type=bind,source=$SOURCE,target=/work/source" \
     --mount "type=bind,source=$STAGE,target=/stage" \
     "$IMAGE" bash -s >"$WORK/build.log" 2>&1 <<'CONTAINER'; then
set -euo pipefail
cd /work/source
export CXXFLAGS="${CXXFLAGS:+$CXXFLAGS }-idirafter /usr/include"
# Needed for libvulkan.so and nested dependencies in the older Ubuntu sysroot.
export LDFLAGS="${LDFLAGS:+$LDFLAGS }-L/usr/lib/x86_64-linux-gnu -Wl,-rpath-link,/opt/ps4rt-deps/lib"
export PS4RT_LTO=OFF PS4RT_PGO=off PS4RT_PORTABLE_BUILD=1
command -v "$CC" >/dev/null
command -v "$CXX" >/dev/null

echo '=== COMPILE MAIN + GPU ==='
bash build.sh
# Optional: older or game-specific ports may not include this test target.
if ninja -C out/gpu -t targets all | grep -q '^menu-sounds-test:'; then
    ninja -j "${PS4RT_BUILD_JOBS:-4}" -C out/gpu menu-sounds-test
    out/gpu/menu-sounds-test
fi
[[ -f out/ps4rt && -f out/gpu/libps4rtgpu.so && -f out/ps4rt-pkg && -f out/ps4rt-setup ]] \
  || { echo 'Missing required Sakura Wars build output'; exit 1; }
install -Dm755 out/ps4rt /stage/bin/ps4rt
install -Dm755 out/ps4rt-pkg /stage/bin/ps4rt-pkg
install -Dm755 out/ps4rt-setup /stage/bin/ps4rt-setup
install -Dm755 out/gpu/libps4rtgpu.so /stage/bin/gpu/libps4rtgpu.so
if [[ -f out/ps4rt-gpu-capabilities ]]; then
  install -Dm755 out/ps4rt-gpu-capabilities /stage/bin/ps4rt-gpu-capabilities
fi

# Preserve the verified 12-library core, plus the exact linked Crypto++ SONAME.
# Collect via the builder's own
# linker resolution, never via random host-installed Fedora libraries.
CXX_LIB="$($CXX -print-file-name=libstdc++.so.6)"
GCC_LIB="$($CXX -print-file-name=libgcc_s.so.1)"
export CXX_LIB GCC_LIB
[[ -f "$CXX_LIB" && -f "$GCC_LIB" ]]
export LD_LIBRARY_PATH="$(dirname "$CXX_LIB"):/opt/ps4rt-deps/lib${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"

python3 - <<'PY'
import os, re, shutil, subprocess
from pathlib import Path
bundle = Path('/stage')
required = {
    'libavcodec.so.62','libavformat.so.62','libavutil.so.60',
    'libfmt.so.11','libgcc_s.so.1','libSDL3.so.0','libstdc++.so.6',
    'libswresample.so.6','libswscale.so.9',
    'libxxhash.so.0','libz.so.1','libZydis.so.5.0',
}
provided = {'libstdc++.so.6':os.environ['CXX_LIB'],
            'libgcc_s.so.1':os.environ['GCC_LIB']}
for binary in (bundle/'bin/ps4rt',bundle/'bin/gpu/libps4rtgpu.so',
               bundle/'bin/ps4rt-pkg',bundle/'bin/ps4rt-setup'):
    result = subprocess.run(['ldd',str(binary)],capture_output=True,text=True)
    if result.returncode or 'not found' in result.stdout:
        raise SystemExit('Unable to resolve in builder: '+str(binary)+'\n'+result.stdout+result.stderr)
    for line in result.stdout.splitlines():
        match = re.match(r'\s*(\S+)\s+=>\s+(/\S+)\s+\(', line)
        if match:
            name,path = match.groups()
            if re.fullmatch(r'libcrypto(?:\+\+|pp)\.so\.\d+',name):
                required.add(name)
            if name in required and name not in provided:
                provided[name] = path
if len([x for x in required if re.fullmatch(r'libcrypto(?:\+\+|pp)\.so\.\d+',x)]) != 1:
    raise SystemExit('Expected exactly one linked Crypto++ SONAME for ps4rt-pkg')
missing=required-set(provided)
if missing:
    raise SystemExit('Missing release libraries in builder: '+', '.join(sorted(missing)))
for name in sorted(required):
    path=Path(provided[name]).resolve(strict=True)
    dst=bundle/'lib'/name
    shutil.copy2(path,dst)
    if not dst.is_file() or dst.stat().st_size==0:
        raise SystemExit('Empty library: '+str(dst))
    print(name,'<-',path)
PY
CONTAINER
  tail -n 40 "$WORK/build.log" >&2
  fail "Container source build failed. Read $WORK/build.log; nothing was published or overwritten."
fi

# Copy only allowlisted, committed, template-owned resources. The game
# packager still decides which assets are legally redistributable.
for name in scripts LICENSE fsr4_shaders; do
  if [[ -e "$SOURCE/$name" && ! -L "$SOURCE/$name" ]]; then
    cp -a -- "$SOURCE/$name" "$STAGE/"
  fi
done
for asset in sakura_wars_overlay.png sounds/scroll.wav sounds/open.wav sounds/close.wav; do
  origin="$SOURCE/gpu/assets/$asset"
  dest="$STAGE/gpu/assets/$asset"
  if [[ -f "$origin" && ! -L "$origin" ]]; then
    mkdir -p -- "$(dirname "$dest")"
    cp -- "$origin" "$dest"
  fi
done
# The standalone graphical update selector can use our licensed font.
if [[ -f "$SOURCE/gpu/third_party/fonts/DejaVuSans.ttf" ]]; then
  install -Dm644 "$SOURCE/gpu/third_party/fonts/DejaVuSans.ttf" \
    "$STAGE/gpu/third_party/fonts/DejaVuSans.ttf"
fi
# Carry only the known committed generated FSR 3 Vulkan shader files.
shader_root="$SOURCE/gpu/third_party/fsr-vulkan/generated"
if [[ -d "$shader_root" ]]; then
  while IFS= read -r -d '' shader; do
    rel="${shader#"$SOURCE/"}"
    mkdir -p -- "$STAGE/$(dirname "$rel")"
    cp -- "$shader" "$STAGE/$rel"
  done < <(find "$shader_root" -type f -name '*.spv' -print0)
fi
# No arbitrary local/uncommitted media, original PS4 files, saves or caches.
# Launcher integration and final redistribution remain per-game decisions.
patchelf --set-rpath '$ORIGIN/../lib:$ORIGIN/gpu' "$STAGE/bin/ps4rt"
patchelf --set-rpath '$ORIGIN/../lib' "$STAGE/bin/ps4rt-pkg"
patchelf --set-rpath '$ORIGIN/../lib' "$STAGE/bin/ps4rt-setup"
patchelf --set-rpath '$ORIGIN/../../lib' "$STAGE/bin/gpu/libps4rtgpu.so"
if [[ -f "$STAGE/bin/ps4rt-gpu-capabilities" ]]; then
  patchelf --set-rpath '$ORIGIN/../lib' "$STAGE/bin/ps4rt-gpu-capabilities"
fi
while IFS= read -r -d '' so; do
  patchelf --set-rpath '$ORIGIN' "$so"
done < <(find "$STAGE/lib" -maxdepth 1 -type f -name '*.so*' -print0)

# Verify the exact packaged-binary rules without requiring a Git commit.
# A live ldd check still uses the host Vulkan loader and glibc as intended.
STAGE="$STAGE" python3 - <<'PY' | tee "$WORK/elf-audit.txt"
import os,re,subprocess
from pathlib import Path
p=Path(os.environ['STAGE'])
needed=['bin/ps4rt','bin/gpu/libps4rtgpu.so','bin/ps4rt-pkg','bin/ps4rt-setup']
if (p/'bin/ps4rt-gpu-capabilities').exists():
    needed.append('bin/ps4rt-gpu-capabilities')
crypto=[x.name for x in (p/'lib').iterdir()
        if re.fullmatch(r'libcrypto(?:\+\+|pp)\.so\.\d+',x.name)]
if len(crypto)!=1:
    raise SystemExit('Missing or ambiguous bundled Crypto++ SONAME')
needed.append('lib/'+crypto[0])
needed += ['lib/'+x for x in (
 'libavcodec.so.62','libavformat.so.62','libavutil.so.60',
 'libfmt.so.11','libgcc_s.so.1','libSDL3.so.0','libstdc++.so.6',
 'libswresample.so.6','libswscale.so.9',
 'libxxhash.so.0','libz.so.1','libZydis.so.5.0')]
expected={'bin/ps4rt':'$ORIGIN/../lib:$ORIGIN/gpu',
           'bin/ps4rt-pkg':'$ORIGIN/../lib',
           'bin/ps4rt-setup':'$ORIGIN/../lib',
          'bin/gpu/libps4rtgpu.so':'$ORIGIN/../../lib',
          'bin/ps4rt-gpu-capabilities':'$ORIGIN/../lib'}
for name in needed:
    elf=p/name
    if not elf.is_file():raise SystemExit('Missing ELF '+name)
    rpath=subprocess.check_output(['patchelf','--print-rpath',str(elf)],text=True).strip()
    if rpath!=expected.get(name,'$ORIGIN'):
        raise SystemExit(f'Bad RUNPATH: {name}: {rpath}')
    output=subprocess.check_output(['readelf','-W','--version-info',str(elf)],text=True)
    nums=[tuple(map(int,n.split('.'))) for n in re.findall(r'GLIBC_([0-9]+(?:\.[0-9]+)+)',output)]
    maximum=max(nums) if nums else None
    if maximum and maximum>(2,35):
        raise SystemExit(f'glibc too new: {name}: {maximum}')
    print(name,'maximum glibc:', '.'.join(map(str,maximum)) if maximum else 'none', 'RUNPATH:',rpath)
env=os.environ.copy()
env.pop('LD_LIBRARY_PATH',None); env.pop('LD_PRELOAD',None)
for name in [n for n in needed if n.startswith('bin/')]:
    r=subprocess.run(['ldd',str(p/name)],capture_output=True,text=True,env=env)
    if r.returncode or 'not found' in (r.stdout+r.stderr):
        raise SystemExit('Host dependency resolution failed for '+name+'\n'+r.stdout+r.stderr)
    for line in r.stdout.splitlines():
        m=re.match(r'\s*(\S+)\s+=>\s+(/\S+)\s+\(',line)
        if m and (p/'lib'/m.group(1)).exists():
            if Path(m.group(2)).resolve()!=(p/'lib'/m.group(1)).resolve():
                raise SystemExit('Bundled dependency escaped bundle: '+line)
    print('HOST RESOLUTION PASS:',name)
print('ELF BASELINE + RELATIVE PATHS: PASS')
PY

echo
echo '=== PORTABLE ELF BUNDLE READY FOR GAME-SPECIFIC TESTING ==='
echo "BUNDLE=$STAGE"
echo "BUILD_LOG=$WORK/build.log"
echo "ELF_AUDIT=$WORK/elf-audit.txt"
echo 'Runtime stage complete. Use build-release.sh for archives, then inspect and play-test.'
