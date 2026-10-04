#!/usr/bin/env bash
# run.sh <file.c> — rewrite a C file's integer #defines into one enum.
#
# Runs the rewriter on the given file and prints the rewritten source to
# stdout. The rewriter, with its grammar embedded, is built by `mise run
# examples` into build/examples/rewriter/ ($TP_EXAMPLES/rewriter when the test
# suite runs it). Works regardless of the caller's cwd (the test harness
# invokes it from tinyparse/): every path is resolved from this script's own
# location.
set -uo pipefail

HERE="$(cd "$(dirname "$0")" && pwd)"
BIN="${TP_EXAMPLES:-$HERE/../../build/examples}/rewriter/rewriter"

if [ "$#" -lt 1 ]; then
  echo "usage: run.sh <file.c>" >&2
  exit 2
fi
if [ ! -x "$BIN" ]; then
  echo "run.sh: $BIN is not built; run: mise run examples" >&2
  exit 1
fi

exec "$BIN" "$1"
