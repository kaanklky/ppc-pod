#!/bin/bash
set -e

# Assembles the real .app bundle structure NSBundle mainBundle needs
# (see src/cocoa_ui.m) around the already cross-compiled binary - a bare
# Mach-O executable has no bundle identity at all, so window/Dock
# integration and the Finder icon would not behave correctly without
# this.
#
# Usage: docker/make_app_bundle.sh <path-to-compiled-ppc-binary> [output-dir]

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_DIR="$(dirname "$SCRIPT_DIR")"

BIN_PATH="$1"
OUT_DIR="${2:-$PROJECT_DIR}"
APP_NAME="PowerPC Pod"
APP_DIR="$OUT_DIR/$APP_NAME.app"

if [ -z "$BIN_PATH" ] || [ ! -f "$BIN_PATH" ]; then
  echo "Usage: $0 <path-to-compiled-ppc-binary> [output-dir]" >&2
  exit 1
fi

rm -rf "$APP_DIR"
mkdir -p "$APP_DIR/Contents/MacOS"
mkdir -p "$APP_DIR/Contents/Resources"

cp "$BIN_PATH" "$APP_DIR/Contents/MacOS/PowerPCPod"
chmod +x "$APP_DIR/Contents/MacOS/PowerPCPod"

if [ -f "$PROJECT_DIR/resources/AppIcon.icns" ]; then
  cp "$PROJECT_DIR/resources/AppIcon.icns" "$APP_DIR/Contents/Resources/AppIcon.icns"
else
  echo "Warning: $PROJECT_DIR/resources/AppIcon.icns not found - app will use the generic Finder icon" >&2
fi

if [ ! -f "$PROJECT_DIR/resources/Info.plist" ]; then
  echo "Missing $PROJECT_DIR/resources/Info.plist" >&2
  exit 1
fi
cp "$PROJECT_DIR/resources/Info.plist" "$APP_DIR/Contents/Info.plist"

echo "Built $APP_DIR"
