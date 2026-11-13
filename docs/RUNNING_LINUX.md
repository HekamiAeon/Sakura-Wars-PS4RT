# Running Sakura Wars PS4RT on Linux

Sakura Wars PS4RT is an experimental, game-specific x86-64 Linux compatibility runtime for **CUSA16429 v1.01**. No game files are distributed. Players should use the prebuilt [v1.2 release](https://github.com/HekamiAeon/Sakura-Wars-PS4RT/releases/tag/v1.2); compiling from source is for developers.

## 1. Download and run the portable

1. Download **`Sakura-Wars-PS4RT-b45cfaa-linux-x86_64.tar.gz`** from the [v1.2 release](https://github.com/HekamiAeon/Sakura-Wars-PS4RT/releases/tag/v1.2), not the source archive. Extract it into a writable directory.
2. Provide your own extracted, plaintext **`CUSA16429`** directory containing `eboot.bin`, `sce_sys/param.sfo`, `sce_module/` and game data. Point to that directory itself, not the PKG, its parent, or the executable.
3. Open a terminal inside the extracted player directory and run:

```bash
bash Play-Game.sh --game "/absolute/path/to/CUSA16429"
```

**Later launches:** After successful preparation, run `bash Play-Game.sh` alone. The player remembers the validated path in `out/CUSA16429/game-folder.txt`. Use `--game` again if you move the extraction or select another copy. With no configured path, the launcher prints a first-launch command and points to `PLAYER_README.txt`.

**Game version:** An extracted v1.01 game starts normal preparation. An extracted v1.00 game requires your own matching v1.01 update PKG through the included native setup tool. **The update modifies the selected extracted CUSA16429 directory in place. Back up pristine v1.00 if you need to preserve it.** After a successful fresh update, optional DLC setup can be skipped or completed; the launcher then restarts into v1.01. An already updated v1.01 game does not repeat update/DLC prompts. See [Player update and DLC setup](PLAYER_PKG_SETUP.md).

## 2. Graphics, controls, and optional DLC

Open PS4RT settings with **Insert** on a keyboard or **L2 + R3 click** on a
gamepad. Navigate with the D-pad, confirm with Cross, and go back with Circle.
The full overlay includes **Achievements**; the compact overlay has Graphics
and Achievements tabs (switch with L1/R1 or Q/E).

The current source's **Graphics** menu includes:

- **Internal resolution:** 1024×576 (low-end "540p"), 1280×720, 1920×1080,
  2560×1440, and 3840×2160, plus experimental **21:9** (2560×1080,
  3440×1440), **24:9** (3840×1440) and **32:9** (3840×1080, 5120×1440).
  **Restart required** when changing the internal resolution.
- **FSR Upscale output:** A separate choice from internal resolution. Only
  higher outputs within the same aspect-ratio family and the supported scaling
  range are offered; **Native** keeps the internal dimensions.
- **Upscaling:** **Off / FSR 1 / FSR 3.1 / FSR 4 v07**, depending on GPU
  support. FSR 1 uses spatial scaling without temporal anti-aliasing; FSR 3.1
  and FSR 4 use temporal reconstruction. The 3D scene is reconstructed before
  the HUD is composited. FSR selection and output changes apply live.
- **Graphics presets:** Max (original), High, Medium, and Lowest. High
  disables contact shadows and bloom; Medium reduces anti-aliasing, depth of
  field, dash motion blur, and character screen-space shadows; Lowest also
  disables anti-aliasing, outlines, and depth of field. These apply live.
- **Other controls:** Sharpening, anisotropic filtering, outlines, display
  mode, VSync, FPS/performance overlay, and original 30 FPS or experimental
  60 FPS. **Anisotropic filtering requires a restart.**

**Ultrawide is experimental.** Some HUD markers, dialogue UI, 2D screens and
cutscenes may remain framed for 16:9. A higher FSR output resolution does not
mean the scene itself is rendered at that resolution. Compare visual issues
against 16:9 with FSR Off.

**Game language:** The overlay's dropdown offers **English, French, German,
and Spanish** for the supported EU v1.01 build. Choose a language and use
**Apply and restart**. Japanese text is not present in this build; trophy
text follows the selected console language.

**Local achievements:** The game triggers trophy unlocks; the **Achievements**
tab shows local progress, icons, hidden entries, unlock dates and notifications.
Progress is saved locally and **not synchronized with PSN**. Previously
completed objectives will not unlock retroactively.

FSR 4 requires compatible Vulkan GPU features, including INT8 dot-product
support; use **FSR 3.1, FSR 1, or Off** on unsupported hardware. Choose
internal resolution, FSR output, and upscaler through the in-game overlay.

For later DLC installation, use **Game > Install DLC...** in the overlay with your own compatible PKGs. New DLC loads after a restart. **Save your game before using Restart Game** or quitting, since graphics settings and game progress are saved separately.

## 3. Settings, saves, and private data

Keep the extracted player directory together and writable. Generated files and player data live under `out/CUSA16429/`:

- `out/CUSA16429/game-folder.txt`: remembered game directory.
- `out/CUSA16429/ps4rt.ini`: graphics settings.
- `out/CUSA16429/user/savedata/`: game saves.
- `out/CUSA16429/user/achievements/`: locally earned trophy state and
  generated catalog.
- `out/CUSA16429/user/gpu/cache/`: shader/pipeline caches; deleting a cache forces recompilation.
- `out/CUSA16429/player-content/dlc/`: optional installed DLC.

**Never redistribute `out/`**: it may contain game-derived code, DLC, saves, caches and private player data.

## 4. Build from source (developers only)

A source checkout requires CMake 3.24+, Ninja, a C11/C++23 compiler, pkg-config, Python 3, glslang, and development packages for Vulkan, SDL3/X11, fmt, Boost, tsl-robin-map, xxHash, Zydis, FFmpeg, **Crypto++ and zlib**. Package names differ by distribution. Players of the prebuilt archive need none of these developer tools, a separate PKG extractor, or a special `LD_LIBRARY_PATH`.

```bash
PS4RT_LTO=OFF bash build.sh --test
bash run.sh --game "/absolute/path/to/CUSA16429"
```

Source builds use `out/ps4rt`; the portable uses `bin/ps4rt`. For redistributable archives, follow the audited [portable build instructions](../packaging/linux/README.md) and [distribution guide](DISTRIBUTING_LINUX.md).

## 5. Compatibility and troubleshooting

- **System:** Native Linux x86-64/AMD64, glibc 2.35+, Python 3, Vulkan 1.3-capable drivers and compatible host display/input libraries. ARM64 and CPU translation via FEX or Box64 are unsupported/unverified.
- **Testing:** Fedora gameplay including saves through Chapters 1–3 has been verified; a complete playthrough, other distributions and SteamOS have not been exhaustively tested. The 60 FPS mode remains experimental.
- **DrawPipeline:** Off by default. On CPU-limited systems, try `PS4RT_DRAW_PIPE=1 bash Play-Game.sh`. It may cause flickering or missing UI/fade draws; reproduce bugs with DrawPipeline off before reporting them.
- **No configured game folder:** Supply `--game "/absolute/path/to/CUSA16429"` on first launch, or again if you moved the folder; consult `PLAYER_README.txt`.
- **Interrupted update:** Rerun setup with the same matching v1.01 update PKG. The chosen extracted game directory is modified in place; see [update and DLC setup](PLAYER_PKG_SETUP.md).
- **FSR 4 unavailable:** Your GPU/driver may lack the required INT8 dot-product features. Use FSR 3.1, FSR 1, or Off instead.
- **Ultrawide UI misplaced:** Compare the same scene at 16:9 with FSR Off; record internal resolution, FSR output, upscaler, scene, and a screenshot.
- **Source checkout says runtime not built:** Run `bash build.sh`, or download the prebuilt player archive.

For bug reports and feature notes, see the [main README](../README.md) or [GitHub Issues](https://github.com/HekamiAeon/Sakura-Wars-PS4RT/issues).
