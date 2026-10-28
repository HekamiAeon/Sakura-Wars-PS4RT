#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0-or-later
# Build, audit, and package four private-free release assets from Git HEAD.
set -euo pipefail

fail() { printf 'ERROR: %s\n' "$*" >&2; exit 1; }
ROOT="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/../.." && pwd -P)"
PROJECT="${PS4RT_BUILD_ID:-$(basename "$ROOT")}"
[[ "$PROJECT" =~ ^[A-Za-z0-9][A-Za-z0-9._-]{0,79}$ ]] || fail 'Invalid project name'
# Reject dirty tracked source BEFORE spending time compiling old HEAD.
git -C "$ROOT" diff --quiet HEAD || fail 'Commit tracked source changes before building a release'
PARENT="${PS4RT_LINUX_WORK_PARENT:-$HOME/Documents/PS4RT-Builds}"
WORK="$PARENT/$PROJECT"
RELEASE_ROOT="${PS4RT_RELEASE_ROOT:-$HOME/Documents/PS4RT-Releases}"
echo "Project: $PROJECT"
echo "Source: committed Git HEAD only"
echo "Private build workspace: $WORK"
echo "Release assets: $RELEASE_ROOT/$PROJECT"
bash "$ROOT/packaging/linux/build-runtime.sh"
[[ -f "$WORK/source-revision.txt" && -d "$WORK/bundle" ]] ||
  fail 'Build did not create expected workspace'
[[ "$(cat "$WORK/source-revision.txt")" == "$(git -C "$ROOT" rev-parse HEAD)" ]] ||
  fail 'Source changed after build; refusing mixed-revision package'
python3 "$ROOT/packaging/linux/release.py" package \
  --bundle-from "$WORK/bundle" \
  --name "$PROJECT" \
  --output "$RELEASE_ROOT/$PROJECT"
echo 'Four archives/checksums created, not published. Inspect and play-test before releasing.'
