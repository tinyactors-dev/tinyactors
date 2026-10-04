#!/usr/bin/env bash
# ci/pocketparse-hide-ax.sh — the W16-4 ax-visibility gate.
#
# The generated C parser uses the ax actor engine internally (ax is the
# freestanding runtime it links against), but an embeddable artifact built from
# the generated parser must NOT export ax to the host: its exported symbol table
# should carry ONLY the parser's own public entries (<prefix>_load / <prefix>_run)
# plus the public pocketparse_* API the host calls — no ax_* symbol, and no
# pocketparse-internal pp_* symbol.
#
# The recipe that delivers this is examples/embed/hide-ax.sh: it compiles the
# pocketparse LIBRARY AMALGAMATION (which folds ax in under hidden visibility) plus
# the generated parser-lib translation unit(s), and links behind a linker version
# script that exports exactly the parser entries and pocketparse_* and localizes
# everything else (so ax_* AND the pocketparse-internal pp_* are local). See
# docs/peg-vs-glr.md (`## W16-4`).
#
# This gate builds that embeddable artifact two ways and asserts, with `nm`:
#   1. a SINGLE generated parser (std.json, prefix `jp`) → its shared object
#      exports `jp_load`/`jp_run` and the public pocketparse_* API and ZERO ax_*
#      and ZERO pp_* internals;
#   2. the TWO-parser embed example (host_parser + json_parser) → its shared object
#      exports both parsers' four entries, the public API, and again ZERO ax_*;
#   3. a hardened EXECUTABLE of the two-parser case (driver + parsers + amalgamation,
#      behind the same version script) runs on the committed sample and produces the
#      golden tree byte for byte — proving the ax-hidden artifact still WORKS.
#
# Run from the repo root:
#     bash ci/pocketparse-hide-ax.sh
# Exit 0 only when every artifact builds, leaks no ax_, keeps its public API, and
# the hardened example still parses correctly.
set -uo pipefail

REPO="${CLAUDE_PROJECT_DIR:-$(cd "$(dirname "$0")/.." && pwd)}"
PP_DIR="$REPO/tools/pocketparse"
EX="$PP_DIR/examples/embed"
cd "$REPO" || exit 1

platform() {
  case "$(uname -s)/$(uname -m)" in
    Linux/x86_64) echo linux-x86_64 ;;
    Darwin/arm64|Darwin/aarch64) echo macos-arm64 ;;
    *) echo linux-x86_64 ;;
  esac
}
PLAT="$(platform)"
PP="$PP_DIR/dist/$PLAT/pocketparse"
AMALG="$PP_DIR/dist/amalgamation"
AXDIR="$REPO/dist/$PLAT"
CC="${CC:-cc}"

echo "== pocketparse hide-ax gate (W16-4) =="

if ! command -v nm >/dev/null 2>&1; then
  echo "FATAL: nm not found; cannot verify the exported symbol table"
  exit 1
fi

# 1. Build the CLI (which also bakes the amalgamation) and the amalgamation source
#    the embedding recipe folds in.
( cd "$PP_DIR" && bash build.sh cli ) 2>&1 | sed 's/^/  /'
( cd "$PP_DIR" && bash amalgamate.sh ) >/dev/null 2>&1 || true
if [ ! -x "$PP" ] || [ ! -f "$AMALG/pocketparse.c" ] || [ ! -f "$AXDIR/ax.h" ]; then
  echo "FATAL: CLI ($PP), amalgamation ($AMALG/pocketparse.c) or ax.h ($AXDIR/ax.h) missing"
  echo "RESULT=0/0"
  exit 1
fi

WORK="$(mktemp -d)"
trap 'rm -rf "$WORK"' EXIT
FAIL=0

# ---------------------------------------------------------------------------
# assert_hidden SO  — nm the shared object: zero exported ax_*, zero exported
# pocketparse-internal pp_*. The defined-external dynamic table is the surface a
# host sees; ax may exist only as a LOCAL symbol (lowercase t/d) or be absent.
# ---------------------------------------------------------------------------
assert_hidden() {
  local so="$1" ; shift
  local ax pp
  ax="$(nm -D --defined-only "$so" 2>/dev/null | grep -c ' ax_')"
  pp="$(nm -D --defined-only "$so" 2>/dev/null | grep -cE ' pp_')"
  if [ "$ax" -ne 0 ]; then
    echo "  FAIL: $so exports $ax ax_* symbol(s):"
    nm -D --defined-only "$so" 2>/dev/null | grep ' ax_' | sed 's/^/    /' | head
    FAIL=1
  else
    echo "  OK: $so exports 0 ax_* symbols"
  fi
  if [ "$pp" -ne 0 ]; then
    echo "  FAIL: $so exports $pp pocketparse-internal pp_* symbol(s):"
    nm -D --defined-only "$so" 2>/dev/null | grep -E ' pp_' | sed 's/^/    /' | head
    FAIL=1
  else
    echo "  OK: $so exports 0 pocketparse-internal pp_* symbols"
  fi
}

