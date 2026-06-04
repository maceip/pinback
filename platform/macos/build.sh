#!/usr/bin/env bash
# Build the macOS shell and, by default, wrap it in Pinback.app with pinback-server.
set -euo pipefail
cd "$(dirname "$0")"

swift build -c release
test -x .build/release/PinbackShell

if [[ "${1:-bundle}" == "bundle" ]]; then
    ./Scripts/bundle.sh
else
    echo "Built .build/release/PinbackShell (pass 'bundle' arg or no args to also build Pinback.app)"
fi
