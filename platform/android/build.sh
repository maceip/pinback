#!/usr/bin/env bash
# Build the Android shell APK (Unix hosts / CI). Requires JDK 17+ and ANDROID_HOME.
set -euo pipefail
cd "$(dirname "$0")"

VARIANT="${1:-debug}"
case "$VARIANT" in
  debug)
    ./gradlew assembleDebug --no-daemon
    echo "Built: app/build/outputs/apk/debug/app-debug.apk"
    ;;
  release)
    ./gradlew assembleRelease --no-daemon
    echo "Built: app/build/outputs/apk/release/app-release-unsigned.apk"
    ;;
  both)
    ./gradlew assembleDebug assembleRelease --no-daemon
    echo "Built debug + release APKs under app/build/outputs/apk/"
    ;;
  *)
    echo "usage: $0 [debug|release|both]" >&2
    exit 2
    ;;
esac