# ---------------------------------------------------------------------------
# assert_present SO ENTRY  — the named entry IS exported (defined, external).
# ---------------------------------------------------------------------------
assert_present() {
  local so="$1" entry="$2"
  if nm -D --defined-only "$so" 2>/dev/null | grep -qE " T $entry\$"; then
    echo "  OK: $so exports $entry"
  else
    echo "  FAIL: $so does NOT export the public entry $entry"
    FAIL=1
  fi
}

# 2. Single generated parser → ax-hidden shared object.
echo "-- single parser (std.json, prefix jp) --"
if ! bash "$EX/hide-ax.sh" "$WORK/libjp.so" jp:std.json 2>&1 | sed 's/^/  /'; then
  echo "  FAIL: hide-ax.sh could not build the single-parser artifact"
  FAIL=1
else
  assert_hidden "$WORK/libjp.so"
  assert_present "$WORK/libjp.so" jp_load
  assert_present "$WORK/libjp.so" jp_run
  # The public API the host calls must survive (a couple of representatives).
  assert_present "$WORK/libjp.so" pocketparse_open
  assert_present "$WORK/libjp.so" pocketparse_push
fi

# 3. Two-parser embed example → ax-hidden shared object.
echo "-- two parsers (host_parser + json_parser) --"
if ! bash "$EX/hide-ax.sh" "$WORK/libembed.so" \
        host_parser:host.scm json_parser:std.json 2>&1 | sed 's/^/  /'; then
  echo "  FAIL: hide-ax.sh could not build the two-parser artifact"
  FAIL=1
else
  assert_hidden "$WORK/libembed.so"
  for e in host_parser_load host_parser_run json_parser_load json_parser_run; do
    assert_present "$WORK/libembed.so" "$e"
  done
fi

# 4. A hardened EXECUTABLE of the two-parser case must still parse correctly.
#    Build it from the driver + the two parser TUs + the amalgamation, behind the
#    same kind of version script, then run it on the committed sample and diff the
#    golden. This proves hiding ax does not break the parse.
echo "-- hardened executable runs correctly --"
"$PP" c "$EX/host.scm" --parser-lib --name host_parser --out "$WORK/host_parser.c" || FAIL=1
"$PP" c std.json       --parser-lib --name json_parser --out "$WORK/json_parser.c" || FAIL=1
"$CC" -std=c11 -O2 -fPIC -c "$AMALG/pocketparse.c" -I"$AMALG" -o "$WORK/pp.o" || FAIL=1
"$CC" -std=c11 -O2 -fPIC -c "$WORK/host_parser.c" -I"$PP_DIR/include" -I"$PP_DIR/src" -I"$AXDIR" -o "$WORK/host_parser.o" || FAIL=1
"$CC" -std=c11 -O2 -fPIC -c "$WORK/json_parser.c" -I"$PP_DIR/include" -I"$PP_DIR/src" -I"$AXDIR" -o "$WORK/json_parser.o" || FAIL=1
"$CC" -std=c11 -O2 -Wall -Wextra -c "$EX/embed.c" -I"$PP_DIR/include" -o "$WORK/embed.o" || FAIL=1
cat > "$WORK/exe.map" <<'EOF'
{
  global:
    main;
    host_parser_load; host_parser_run;
    json_parser_load; json_parser_run;
    pocketparse_*;
  local:
    *;
};
EOF
if [ "$FAIL" -eq 0 ]; then
  "$CC" -std=c11 -O2 -Wl,--version-script="$WORK/exe.map" -o "$WORK/embed" \
    "$WORK/embed.o" "$WORK/host_parser.o" "$WORK/json_parser.o" "$WORK/pp.o" || FAIL=1
fi
if [ "$FAIL" -eq 0 ] && [ -x "$WORK/embed" ]; then
  axexe="$(nm "$WORK/embed" 2>/dev/null | grep -cE ' [TDB] ax_')"
  if [ "$axexe" -ne 0 ]; then
    echo "  FAIL: hardened executable exports $axexe ax_* symbol(s)"
    FAIL=1
  else
    echo "  OK: hardened executable exports 0 ax_* symbols"
  fi
  GOT="$("$WORK/embed" "$EX/sample.md")"
  if [ "$?" -ne 0 ]; then
    echo "  FAIL: hardened executable exited nonzero"
    FAIL=1
  elif ! diff -u "$EX/expected.txt" <(printf '%s\n' "$GOT") >/dev/null; then
    echo "  FAIL: hardened executable output does not match expected.txt"
    diff -u "$EX/expected.txt" <(printf '%s\n' "$GOT") | sed 's/^/    /' | head
    FAIL=1
  else
    echo "  OK: hardened executable parses the sample to the golden tree"
  fi
fi

if [ "$FAIL" -ne 0 ]; then
  echo "RESULT=0/1"
  echo "== hide-ax: RED =="
  exit 1
fi
echo "RESULT=1/1"
echo "== hide-ax: GREEN (no exported ax_; parser API present; parse correct) =="
exit 0
