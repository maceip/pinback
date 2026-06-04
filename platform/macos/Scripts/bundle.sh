#!/usr/bin/env bash
# Wrap the SwiftPM executable in a minimal Pinback.app so it gets a real
# Info.plist — needed for App Transport Security (the http dev server) and a
# proper Dock/Finder presence. `swift run` is fine for quick iteration, but a
# bare binary has no Info.plist and ATS will block plain-http loads.
set -euo pipefail
cd "$(dirname "$0")/.."

swift build -c release
BIN=".build/release/PinbackShell"
APP="Pinback.app"

rm -rf "$APP"
mkdir -p "$APP/Contents/MacOS"
cp "$BIN" "$APP/Contents/MacOS/PinbackShell"
strip -x "$APP/Contents/MacOS/PinbackShell"   # drop local symbols

cat > "$APP/Contents/Info.plist" <<'PLIST'
<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0">
<dict>
    <key>CFBundleExecutable</key><string>PinbackShell</string>
    <key>CFBundleIdentifier</key><string>dev.pinback.shell</string>
    <key>CFBundleName</key><string>Pinback</string>
    <key>CFBundlePackageType</key><string>APPL</string>
    <key>CFBundleShortVersionString</key><string>0.1.0</string>
    <key>CFBundleVersion</key><string>1</string>
    <key>LSMinimumSystemVersion</key><string>26.0</string>
    <key>NSHighResolutionCapable</key><true/>
    <key>NSAppTransportSecurity</key>
    <dict><key>NSAllowsLocalNetworking</key><true/></dict>
</dict>
</plist>
PLIST

echo "Built $APP"
echo "Run:  open $APP            (or: PINBACK_URL=... open $APP)"
