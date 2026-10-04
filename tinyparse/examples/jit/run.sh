#!/usr/bin/env bash
# examples/jit/run.sh '<C source>' <function> <int-arg>
#
# JITs the named function from the C source, calls it with the integer
# argument, and prints the integer result. The driver is built by `mise run
# examples` into build/examples/jit/ ($TP_EXAMPLES/jit when the test suite
# runs it).
#
# Designed to run with cwd = tinyparse/ (as the test harness does) but
# also works from anywhere: paths default from this script's own location.
set -uo pipefail

HERE="$(cd "$(dirname "$0")" && pwd)"
BIN="${TP_EXAMPLES:-$HERE/../../build/examples}/jit/jit"

if [ "$#" -lt 3 ]; then
  echo "usage: run.sh '<C source>' <function> <int-arg>" >&2
  exit 2
fi
if [ ! -x "$BIN" ]; then
  echo "run.sh: $BIN is not built; run: mise run examples" >&2
  exit 2
fi

JIT_GRAMMAR="$HERE/jit.scm" exec "$BIN" "$1" "$2" "$3"
