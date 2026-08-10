#!/bin/bash
set -e

# Assembles the real .app bundle structure LSSharedFileListInsertItemURL/
# NSBundle mainBundle need (see src/login_items.m, src/cocoa_ui.m) around
# the already cross-compiled binary - a bare Mach-O executable has no
# bundle identity at all, so Login Items registration, window/Dock
# integration, and the Finder icon would not behave correctly without
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

cat > "$APP_DIR/Contents/Info.plist" <<'EOF'
<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0">
<dict>
    <key>CFBundleExecutable</key>
    <string>PowerPCPod</string>
    <key>CFBundleIdentifier</key>
    <string>com.ppcpod.gui</string>
    <key>CFBundleName</key>
    <string>PowerPC Pod</string>
    <key>CFBundleDisplayName</key>
    <string>PowerPC Pod</string>
    <key>CFBundleIconFile</key>
    <string>AppIcon</string>
    <key>CFBundlePackageType</key>
    <string>APPL</string>
    <key>CFBundleShortVersionString</key>
    <string>1.0</string>
    <key>CFBundleVersion</key>
    <string>1</string>
    <key>LSMinimumSystemVersion</key>
    <string>10.5</string>
    <key>NSHighResolutionCapable</key>
    <false/>
    <key>NSHumanReadableCopyright</key>
    <string>Kaan Kölköy</string>
</dict>
</plist>
EOF

echo "Built $APP_DIR"
