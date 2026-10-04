#!/usr/bin/env bash
# examples/ebnf/run.sh <file.ebnf>
#
# Convert an EBNF grammar into a tinyparse .scm grammar. It prints the .scm to
# stdout AND writes a copy to examples/uri/uri.scm, so the URI example downstream
# consumes a freshly generated grammar.
#
# Works whether cwd is tinyparse/ (as the data tests run it) or this
# directory: paths are resolved from the script's own location.
set -uo pipefail

HERE="$(cd "$(dirname "$0")" && pwd)"
URI_DIR="$(cd "$HERE/../uri" && pwd)"

if [ "$#" -lt 1 ]; then
  echo "usage: run.sh <file.ebnf>" >&2
  exit 2
fi

INPUT="$1"

# Build the converter if it is missing or out of date.
if [ ! -x "$HERE/ebnf2scm" ] || [ "$HERE/ebnf2scm.c" -nt "$HERE/ebnf2scm" ]; then
  bash "$HERE/build.sh" || exit 1
fi

# Emit to stdout and, in the same pass, persist the grammar for the URI example.
"$HERE/ebnf2scm" "$INPUT" | tee "$URI_DIR/uri.scm"
exit "${PIPESTATUS[0]}"
