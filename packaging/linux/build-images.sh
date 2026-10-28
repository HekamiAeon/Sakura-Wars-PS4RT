#!/usr/bin/env bash
set -euo pipefail
cd -- "$(dirname -- "$0")/../.."
for entry in \
  'Containerfile jammy' \
  'Containerfile.deps complete' \
  'Containerfile.cxx23 cxx23' \
  'Containerfile.zycore zycore' \
  'Containerfile.ffmpeg-png linux'; do
  read -r file tag <<< "$entry"
  podman build --format docker -f "packaging/linux/$file" -t "localhost/ps4rt-builder:$tag" .
done
echo 'Final image: localhost/ps4rt-builder:linux'
