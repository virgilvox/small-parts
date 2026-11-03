#!/usr/bin/env bash
# Adds the shader states a test run printed as "[cart] standard+..." to the
# program table and rebuilds, one caller at a time (several people can share
# the tree: the table in src/gen is common to every build).
#   tools/regen.sh <file with one state per line> [OUT dir]
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
LOCK="$ROOT/build/.regen.lock"
mkdir -p "$ROOT/build"
until mkdir "$LOCK" 2>/dev/null; do sleep 1; done
trap 'rmdir "$LOCK"' EXIT
grep -E '^(standard|basic|lambert|phong|physical|toon|points|line|sprite|shader|depth|distance|shadow)' "$1" | sort -u > "$ROOT/build/missing.txt" || true
(cd "$ROOT" && REGEN_STATES=1 OUT="$ROOT/build" ./build.sh)
[ -n "${2:-}" ] && (cd "$ROOT" && OUT="$2" ./build.sh) || true
