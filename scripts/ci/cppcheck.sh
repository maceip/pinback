#!/usr/bin/env bash
# Static security-oriented analysis with cppcheck.
set -euo pipefail

root="$(cd "$(dirname "$0")/../.." && pwd)"
cd "$root"

if ! command -v cppcheck >/dev/null 2>&1; then
  echo "cppcheck: not installed, skipping" >&2
  exit 0
fi

make embed >/dev/null

includes=(-I src)
if [[ "$(uname -s)" == "Darwin" ]]; then
  includes+=(-D_DARWIN_C_SOURCE)
else
  includes+=(-D_DEFAULT_SOURCE)
fi

sources="$(find src -name '*.c' | sort | tr '\n' ' ')"
echo "cppcheck: analyzing src/*.c"
# shellcheck disable=SC2086
cppcheck \
  --enable=warning,performance,portability \
  --suppress=missingIncludeSystem \
  --error-exitcode=1 \
  --inline-suppr \
  "${includes[@]}" \
  $sources

echo "cppcheck: ok"
