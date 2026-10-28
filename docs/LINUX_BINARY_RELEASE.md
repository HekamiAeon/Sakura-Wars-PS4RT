# Sakura Wars PS4RT: Linux x86-64 release

**For players: download the portable Linux runtime, extract it, and point the launcher at your own game. No compilation, separate PKG extractor, or manual FFmpeg/SDL3 installation is required.**

## 1. Download the player archive

Open the [latest Sakura Wars PS4RT release](https://github.com/HekamiAeon/Sakura-Wars-PS4RT/releases/latest).

Choose the asset matching **`Sakura-Wars-PS4RT-*-linux-x86_64.tar.gz`**. This is the playable Linux runtime. The separately named **`Sakura-Wars-PS4RT-*-source.tar.gz`** is corresponding source for developers, not the game runtime. Each archive has a companion `.sha256` file. Do not confuse these with GitHub's automatically generated “Source code” downloads.

To check a downloaded runtime archive, put its matching `.sha256` next to it and run (in the download directory):

```bash
sha256sum -c Sakura-Wars-PS4RT-*-linux-x86_64.tar.gz.sha256
```

Use only one matching release archive at a time when using these wildcard commands.

## 2. Supply your own extracted PS4 game

The game is **Sakura Wars, title ID CUSA16429**. Supply an extracted, *plaintext* game directory with a structure like:

```text
CUSA16429/
  eboot.bin
  sce_sys/param.sfo
  sce_module/
  (other game data)
```

Point the launcher to `CUSA16429` itself, **not** a PKG, `eboot.bin`, or its parent directory. Both base **v1.00** and already-updated **v1.01** are accepted as inputs, but **v1.01 is required to play**. You must provide your own matching v1.01 update PKG if starting from v1.00. DLC is optional.

Neither the runtime nor the source archive includes PS4 game data, updates, DLC, firmware, decryption keys, or saved games.

## 3. Extract and launch

From the directory containing the download:

```bash
tar -xzf Sakura-Wars-PS4RT-*-linux-x86_64.tar.gz
cd Sakura-Wars-PS4RT-*-linux-x86_64
bash Play-Game.sh --game "/path/to/CUSA16429"
```

Replace the quoted path with the location of **your own extracted game folder**. Keep the entire extracted PS4RT directory together and writable: prepared game files, settings, saves, shader caches and installed DLC live under that directory.

**Important: The v1.00 update changes the game folder you selected in place.** The installer does not make a backup of the original base game. If you want to preserve pristine v1.00, duplicate the extracted game directory before pointing the launcher at it. Your update PKG is not modified.

### First-run setup

- **Already v1.01:** The launcher proceeds directly to game preparation and startup. There is no update or DLC setup prompt.
- **v1.00:** The bundled graphical setup asks for your matching **CUSA16429 v1.01 update PKG**, validates it and installs the update into the selected extracted game folder. Cancelling or failing this required step prevents launch.
- **After a successful v1.00 update:** The launcher presents **optional DLC setup**. Install your own compatible DLC PKGs, or select **Close and launch game** to skip. The launcher then restarts cleanly and enters the game.
- **Subsequent starts:** No initial setup prompt repeats. DLC can be installed later via the in-game overlay.

The portable bundles its own graphical installer and native PKG extractor. No Tkinter, KDialog, Zenity, or external `ps4-pkg` installation is needed. See [Player update and DLC setup](PLAYER_PKG_SETUP.md) for troubleshooting and the exact DLC workflow.

## 4. In-game controls and graphics

Open the PS4RT menu using **Insert** on a keyboard or **L2 + R3 click** on a controller. Navigate with the D-pad, Cross to select, and Circle to go back (typical Xbox-style controllers use A/B equivalents).

The menu includes **1080p / 1440p / 4K** output, **Off / FSR 3.1 / FSR 4 v07** upscaling where supported, sharpening, anisotropic filtering, display/VSync controls, silhouette/outline options, and **30 / experimental 60 FPS**. Some graphics settings apply live; anisotropic filtering requires a game restart. 4K output does not mean the original scene is rasterized natively at 4K.

Optional command-line overrides are available:

```bash
bash Play-Game.sh --game "/path/to/CUSA16429" --upscaler fsr4 --output-resolution 4k
```

After installing DLC from **Game > Install DLC...** in the overlay, save your game and choose **Restart to load DLC...**. An already installed DLC is validated and **skipped without overwriting** its existing files. The newly installed content loads on restart, not in the running game.

## Requirements and limitations

- Native **x86-64/AMD64 Linux**, with **glibc 2.35+** and **Python 3**. ARM64 and CPU translators such as FEX or Box64 are not supported or validated.
- Working Vulkan 1.3-capable graphics driver/loader and compatible desktop display/input environment. FSR 4 depends on additional compatible device features and falls back to other supported options when unavailable.
- Fedora Linux with AMD/RADV graphics has been gameplay-tested. Other Linux distributions and GPUs, including Steam Deck / SteamOS, are not comprehensively validated.
- The portable bundles its audited application libraries, including SDL3, FFmpeg and Crypto++. **Do not install separate versions or set `LD_LIBRARY_PATH` merely to run the portable.**

This is an **experimental, title-specific** PS4 native compatibility runtime, not a general PS4 emulator. Testing has reached Chapters 1–3; the full game has **not** been verified end-to-end. For developer instructions, see [Linux distribution guide](DISTRIBUTING_LINUX.md), [Running on Linux](RUNNING_LINUX.md), and [`packaging/linux/README.md`](../packaging/linux/README.md).
