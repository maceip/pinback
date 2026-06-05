#!/usr/bin/env bash
# Lint pinback core C sources: clang-format + compile-all-warnings.
set -euo pipefail

root="$(cd "$(dirname "$0")/../.." && pwd)"
cd "$root"

if ! command -v clang-format >/dev/null 2>&1; then
  echo "lint: clang-format not found" >&2
  exit 1
fi

echo "lint: clang-format"
diff=0
nfiles=0
while IFS= read -r f; do
  nfiles=$((nfiles + 1))
  tmp="$(mktemp)"
  clang-format "$f" >"$tmp"
  if ! cmp -s "$f" "$tmp"; then
    echo "lint: format drift in $f (run: clang-format -i $f)" >&2
    diff=1
  fi
  rm -f "$tmp"
done < <(find src tests/support \( -name '*.c' -o -name '*.h' \) | sort)
echo "lint: checked ${nfiles} files"
if [[ "$diff" -ne 0 ]]; then
  exit 1
fi

echo "lint: compile check (-O0 debug, full warnings)"
make clean
make OPT_FLAGS=-O0 DEBUG_FLAGS=-g3 embed all

echo "lint: ok"
