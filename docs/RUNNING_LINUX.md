# Running on Linux

## 1. Install the build dependencies

You need CMake 3.24 or newer, Ninja, a C11/C++23 compiler (GCC or Clang), glslang
(`glslang` or `glslangValidator`), pkg-config and Python 3, plus the development packages for:

- Vulkan (headers and loader)
- SDL3 and X11
- fmt, Boost, tsl-robin-map, xxHash, Zydis
- FFmpeg

Package names differ between distributions; search your package manager for the names
above (development packages usually end in `-devel` or `-dev`).

## 2. Build

From the repository root:

```sh
bash build.sh
```

The runtime is written to `out/ps4rt`, and its renderer library to `out/gpu/`.
`bash build.sh --test` also builds and runs the test suite.

## 3. Prepare your game dump

Use your own **plaintext (decrypted)** extraction of Sakura Wars, title ID
**CUSA16429**, containing `eboot.bin` and `sce_sys/param.sfo`.
The `--game` argument must point to this extracted CUSA directory, **not a
base-game PKG**. No game files are distributed.

The port requires **game update v1.01**. If your extracted game is v1.00,
the launcher offers a graphical setup window to select and apply your own
matching **v1.01 update PKG** using the bundled native extractor. The update
modifies the selected CUSA folder **in place**, replacing its original v1.00
files. **Make a separate backup first** if you need to keep v1.00. After
successful installation, the launcher restarts its normal v1.01 boot path.
If you already have v1.01, it skips this update step.

## 4. Run

```sh
./run.sh --game /path/to/CUSA16429
```

The first launch prepares the game in `out/CUSA16429/` (linked boot image and content
profile). Later launches reuse it and start faster. Optional overrides for a single
launch:

```sh
./run.sh --game /path/to/CUSA16429 --upscaler fsr4 --output-resolution 4k
```

`--upscaler` takes `off`, `fsr3` or `fsr4`; `--output-resolution` takes `1080`, `1440`
or `4k`.

DLC: the first-run setup offers to install your own DLC PKGs and you can also
use the in-game **Game > Install DLC...** button later. New DLC takes effect
after saving and using **Restart Game**. Manually extracted DLC folders remain
supported under `dlc/CUSA16429/`; `--dlc /path` points at a directory
containing per-DLC folders. See [Player update and DLC setup](PLAYER_PKG_SETUP.md)
for details.

## 5. Settings and saves

- Open the in-game PS4RT menu with **Insert** (keyboard) or **R3 + L2** (controller).
  Choices save when the menu closes.
- Settings: `out/CUSA16429/ps4rt.ini`
- Saves and other per-user data: `out/CUSA16429/user/`
- Compiled shader cache: `out/CUSA16429/user/gpu/cache/`. It can be deleted at any time;
  it is rebuilt on the next launch, with some stutter while it fills.

## Optional performance experiment: DrawPipeline

DrawPipeline is **off by default**. On CPU-limited systems, you can try
`PS4RT_DRAW_PIPE=1 ./run.sh --game /path/to/CUSA16429`, but this may
cause intermittent flickering or missing UI/fade draws. To disable it
explicitly, use `PS4RT_DRAW_PIPE=0`. **Only bugs that reproduce with
DrawPipeline disabled are supported**; issues exclusive to the experimental
mode will not be investigated.

## Troubleshooting

- **FSR 4 is not offered in the menu:** your GPU or driver lacks the required INT8
  dot-product support. FSR 3.1 is available instead.
- **"Build the runtime first":** run `bash build.sh` before `./run.sh`.
- **"Expected a PS4 game with CUSAxxxxx TITLE_ID":** `--game` must point at the game
  folder that contains `sce_sys/param.sfo`.
