#!/usr/bin/env bash
# run.sh <file.c> — rewrite a C file's integer #defines into one enum.
#
# Builds the rewriter on first use (or when stale), then runs it on the
# given file and prints the rewritten source to stdout. Works regardless of
# the caller's cwd (the test harness invokes it from tinyparse/):
# every path is resolved from this script's own location.
set -uo pipefail

HERE="$(cd "$(dirname "$0")" && pwd)"
BIN="$HERE/rewriter"

if [ "$#" -lt 1 ]; then
  echo "usage: run.sh <file.c>" >&2
  exit 2
fi

# Build if the binary is missing or older than its sources.
needs_build=0
[ -x "$BIN" ] || needs_build=1
for src in "$HERE/rewriter.c" "$HERE/cdefines.scm" "$HERE/build.sh"; do
  [ -f "$src" ] && [ "$src" -nt "$BIN" ] && needs_build=1
done
if [ "$needs_build" -ne 0 ]; then
  bash "$HERE/build.sh" >&2 || exit 1
fi

exec "$BIN" "$1"
