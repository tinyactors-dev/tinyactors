#!/usr/bin/env bash
# ci/tinyparse-multiparser.sh — the W16-3 multi-parser gate.
#
# The generator must emit independent, NON-COLLIDING parsers that coexist in one
# binary: link two parsers generated from two grammars into one program and their
# file-scope symbols must not collide. `tinyparse c ... --parser-lib --name P'
# renames every emitted file-scope symbol to `P_...' so two parsers carry disjoint
# symbol sets; the default (no --name) keeps the historical names, so the cc-diff
# oracle is unaffected (that byte-identity is proved by ci/tinyparse-cc-diff.sh).
#
# This gate proves the capability end to end through the examples/embed example:
#   - a HOST fenced-block grammar (host.scm) that delimits ```...``` code spans,
#   - the std.json parser as the EMBEDDED language inside each fence,
# each compiled to its own parser-lib TU with a distinct --name prefix, linked
# with the embed.c driver into ONE program. The driver parses a sample document
# with the host parser, reads each code span off the host tree, and runs the json
# parser on those bytes. The gate:
#   1. builds the CLI + the host-platform dist (the example links the archives),
#   2. builds the example (which generates both parsers and links them),
#   3. asserts the example's exact output matches the committed golden expected.txt
#      (the host tree AND both embedded json trees), deterministically,
#   4. asserts the linked binary actually carries BOTH parsers' distinct exported
#      entries and no leftover collision-prone default symbol.
#
# Run it with:
#     mise run gate multiparser
#
# Exit 0 only when both parsers build, link, run, and produce the golden trees.
set -uo pipefail

REPO="$(cd "$(dirname "$0")/.." && pwd)"
TP_DIR="$REPO"
EX="$TP_DIR/examples/embed"
cd "$REPO" || exit 1

: "${TP:?run the gates through mise run gate, which builds what they need and sets TP, TP_LIB, TP_CC and TP_NM}"
LIB="$TP_LIB"

echo "== tinyparse multi-parser gate (W16-3) =="

# 1. The CLI and the library archive `mise run gate` built (the generated parsers link against it).
if [ ! -x "$TP" ] || [ ! -f "$LIB" ]; then
  echo "FATAL: CLI ($TP) or libtinyparse ($LIB) not built"
  echo "RESULT=0/0"
  exit 1
fi

# 2. The example, which `mise run examples` builds into $TP_EXAMPLES/embed: it generates BOTH parsers (distinct --name prefixes) and
#    links them with the driver; a link-time symbol collision fails that build.
BUILT="$TP_EXAMPLES/embed"
if [ ! -x "$BUILT/embed" ]; then
  echo "FAIL: example binary $BUILT/embed was not produced (mise run examples)"
  echo "== multiparser: RED =="
  exit 1
fi

# 3. Run it on the committed sample and assert the golden output, byte for byte.
GOT="$("$BUILT/embed" "$EX/sample.md")"
RC=$?
if [ "$RC" -ne 0 ]; then
  echo "FAIL: example exited $RC (expected 0)"
  echo "== multiparser: RED =="
  exit 1
fi
if ! diff -u "$EX/expected.txt" <(printf '%s\n' "$GOT") ; then
  echo "FAIL: example output does not match expected.txt"
  echo "== multiparser: RED =="
  exit 1
fi

# 4. Assert both parsers' distinct entries are present and no collision-prone
#    default symbol survived (proof the --name prefix actually disjointed them).
{
  SYMS="$("$TP_NM" "$BUILT/embed" 2>/dev/null || true)"
  for s in host_parser_load host_parser_run json_parser_load json_parser_run; do
    if ! printf '%s\n' "$SYMS" | grep -qE " T $s$"; then
      echo "FAIL: expected exported entry '$s' missing from the linked binary"
      echo "== multiparser: RED =="
      exit 1
    fi
  done
  if printf '%s\n' "$SYMS" | grep -qE " T tp_compiled_parser_(load|run)$"; then
    echo "FAIL: default-name entry tp_compiled_parser_* survived (no prefix applied)"
    echo "== multiparser: RED =="
    exit 1
  fi
  echo "  both parsers' entries present; no default-name collision symbol"
}

echo "RESULT=1/1"
echo "== multiparser: GREEN (two parsers, two trees, one binary) =="
exit 0
