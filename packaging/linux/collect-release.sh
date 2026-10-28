#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0-or-later
# Copy a game packager's ALREADY CREATED archives to one per-game release folder.
# Does not create archives, publish anything, or touch the source repository.
set -euo pipefail
die() { printf 'ERROR: %s\n' "$*" >&2; exit 1; }

ROOT="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/../.." && pwd -P)"
PROJECT="${PS4RT_BUILD_ID:-$(basename "$ROOT")}"
[[ "$PROJECT" =~ ^[a-zA-Z0-9][a-zA-Z0-9._-]{0,79}$ ]] || die 'Invalid project folder name'
FROM="${1:-$ROOT/out/community}"
[[ -d "$FROM" && ! -L "$FROM" ]] || die "Missing source release directory: $FROM"
FROM="$(cd -- "$FROM" && pwd -P)"
PARENT="${PS4RT_RELEASE_ROOT:-$HOME/Documents/PS4RT-Releases}"
mkdir -p -- "$PARENT"
TARGET="$PARENT/$PROJECT"
[[ ! -L "$TARGET" ]] || die "Refusing symlink destination: $TARGET"
mkdir -p -- "$TARGET"
[[ -d "$TARGET" ]] || die "Release path is not a directory: $TARGET"

shopt -s nullglob
archives=("$FROM"/*.tar.gz)
checksums=("$FROM"/*.tar.gz.sha256)
(( ${#archives[@]} > 0 && ${#checksums[@]} > 0 )) ||
    die "Expected .tar.gz and .tar.gz.sha256 assets in $FROM"
(( ${#archives[@]} == ${#checksums[@]} )) ||
    die "Each archive must have its own .sha256 file"
for archive in "${archives[@]}"; do
  [[ ! -L "$archive" && -f "$archive" ]] || die "Unsafe archive: $archive"
  checksum="$archive.sha256"
  [[ ! -L "$checksum" && -f "$checksum" ]] || die "Missing checksum: $checksum"
done
(
  cd -- "$FROM"
  for checksum in "${checksums[@]}"; do
    sha256sum --check --status -- "$(basename "$checksum")" ||
      die "Checksum verification failed: $(basename "$checksum")"
  done
)
for input in "${archives[@]}" "${checksums[@]}"; do
  dest="$TARGET/$(basename "$input")"
  if [[ -e "$dest" ]]; then
    cmp -s -- "$input" "$dest" ||
      die "Different file with same name already exists: $dest"
    printf 'Already collected: %s\n' "$dest"
  else
    cp -- "$input" "$dest"
    printf 'Collected: %s\n' "$dest"
  fi
done
echo "RELEASE_DIR=$TARGET"
echo 'Archives copied, not published. Play-test the extracted player archive before uploading.'
