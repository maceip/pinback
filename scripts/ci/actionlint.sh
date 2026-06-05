#!/usr/bin/env bash
# Validate GitHub Actions workflow YAML.
set -euo pipefail

root="$(cd "$(dirname "$0")/../.." && pwd)"
cd "$root"

if ! command -v actionlint >/dev/null 2>&1; then
  if command -v go >/dev/null 2>&1; then
    GOBIN="${GOBIN:-$(go env GOPATH 2>/dev/null)/bin}" \
      go install github.com/rhysd/actionlint/cmd/actionlint@latest
    export PATH="$GOBIN:$PATH"
  fi
fi

if ! command -v actionlint >/dev/null 2>&1; then
  echo "actionlint: not installed, skipping" >&2
  exit 0
fi

actionlint -color
echo "actionlint: ok"
