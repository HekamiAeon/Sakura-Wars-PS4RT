#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0-or-later
# Fast, incremental Crypto++ layer: reuse the existing verified PS4RT Linux image.
set -euo pipefail
cd -- "$(dirname -- "$0")/../.."
command -v podman >/dev/null || { echo "Install Podman on the developer host." >&2; exit 1; }
if ! podman image exists localhost/ps4rt-builder:linux; then
    echo "Base PS4RT Linux builder is missing. Run: bash packaging/linux/build-images.sh" >&2
    exit 1
fi
podman build --format docker -f packaging/linux/Containerfile.pkg \
    -t localhost/ps4rt-builder:linux-pkg .
echo "Sakura Wars Crypto++ builder ready: localhost/ps4rt-builder:linux-pkg"
