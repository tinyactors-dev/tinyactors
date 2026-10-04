#!/usr/bin/env bash
# examples/uri/run.sh <url>
#
# Parse a URL with the generated tinyparse grammar and print its components.
# Builds the driver if needed and regenerates uri.scm from the EBNF if it is
# missing, so the example is self-contained. Works from any cwd: paths resolve
# from the script's own location.
set -uo pipefail

HERE="$(cd "$(dirname "$0")" && pwd)"
EBNF_DIR="$(cd "$HERE/../ebnf" && pwd)"

if [ "$#" -lt 1 ]; then
  echo "usage: run.sh <url>" >&2
  exit 2
fi

# Ensure the generated grammar exists; regenerate from the EBNF if not.
if [ ! -f "$HERE/uri.scm" ]; then
  bash "$EBNF_DIR/run.sh" "$EBNF_DIR/uri.ebnf" >/dev/null || exit 1
fi

# Build the driver if it is missing or out of date.
if [ ! -x "$HERE/uri" ] || [ "$HERE/uri.c" -nt "$HERE/uri" ]; then
  bash "$HERE/build.sh" || exit 1
fi

exec "$HERE/uri" "$1"
