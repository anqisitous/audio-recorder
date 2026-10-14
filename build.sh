#!/bin/bash
# Build Rec.app (needs only Command Line Tools: xcode-select --install)
set -euo pipefail
cd "$(dirname "$0")"

APP=Rec.app
rm -rf "$APP"
mkdir -p "$APP/Contents/MacOS" "$APP/Contents/Resources"

clang++ -std=c++17 -O2 rec.cpp -o "$APP/Contents/MacOS/rec" \
  -framework CoreAudio -framework CoreFoundation -framework Foundation \
  -framework AudioToolbox -framework AppKit

cp index.html "$APP/Contents/Resources/index.html"

# --- SVG からアイコンを生成 ---
if [ -f icon.svg ]; then
  echo "Generating icon..."
  TMP=$(mktemp -d)
  trap 'rm -rf "$TMP"' EXIT

  # SVG -> 1024x1024 PNG (qlmanage は -s で最大辺を指定)
  qlmanage -t -s 1024 -o "$TMP" icon.svg >/dev/null 2>&1
  SRC="$TMP/icon.svg.png"
  if [ ! -f "$SRC" ]; then
    echo "warning: qlmanage failed to render icon.svg" >&2
  else
    mkdir -p "$TMP/icon.iconset"
    for s in 16 32 64 128 256 512; do
      sips -z "$s"        "$s"        "$SRC" --out "$TMP/icon.iconset/icon_${s}x${s}.png"    >/dev/null
      sips -z "$((s*2))"  "$((s*2))"  "$SRC" --out "$TMP/icon.iconset/icon_${s}x${s}@2x.png" >/dev/null
    done
    iconutil -c icns "$TMP/icon.iconset" -o "$APP/Contents/Resources/Rec.icns"
  fi
fi

cat > "$APP/Contents/Info.plist" <<'PLIST'
<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0">
<dict>
  <key>CFBundleIdentifier</key><string>local.rec.app</string>
  <key>CFBundleName</key><string>Rec</string>
  <key>CFBundleExecutable</key><string>rec</string>
  <key>CFBundleIconFile</key><string>Rec</string>
  <key>CFBundlePackageType</key><string>APPL</string>
  <key>NSPrincipalClass</key><string>NSApplication</string>
  <key>LSMinimumSystemVersion</key><string>14.4</string>
  <key>NSAudioCaptureUsageDescription</key><string>Record per-app audio.</string>
</dict>
</plist>
PLIST

codesign --force --sign - "$APP"

echo "Built $APP"
echo "Run:   open Rec.app      -> http://127.0.0.1:8765/"
echo "CLI:   $APP/Contents/MacOS/rec --list | rec Spotify"