#!/usr/bin/env bash
set -euo pipefail
cd -- "$(dirname -- "$0")"
command -v cmake >/dev/null
command -v ninja >/dev/null
pkg-config --exists vulkan sdl3 libavcodec libavutil libswscale
# Build player-owned native PKG extraction and the bundled setup UI.
# No player-side package manager, AppImage, or external PKG extractor needed.
bash scripts/build_pkg_native.sh
bash scripts/build_setup.sh
cmake -S gpu -B out/gpu -G Ninja -DCMAKE_BUILD_TYPE=Release -DPS4RT_PGO=off -DPS4RT_LTO="${PS4RT_LTO:-OFF}"
ninja -C out/gpu -j "${PS4RT_BUILD_JOBS:-4}" ps4rtgpu
read -r -a includes <<< "$(pkg-config --cflags vulkan sdl3 libavcodec libavutil libswscale)"
read -r -a libraries <<< "$(pkg-config --libs vulkan sdl3 libavcodec libavutil libswscale)"
# Portable Linux builds use the same GCC14/Jammy sysroot as PS4RT-Lab.
portable_libs=()
if [[ ${PS4RT_PORTABLE_BUILD:-0} == 1 ]]; then
    [[ -n ${CXX:-} ]] || { echo 'PS4RT_PORTABLE_BUILD requires CXX' >&2; exit 1; }
    portable_libs=(-L/usr/lib/x86_64-linux-gnu
        -Wl,-rpath-link,/opt/ps4rt-deps/lib
        -Wl,-rpath-link,/usr/lib/x86_64-linux-gnu
        -Wl,-rpath-link,/lib/x86_64-linux-gnu
        "$("$CXX" -print-file-name=libstdc++.so.6)")
fi
compiler=${CC:-cc}
mkdir -p out/atrac9
for source in third_party/LibAtrac9/C/src/*.c; do
    "$compiler" -std=c99 -O2 -w -c "$source" -o "out/atrac9/$(basename "${source%.c}").o"
done
ar rcs out/libatrac9.a out/atrac9/*.o
"$compiler" -std=c11 -O2 -Wall -Wextra -Werror -mgeneral-regs-only -c src/isa_emulate.c -o out/isa_emulate.o
"$compiler" -std=c11 -O2 -Wall -Wextra -Werror -pthread -no-pie -DPS4RT_RUNTIME "${includes[@]}" -I. -Isrc \
    src/loader.c src/runtime*.c src/cpu_check.c out/isa_emulate.o src/isa_rewrite.c out/libatrac9.a \
    -lm -Lout/gpu -lps4rtgpu -Wl,-rpath,'$ORIGIN/gpu' -rdynamic "${libraries[@]}" "${portable_libs[@]}" -o out/ps4rt
if [[ ${1:-} == --test ]]; then
    ninja -C out/gpu -j "${PS4RT_BUILD_JOBS:-4}" ui-coordinates-gpu-test display-sample-gpu-test transition-snapshot-gpu-test \
        cpu-image-writeback-test color-export-test image-subresource-test gpu-workarounds-test \
        camera-layout-test scene-camera-history-test menu-input-test graphics-settings-test framerate-test sakura-camera-test
    "$compiler" -std=c11 -O2 -Wall -Wextra -Werror -pthread "${includes[@]}" -I. -Isrc \
        tests/test_content.c src/runtime_content.c -o out/content-test
    "$compiler" -std=c11 -O2 -Wall -Wextra -Werror -pthread -I. -Isrc \
        tests/test_thread.c src/runtime_thread.c -o out/thread-test
    "$compiler" -std=c11 -O2 -Wall -Wextra -Werror -pthread -I. -Isrc \
        tests/test_direct_memory.c src/runtime_memory.c -o out/direct-memory-test
    "$compiler" -std=c11 -O2 -Wall -Wextra -Werror -pthread -I. -Isrc \
        tests/test_videodec2.c src/runtime_videodec2.c -o out/videodec2-test
    "$compiler" -std=c11 -O2 -Wall -Wextra -Werror -pthread -ffunction-sections -fdata-sections -I. -Isrc \
        tests/test_save_dialog.c -Wl,--gc-sections -o out/save-dialog-test
    ctest --test-dir out/gpu --output-on-failure
    python3 -m unittest discover -s tests -p 'test_*.py' -v
fi
