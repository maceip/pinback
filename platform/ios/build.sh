#!/usr/bin/env bash
# Build the iOS shell for the iOS Simulator.
set -euo pipefail
cd "$(dirname "$0")"

# Single source: platform/common/ (see platform/CONTRACT.md).
mkdir -p Pinback/HostAssets
cp ../common/setup.html ../common/pinback-host.js Pinback/HostAssets/

CONFIG="${CONFIG:-Debug}"
DEST="${DEST:-platform=iOS Simulator,name=iPhone 17}"

xcodebuild \
    -scheme Pinback \
    -destination "$DEST" \
    -configuration "$CONFIG" \
    build
