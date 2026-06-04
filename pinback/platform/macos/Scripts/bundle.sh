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

# Build the cockpit backend (pinback-server, with its UI embedded) from the
# repo project root and embed it next to the shell so the app is
# self-contained: at launch the shell spawns Contents/MacOS/pinback-server on
# 127.0.0.1:8088. Skipped if `make` isn't available here — the shell then falls
# back to a PATH install or a PINBACK_URL override.
REPO_ROOT="$(cd ../.. && pwd)"          # platform/macos -> project root
SERVER=""
if make -C "$REPO_ROOT" pinback-server >/dev/null 2>&1; then
    SERVER="$REPO_ROOT/pinback-server"
fi

rm -rf "$APP"
mkdir -p "$APP/Contents/MacOS" "$APP/Contents/Resources"
cp "$BIN" "$APP/Contents/MacOS/PinbackShell"
cp "Resources/AppIcon.icns" "$APP/Contents/Resources/AppIcon.icns"
strip -x "$APP/Contents/MacOS/PinbackShell"   # drop local symbols
if [ -n "$SERVER" ] && [ -x "$SERVER" ]; then
    cp "$SERVER" "$APP/Contents/MacOS/pinback-server"
    strip -x "$APP/Contents/MacOS/pinback-server"
    echo "Embedded pinback-server from $SERVER"
else
    echo "warning: pinback-server not built; app relies on PATH or PINBACK_URL"
fi

cat > "$APP/Contents/Info.plist" <<'PLIST'
<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0">
<dict>
    <key>CFBundleExecutable</key><string>PinbackShell</string>
    <key>CFBundleIdentifier</key><string>dev.pinback.shell</string>
    <key>CFBundleName</key><string>Pinback</string>
    <key>CFBundleIconFile</key><string>AppIcon</string>
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
