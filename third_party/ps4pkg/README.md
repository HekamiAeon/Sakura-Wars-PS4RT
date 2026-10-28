# PS4RT native PKG reader (vendored source)

This directory contains only the PS4 PKG/PFS structures, file parser, and
cryptography routines from **AzaharPlus/shadPS4Plus**, tag
`PKG_EXTRACTOR_1_1`, based on shadPS4's PKG implementation.

Upstream: https://github.com/AzaharPlus/shadPS4Plus/tree/PKG_EXTRACTOR_1_1

Source initially imported from upstream: `common/types.h`, `common/endian.h`,
`core/crypto/{keys.h,crypto.h,crypto.cpp}`,
`core/file_format/{pfs.h,pkg.h,pkg.cpp,pkg_type.h,pkg_type.cpp}`.
The PKG parser has since received PS4RT-specific bounds checks, short-read
handling, sector-aligned file reads, and opt-in diagnostics. Original GPL
notices remain intact.
See `LICENSE` here. PS4RT's minimal `common/io_file.h` is an original
stdio adapter to avoid the upstream GUI, logging, and emulator frameworks.

PS4RT's own entry point is `scripts/ps4rt_pkg_native.cpp`.
It performs early PKG metadata checks, uses the vendored extractor,
normalizes DLC output, and returns nonzero on extraction failure.

`scripts/build_pkg_native.sh` links Crypto++ (Boost Software License 1.0)
and zlib (zlib license). Those are **maintainer build dependencies**.
Developer builds produce `out/ps4rt-pkg` and the matching shared library in
`out/lib/`. The audited portable release carries `bin/ps4rt-pkg` and the
exact Crypto++ SONAME in `lib/`, so users install nothing.
Preserve the Crypto++ and zlib license notices and satisfy applicable
GPL corresponding-source requirements when publishing binary releases.

The native component compiled successfully in CI and was exercised against
a real Sakura Wars v1.01 update package. An initial read failure was repaired,
but a follow-up game launch showed a black screen. Extraction, installation,
and DLC/gameplay integration are **not yet fully validated**. Use only a
disposable pristine v1.00 copy until those tests have passed.
