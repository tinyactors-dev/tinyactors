#!/usr/bin/env bash
# examples/ebnf/run.sh <file.ebnf>
#
# Convert an EBNF grammar into a tinyparse .scm grammar. It prints the .scm to
# stdout AND writes a copy to examples/uri/uri.scm, so the URI example downstream
# consumes a freshly generated grammar.
#
# The converter is built by `mise run examples` into build/examples/ebnf/
# ($TP_EXAMPLES/ebnf when the test suite runs it). Works whether cwd is
# tinyparse/ (as the data tests run it) or this directory: paths are resolved
# from the script's own location.
set -uo pipefail

HERE="$(cd "$(dirname "$0")" && pwd)"
URI_DIR="$(cd "$HERE/../uri" && pwd)"
BIN="${TP_EXAMPLES:-$HERE/../../build/examples}/ebnf/ebnf2scm"

if [ "$#" -lt 1 ]; then
  echo "usage: run.sh <file.ebnf>" >&2
  exit 2
fi
if [ ! -x "$BIN" ]; then
  echo "run.sh: $BIN is not built; run: mise run examples" >&2
  exit 1
fi

INPUT="$1"

# Emit to stdout and persist the grammar for the URI example. The copy is
# written beside it and renamed into place only when its bytes differ, so a
# reader of uri.scm running at the same time (the test suite runs the URI
# example and the gates read uri.scm while this runs) never sees it half
# written, and an unchanged grammar leaves the committed file untouched.
TMP="$(mktemp "$URI_DIR/.uri.scm.XXXXXX")" || exit 1
trap 'rm -f "$TMP"' EXIT
"$BIN" "$INPUT" >"$TMP" || exit $?
cat "$TMP"
if ! cmp -s "$TMP" "$URI_DIR/uri.scm"; then
  chmod 644 "$TMP" && mv "$TMP" "$URI_DIR/uri.scm" || exit 1
fi
