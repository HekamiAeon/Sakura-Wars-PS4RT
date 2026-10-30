# Sakura Wars: portable Linux build

This is the **same controlled Ubuntu 22.04 / GCC 14 / glibc 2.35 portability
strategy** manually validated on the other PS4RT ports. Sakura Wars
was started before the standardized Lab builder existed, so this is a manual
retrofit. The Sakura Wars portable has been gameplay-tested on Fedora, including fresh v1.00 update installation, optional DLC setup, and first boot.

## Build the controlled toolchain

If the already-proven PS4RT base image exists locally:

```bash
bash packaging/linux/build-pkg-image.sh
```

That adds the missing Crypto++ development files without rebuilding the entire
base image. The base image already provides zlib and the runtime's SDL3,
FFmpeg, Vulkan, Boost, fmt, Zydis, and other build dependencies.

On a machine without the existing base image, run:

```bash
bash packaging/linux/build-images.sh
bash packaging/linux/build-pkg-image.sh
```

The toolchain is **developer-only**. Players don't need Podman, compilers,
Crypto++ headers, or zlib development packages.

## Build and package from a reviewed commit

```bash
bash packaging/linux/build-release.sh
```

This uses committed, sanitized source to build into a sentinel-protected
workspace under `~/Documents/PS4RT-Builds/<repository-name>/` and generates
separate player and source tarballs, each with SHA256 checksums, under
`~/Documents/PS4RT-Releases/<repository-name>/`.

Unlike the original 12-library Lab bundle, Sakura Wars also includes the
native `ps4rt-pkg` extractor, graphical `ps4rt-setup` installer, and the
linked Crypto++ shared library. zlib is included with the original 12.
The scripts collect libraries from the **build container**, never from
the developer's Fedora host, set relative ELF RUNPATHs, and audit x86-64,
glibc version requirements, and library resolution.

The portable archive selects `bin/ps4rt`, `bin/ps4rt-setup`, and
`bin/ps4rt-pkg` automatically. Source builds still use `out/`.

## Boundaries

- The experimental branch is separate from the stable `main` release.
- The source and binary archives contain no PS4 game dumps, PKGs, saves,
  private developer files, or original game assets.
- The update installer modifies the **player's selected extracted CUSA folder**
  in place. Always test using a disposable copy of a pristine v1.00 extraction.
- The vendored native extractor has upstream GPL notices in
  `third_party/ps4pkg/`. Distribute corresponding source and third-party
  notices with releases.
- The older PS4RT portable audits do **not** substitute for a
  Sakura Wars clean-container build, package audit, update test, DLC test,
  and actual gameplay validation. Verify the final release archives and license obligations before publication.
