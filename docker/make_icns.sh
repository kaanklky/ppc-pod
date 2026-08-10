#!/bin/bash
set -e

# Builds resources/AppIcon.icns from the PNGs in resources/icon_sources/
# using png2icns (Debian's icnsutils package, from the libicns project) -
# a real, actively-maintained open-source implementation of the icns
# format. Runs inside a throwaway Debian container so no host package
# install is needed.
#
# Usage: docker/make_icns.sh

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_DIR="$(dirname "$SCRIPT_DIR")"
ICON_SRC_DIR="$PROJECT_DIR/resources/icon_sources"
OUT_FILE="$PROJECT_DIR/resources/AppIcon.icns"

for sz in 16 32 48 128 256 512; do
  if [ ! -f "$ICON_SRC_DIR/icon_${sz}.png" ]; then
    echo "Missing $ICON_SRC_DIR/icon_${sz}.png" >&2
    exit 1
  fi
done

docker run --rm -v "$ICON_SRC_DIR":/icons debian:bookworm bash -c '
  apt-get update -qq && apt-get install -y -qq icnsutils >/dev/null 2>&1
  cd /icons
  png2icns AppIcon.icns icon_16.png icon_32.png icon_48.png icon_128.png icon_256.png icon_512.png
  chown 1000:1000 AppIcon.icns
'

mv "$ICON_SRC_DIR/AppIcon.icns" "$OUT_FILE"
echo "Built $OUT_FILE"
