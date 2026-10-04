#!/usr/bin/env bash
# examples/uri/run.sh <url>
#
# Parse a URL with the generated tinyparse grammar and print its components.
# Regenerates uri.scm from the EBNF if it is missing. The driver is built by
# `mise run examples` into build/examples/uri/ ($TP_EXAMPLES/uri when the test
# suite runs it). Works from any cwd: paths resolve from the script's own
# location.
set -uo pipefail

HERE="$(cd "$(dirname "$0")" && pwd)"
EBNF_DIR="$(cd "$HERE/../ebnf" && pwd)"
BIN="${TP_EXAMPLES:-$HERE/../../build/examples}/uri/uri"

if [ "$#" -lt 1 ]; then
  echo "usage: run.sh <url>" >&2
  exit 2
fi

# Ensure the generated grammar exists; regenerate from the EBNF if not.
if [ ! -f "$HERE/uri.scm" ]; then
  bash "$EBNF_DIR/run.sh" "$EBNF_DIR/uri.ebnf" >/dev/null || exit 1
fi

if [ ! -x "$BIN" ]; then
  echo "run.sh: $BIN is not built; run: mise run examples" >&2
  exit 1
fi

URI_GRAMMAR="$HERE/uri.scm" exec "$BIN" "$1"
