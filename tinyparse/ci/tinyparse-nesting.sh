#!/usr/bin/env bash
# ci/tinyparse-nesting.sh — the W17-3 NATIVE NESTING gate.
#
# A GENERATED host parser must be able to nest GENERATED embedded parsers, in one
# binary, coordinated by the engine's language registry — and produce the SAME composite
# tree the VM produces. This is the inject hook the codegen blueprint describes:
# the host grammar (std.fence) names embedded languages in its (inject grammar:
# NAME ...) forms but is not linked against their parsers; the host program
# registers each embedded GENERATED parser on the engine with
# tinyparse_register_language(engine, "std.json", {grammar, json_lang_run}), and
# the generated host parser's baked inject sites resolve them by name at runtime.
#
# The gate proves it end to end through examples/nesting:
#   - std.fence       -> the GENERATED host parser (fence_host_*),
#   - std.json        -> a GENERATED embedded parser (json_lang_*),
#   - std.javascript  -> a GENERATED embedded parser (js_lang_*),
# all three compiled with --parser-lib + distinct --name prefixes and linked with
# the nesting.c driver into ONE program. The gate:
#   1. builds the CLI + the host-platform dist (the example links the archives),
#   2. builds the example (which generates all three parsers and links them),
#   3. asserts the example's composite tree is BYTE-IDENTICAL to the VM's
#      `run std.fence --dump-tree' on the same document (the real oracle), and to
#      the committed golden expected.txt,
#   4. asserts the linked binary carries all three parsers' distinct entries, and
#      that a shared object of the set exports only the parsers' entries and the
#      public tinyparse_* API (the export property ci/tinyparse-exports.sh
#      checks for the embed example).
#
# Run it with:
#     mise run gate nesting
#
# Exit 0 only when the generated host nests the generated embedded parsers and the
# composite tree matches the VM byte for byte.
set -uo pipefail

REPO="$(cd "$(dirname "$0")/.." && pwd)"
TP_DIR="$REPO"
EX="$TP_DIR/examples/nesting"
cd "$REPO" || exit 1

: "${TP:?run the gates through mise run gate, which builds what they need and sets TP, TP_LIB, TP_CC and TP_NM}"
LIB="$TP_LIB"

echo "== tinyparse native-nesting gate (W17-3) =="

# 1. The CLI and the library archive `mise run gate` built (the generated parsers link against it).
if [ ! -x "$TP" ] || [ ! -f "$LIB" ]; then
  echo "FATAL: CLI ($TP) or libtinyparse ($LIB) not built"
  echo "RESULT=0/0"
  exit 1
fi

# 2. The example, which `mise run examples` builds into $TP_EXAMPLES/nesting:
#    all THREE parsers generated (distinct --name prefixes) and linked with the
#    driver; a link-time symbol collision fails that build.
BUILT="$TP_EXAMPLES/nesting"
if [ ! -x "$BUILT/nesting" ]; then
  echo "FAIL: example binary $BUILT/nesting was not produced (mise run examples)"
  echo "== nesting: RED =="
  exit 1
fi

# 3a. The composite tree the generated host+embedded parsers build must be
#     BYTE-IDENTICAL to the VM's --dump-tree of the SAME std.fence grammar on the
#     SAME document. This is the oracle: native nesting changes nothing about the CST.
GOT="$("$BUILT/nesting" "$EX/sample.md")"
RC=$?
if [ "$RC" -ne 0 ]; then
  echo "FAIL: example exited $RC (expected 0)"
  echo "== nesting: RED =="
  exit 1
fi
VM="$("$TP" run std.fence --input "$EX/sample.md" --dump-tree)"
if ! diff -u <(printf '%s\n' "$VM") <(printf '%s\n' "$GOT") ; then
  echo "FAIL: generated nesting tree differs from the VM (native nesting must match)"
  echo "== nesting: RED =="
  exit 1
fi
echo "  generated host nests generated embedded: tree byte-identical to the VM"

# 3b. And to the committed golden, so a silent drift in either is caught.
if ! diff -u "$EX/expected.txt" <(printf '%s\n' "$GOT") ; then
  echo "FAIL: example output does not match the committed expected.txt"
  echo "== nesting: RED =="
  exit 1
fi

# 4. All three parsers' distinct generated entries are present in the linked
#    binary (proof the prefixes disjointed three full parser symbol sets).
if true; then
  SYMS="$("$TP_NM" "$BUILT/nesting" 2>/dev/null || true)"
  for s in fence_host_load fence_host_run json_lang_load json_lang_run \
           js_lang_load js_lang_run; do
    if ! printf '%s\n' "$SYMS" | grep -qE " T $s$"; then
      echo "FAIL: expected generated entry '$s' missing from the linked binary"
      echo "== nesting: RED =="
      exit 1
    fi
  done
  echo "  all three parsers' entries present (no prefix collision)"
fi

# 5. The export property for the nesting set: build a SHARED OBJECT of all three
#    parsers over the amalgamated runtime behind a version script, and assert
#    `nm -D' exports the parser entries + public tinyparse_* API and ZERO
#    library-internal tp_ symbols. A plain static executable always shows tp_ in
#    its full symbol table; the exported surface of the embeddable artifact is
#    what must be clean, and a host that nests these parsers ships that artifact.
if true; then
  SODIR="$(mktemp -d)"
  SO="$SODIR/libnesting.so"
  if ! ( cd "$TP_DIR/examples/embed" && \
         CC="$TP_CC" TP_CLI="$TP" bash embed-lib.sh "$SO" \
           fence_host:std.fence json_lang:std.json js_lang:std.javascript ) \
       2>&1 | sed 's/^/  /'; then
    echo "FAIL: shared object for the nesting set did not build"
    echo "== nesting: RED =="
    exit 1
  fi
  DYN="$("$TP_NM" -D --defined-only "$SO" 2>/dev/null || true)"
  if printf '%s\n' "$DYN" | grep -qE " [TtWwiDdBbRr] tp_"; then
    echo "FAIL: a library-internal tp_ symbol is EXPORTED from the shared object"
    printf '%s\n' "$DYN" | grep -E " [TtWwiDdBbRr] tp_" | head -5 | sed 's/^/      /'
    echo "== nesting: RED =="
    exit 1
  fi
  for s in fence_host_run json_lang_run js_lang_run; do
    if ! printf '%s\n' "$DYN" | grep -qE " T $s$"; then
      echo "FAIL: shared object does not export generated entry '$s'"
      echo "== nesting: RED =="
      exit 1
    fi
  done
  rm -rf "$SODIR"
  echo "  shared object exports the parsers + tinyparse_* and zero tp_ internals"
fi

echo "RESULT=1/1"
echo "== nesting: GREEN (generated host nests generated embedded, == VM) =="
exit 0
