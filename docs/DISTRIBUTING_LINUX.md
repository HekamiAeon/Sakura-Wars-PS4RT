# Sakura Wars (CUSA16429): Linux distribution guide

This guide covers the **Sakura Wars PS4RT** Linux source and portable runtime release for **CUSA16429 v1.01**. PS4RT is an experimental, game-specific native x86-64 compatibility runtime adapted from the earlier PS4RT porting lineage and shadPS4's rendering foundation. Keep the upstream credits, copyright, GPL and third-party notices intact.

**The release contains no PS4 game data, firmware, update/DLC PKGs, decryption keys, or saved games.** Players provide their own extracted, plaintext CUSA16429 directory and, when necessary, a matching v1.01 update PKG.

## For players: download, extract, run

**Do not compile from source just to play v1.0.** Download the prebuilt
[Linux x86-64 runtime archive](https://github.com/HekamiAeon/Sakura-Wars-PS4RT/releases/download/v1.0/Sakura-Wars-PS4RT-85ac44f-linux-x86_64.tar.gz) from the
[official v1.0 release](https://github.com/HekamiAeon/Sakura-Wars-PS4RT/releases/tag/v1.0)
(or visit [the latest release](https://github.com/HekamiAeon/Sakura-Wars-PS4RT/releases/latest)).
Choose **`Sakura-Wars-PS4RT-85ac44f-linux-x86_64.tar.gz`**, not the source archive.
Download its [SHA256 checksum](https://github.com/HekamiAeon/Sakura-Wars-PS4RT/releases/download/v1.0/Sakura-Wars-PS4RT-85ac44f-linux-x86_64.tar.gz.sha256) alongside it.

Developers can download the separate
[matching source archive](https://github.com/HekamiAeon/Sakura-Wars-PS4RT/releases/download/v1.0/Sakura-Wars-PS4RT-85ac44f-source.tar.gz) and
[source checksum](https://github.com/HekamiAeon/Sakura-Wars-PS4RT/releases/download/v1.0/Sakura-Wars-PS4RT-85ac44f-source.tar.gz.sha256).

From the folder containing your downloads, verify, extract, and launch:

```bash
sha256sum -c Sakura-Wars-PS4RT-85ac44f-linux-x86_64.tar.gz.sha256
tar -xzf Sakura-Wars-PS4RT-85ac44f-linux-x86_64.tar.gz
cd Sakura-Wars-PS4RT-85ac44f-linux-x86_64
bash Play-Game.sh --game "/path/to/CUSA16429"
```

The checksum command needs the runtime archive and its matching `.sha256`
in the same directory. The `--game` value must be the player's **own extracted plaintext** directory with `eboot.bin`, `sce_sys/param.sfo`, `sce_module/` and game data, not a PKG or parent folder. Preserve quotes around paths containing spaces.

The game must be **v1.01** to launch. For a supplied **v1.00**, the integrated installer requests the player's update PKG and updates that extracted game directory **in place**. Back up pristine v1.00 first if it must be retained. A successful fresh upgrade offers optional DLC selection, with a skip/close path; an already updated v1.01 starts without prompts. Additional DLC may be installed using the PS4RT in-game overlay, followed by a game restart.

For a full walkthrough, see [Linux binary release](LINUX_BINARY_RELEASE.md) and [Player update and DLC setup](PLAYER_PKG_SETUP.md). No separate compiler, manual SDL3/FFmpeg installation, GUI toolkit, or external PKG extractor is required for the prebuilt runtime.

### Platform and gameplay status

The prebuilt runtime targets **native Linux x86-64 / AMD64**, **glibc 2.35+**, Python 3 and a working Vulkan 1.3-capable graphics/display stack. It bundles audited application libraries, including SDL3, FFmpeg, Crypto++ and the native PKG extractor, with relative ELF `$ORIGIN` library paths. ARM64, FEX and Box64 translation are unsupported/unverified.

**Fedora with AMD/RADV graphics** has been gameplay-tested through **Chapters 1–3**, including first-boot save creation, saving/loading, controller use, cinematics and combat. Chapters 4 onward, all Linux distributions and additional GPUs have not been exhaustively tested; Steam Deck/SteamOS compatibility is not asserted here. This is a title-specific experimental runtime, not a general PS4 emulator.

In game, open the PS4RT settings overlay with **Insert** or **L2 + R3 click**. Options include 1080p / 1440p / 4K output, Off / FSR 3.1 / FSR 4 v07 upscaling, display settings, sharpening, AF, outlines and original 30 / experimental 60 FPS modes. AF changes require a restart. FSR 4 v07 requires supported Vulkan device features; fallback options remain available. These output modes reconstruct the scene and should not be described as native 4K rasterization.

### Private user data

| Local path within extracted portable | Contents |
| --- | --- |
| `out/CUSA16429/boot-linked.bin`, `content.bin` and related files | Prepared or linked game-derived content |
| `out/CUSA16429/ps4rt.ini` | Local graphics settings |
| `out/CUSA16429/user/savedata/` | Game save data |
| `out/CUSA16429/user/gpu/cache/` | Shader and pipeline caches |
| `out/CUSA16429/player-content/dlc/` | Optional DLC installed from player-supplied PKGs |
| `dlc/CUSA16429/` (if present) | Optional pre-extracted add-ons |

**Never redistribute** these directories, prepared executables, player saves, shader caches, proprietary game data or other private artifacts. The source and runtime packaging allowlists are intended to omit such data; review the generated archives before publishing.

## For developers: build from source

A conventional development build uses a C11/C++23 toolchain, Python 3, CMake, Ninja, pkg-config, Vulkan/SDL3, FFmpeg, fmt, Boost, xxHash, robin-map, Zydis, glslang and X11 development packages. From the source root:

```bash
bash build.sh --test
bash run.sh --game "/path/to/CUSA16429"
```

A host-native development build is **not** the standard redistributable bundle: it may link to libraries installed on that host. For a portable release use the controlled **Ubuntu 22.04 / glibc 2.35** toolchain and the dedicated release script documented in [`packaging/linux/README.md`](../packaging/linux/README.md).

### Maintainer release workflow

1. Start from a **reviewed, clean Git commit** containing all final source code, guides, licenses, attribution and checksums for bundled assets. The release builder reads **committed Git HEAD**, not uncommitted working files.
2. Ensure the controlled build images exist. Build them as needed with `bash packaging/linux/build-images.sh` and `bash packaging/linux/build-pkg-image.sh`; builders use Podman and are not required by players.
3. Run `bash packaging/linux/build-release.sh` from the committed source root. It stages the private build under `~/Documents/PS4RT-Builds/<repository-name>/` and produces **four files** under `~/Documents/PS4RT-Releases/<repository-name>/`: the versioned Linux runtime `.tar.gz`, its `.sha256`, the matching source `.tar.gz`, and its `.sha256`.
4. Validate both checksums and inspect the archive contents for proprietary/private material. Verify the release's embedded source revision, x86-64 ELF architecture, `$ORIGIN` RUNPATHs, glibc baseline, linked dependencies and corresponding-source availability. Verify the archive and exact launcher on a clean host where feasible.
5. Smoke-test a **freshly extracted portable** against a disposable copy of pristine v1.00: update picker, successful v1.01 installation, optional DLC selection and skip, first-boot save creation, game startup, save/load and prompt-free restart. Separately test already-v1.01 startup and duplicate/overlay DLC handling. Do not use development saves as a shortcut.
6. Publish the four assets together under a versioned GitHub Release on [Sakura Wars PS4RT](https://github.com/HekamiAeon/Sakura-Wars-PS4RT/releases). For **v1.0**, the audited release is [tagged here](https://github.com/HekamiAeon/Sakura-Wars-PS4RT/releases/tag/v1.0) and contains the [Linux runtime](https://github.com/HekamiAeon/Sakura-Wars-PS4RT/releases/download/v1.0/Sakura-Wars-PS4RT-85ac44f-linux-x86_64.tar.gz), its [checksum](https://github.com/HekamiAeon/Sakura-Wars-PS4RT/releases/download/v1.0/Sakura-Wars-PS4RT-85ac44f-linux-x86_64.tar.gz.sha256), the [source archive](https://github.com/HekamiAeon/Sakura-Wars-PS4RT/releases/download/v1.0/Sakura-Wars-PS4RT-85ac44f-source.tar.gz), and its [checksum](https://github.com/HekamiAeon/Sakura-Wars-PS4RT/releases/download/v1.0/Sakura-Wars-PS4RT-85ac44f-source.tar.gz.sha256). Confirm downloads and `sha256sum -c` work. Future releases will use the stable [latest-release link](https://github.com/HekamiAeon/Sakura-Wars-PS4RT/releases/latest) with their own revision-specific filenames.

The packager uses a restricted source export and a checked runtime file manifest. It bundles the Linux `ps4rt`, `ps4rt-setup` graphical UI and native `ps4rt-pkg` extractor, plus audited runtime libraries and licensed graphics resources. No PS4 binaries or developer-local artifacts belong in public archives.

### Licenses, notices and provenance

PS4RT is **adapted open-source software**, not a clean-room reimplementation. Preserve the original copyright, GPL and vendor notices recorded in [Upstream credits and notices](UPSTREAM_CREDITS.md) and the repository's third-party license files. PS4RT runtime source is **GPL-2.0-or-later**; individual components and bundled data retain their own license terms.

The FSR 4 **v07** shaders/model resources, their notice and `packaging/fsr4-v07-assets.json` checksum manifest must accompany any build advertising FSR 4. The native PKG component derives from shadPS4Plus code and requires its corresponding GPL source and attribution. The release includes third-party libraries and assets such as FFmpeg, FidelityFX/FSR-Vulkan, ImGui, LibAtrac9, Sirit, half, xbyak, miniz, magic_enum and DejaVu.

**Review the actual FFmpeg build flags and license terms** before binary redistribution. See [FFmpeg legal guidance](https://ffmpeg.org/legal.html) and the [GNU GPL license information](https://www.gnu.org/licenses/). Do not assume host system libraries are safe to copy wholesale into public releases. Check the license/source notices for bundled fonts, interface imagery, menu audio and screenshots separately.

The project is not affiliated with Sega, Sony or AMD, and does not authorize redistribution of the underlying PS4 game or associated media. Publication requires a final source/license audit and verification of the exact release assets.
