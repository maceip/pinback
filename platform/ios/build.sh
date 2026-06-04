#!/usr/bin/env bash
# Build the iOS shell for the iOS Simulator.
set -euo pipefail
cd "$(dirname "$0")"

CONFIG="${CONFIG:-Debug}"
DEST="${DEST:-platform=iOS Simulator,name=iPhone 17}"

xcodebuild \
    -scheme Pinback \
    -destination "$DEST" \
    -configuration "$CONFIG" \
    build
