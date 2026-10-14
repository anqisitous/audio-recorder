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

cat > "$APP/Contents/Info.plist" <<'PLIST'
<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0">
<dict>
  <key>CFBundleIdentifier</key><string>local.rec.app</string>
  <key>CFBundleName</key><string>Rec</string>
  <key>CFBundleExecutable</key><string>rec</string>
  <key>CFBundlePackageType</key><string>APPL</string>
  <key>NSPrincipalClass</key><string>NSApplication</string>
  <key>LSMinimumSystemVersion</key><string>14.4</string>
  <key>NSAudioCaptureUsageDescription</key><string>Record per-app audio.</string>
</dict>
</plist>
PLIST

codesign --force --sign - "$APP"

echo "Built $APP"
echo "Run:   open Rec.app      -> http://127.0.0.1:8765/ ã§ã¢ããªãé¸ãã§é²é³"
echo "CLI:   $APP/Contents/MacOS/rec --list | rec Spotify"
echo "Quit:  ãã¼ã¸ã®ããã¹ã¦åæ­¢ãã¦çµäºã/ Dock ã®ãçµäºã/ Ctrl+C ã§ä¿å­"