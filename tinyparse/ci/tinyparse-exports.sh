#!/usr/bin/env bash
# ci/tinyparse-exports.sh — the W16-4 export-surface gate.
#
# An embeddable artifact built from generated parsers must export exactly its
# intended API to the host: the parsers' own public entries (<prefix>_load /
# <prefix>_run) plus the public tinyparse_* API the host calls — no
# tinyparse-internal tp_* symbol, and nothing else. (Until the migration out of
# the actor runtime tinyparse was born in, this gate was called hide-ax and also
# kept that runtime's symbols out; the runtime is gone, and the gate now asserts
# the whole exported table is the intended one.)
#
# The recipe that delivers this is examples/embed/embed-lib.sh: it compiles the
# tinyparse LIBRARY AMALGAMATION plus the generated parser-lib translation
# unit(s), and links behind a linker version script that exports exactly the
# parser entries and tinyparse_* and localizes everything else. See
# docs/peg-vs-glr.md (`## W16-4`).
#
# This gate builds that embeddable artifact two ways and asserts, with `nm`:
#   1. a SINGLE generated parser (std.json, prefix `jp`) → its shared object
#      exports `jp_load`/`jp_run` and the public tinyparse_* API, ZERO tp_*
#      internals, and no symbol outside those namespaces;
#   2. the TWO-parser embed example (host_parser + json_parser) → its shared object
#      exports both parsers' four entries and the public API, and nothing else;
#   3. a hardened EXECUTABLE of the two-parser case (driver + parsers + amalgamation,
#      behind the same version script) runs on the committed sample and produces the
#      golden tree byte for byte — proving the restricted artifact still WORKS.
#
# Run from the project root:
#     bash ci/tinyparse-exports.sh
# Exit 0 only when every artifact builds, exports exactly its intended API, and
# the hardened example still parses correctly.
set -uo pipefail

REPO="$(cd "$(dirname "$0")/.." && pwd)"
TP_DIR="$REPO"
EX="$TP_DIR/examples/embed"
cd "$REPO" || exit 1

platform() {
  case "$(uname -s)/$(uname -m)" in
    Linux/x86_64) echo linux-x86_64 ;;
    Darwin/arm64|Darwin/aarch64) echo macos-arm64 ;;
    *) echo linux-x86_64 ;;
  esac
}
PLAT="$(platform)"
TP="$TP_DIR/dist/$PLAT/tinyparse"
AMALG="$TP_DIR/dist/amalgamation"
CC="${CC:-cc}"

echo "== tinyparse export-surface gate (W16-4) =="

if ! command -v nm >/dev/null 2>&1; then
  echo "FATAL: nm not found; cannot verify the exported symbol table"
  exit 1
fi

# 1. Build the CLI (which also bakes the amalgamation) and the amalgamation source
#    the embedding recipe folds in.
( cd "$TP_DIR" && bash build.sh cli ) 2>&1 | sed 's/^/  /'
( cd "$TP_DIR" && bash amalgamate.sh ) >/dev/null 2>&1 || true
if [ ! -x "$TP" ] || [ ! -f "$AMALG/tinyparse.c" ]; then
  echo "FATAL: CLI ($TP) or amalgamation ($AMALG/tinyparse.c) missing"
  echo "RESULT=0/0"
  exit 1
fi

WORK="$(mktemp -d)"
trap 'rm -rf "$WORK"' EXIT
FAIL=0

# ---------------------------------------------------------------------------
# assert_exact SO PREFIX...  — nm the shared object's defined-external dynamic
# table (the surface a host sees): zero tinyparse-internal tp_*, and every
# exported function or object is a named parser entry (<prefix>_load/_run) or
# part of the public tinyparse_* API. Anything else is a leak.
# ---------------------------------------------------------------------------
assert_exact() {
  local so="$1" ; shift
  local allowed="^tinyparse_" prefix tp foreign
  for prefix in "$@"; do allowed="$allowed|^${prefix}_(load|run)\$"; done
  tp="$(nm -D --defined-only "$so" 2>/dev/null | grep -cE ' tp_')"
  if [ "$tp" -ne 0 ]; then
    echo "  FAIL: $so exports $tp tinyparse-internal tp_* symbol(s):"
    nm -D --defined-only "$so" 2>/dev/null | grep -E ' tp_' | sed 's/^/    /' | head
    FAIL=1
  else
    echo "  OK: $so exports 0 tinyparse-internal tp_* symbols"
  fi
  foreign="$(nm -D --defined-only "$so" 2>/dev/null \
    | awk '$2 ~ /^[TDBRVWi]$/ { print $3 }' | grep -vE "$allowed" || true)"
  if [ -n "$foreign" ]; then
    echo "  FAIL: $so exports symbols outside its intended API:"
    printf '%s\n' "$foreign" | sed 's/^/    /' | head
    FAIL=1
  else
    echo "  OK: $so exports only the parser entries and tinyparse_*"
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

# 2. Single generated parser → restricted shared object.
echo "-- single parser (std.json, prefix jp) --"
if ! bash "$EX/embed-lib.sh" "$WORK/libjp.so" jp:std.json 2>&1 | sed 's/^/  /'; then
  echo "  FAIL: embed-lib.sh could not build the single-parser artifact"
  FAIL=1
else
  assert_exact "$WORK/libjp.so" jp
  assert_present "$WORK/libjp.so" jp_load
  assert_present "$WORK/libjp.so" jp_run
  # The public API the host calls must survive (a couple of representatives).
  assert_present "$WORK/libjp.so" tinyparse_open
  assert_present "$WORK/libjp.so" tinyparse_push
fi

# 3. Two-parser embed example → restricted shared object.
echo "-- two parsers (host_parser + json_parser) --"
if ! bash "$EX/embed-lib.sh" "$WORK/libembed.so" \
        host_parser:host.scm json_parser:std.json 2>&1 | sed 's/^/  /'; then
  echo "  FAIL: embed-lib.sh could not build the two-parser artifact"
  FAIL=1
else
  assert_exact "$WORK/libembed.so" host_parser json_parser
  for e in host_parser_load host_parser_run json_parser_load json_parser_run; do
    assert_present "$WORK/libembed.so" "$e"
  done
fi

# 4. A hardened EXECUTABLE of the two-parser case must still parse correctly.
#    Build it from the driver + the two parser TUs + the amalgamation, behind the
#    same kind of version script, then run it on the committed sample and diff the
#    golden. This proves restricting the exports does not break the parse.
echo "-- hardened executable runs correctly --"
"$TP" c "$EX/host.scm" --parser-lib --name host_parser --out "$WORK/host_parser.c" || FAIL=1
"$TP" c std.json       --parser-lib --name json_parser --out "$WORK/json_parser.c" || FAIL=1
"$CC" -std=c11 -O2 -fPIC -c "$AMALG/tinyparse.c" -I"$AMALG" -o "$WORK/tp.o" || FAIL=1
"$CC" -std=c11 -O2 -fPIC -c "$WORK/host_parser.c" -I"$TP_DIR/include" -I"$TP_DIR/src" -o "$WORK/host_parser.o" || FAIL=1
"$CC" -std=c11 -O2 -fPIC -c "$WORK/json_parser.c" -I"$TP_DIR/include" -I"$TP_DIR/src" -o "$WORK/json_parser.o" || FAIL=1
"$CC" -std=c11 -O2 -Wall -Wextra -c "$EX/embed.c" -I"$TP_DIR/include" -o "$WORK/embed.o" || FAIL=1
cat > "$WORK/exe.map" <<'EOF'
{
  global:
    main;
    host_parser_load; host_parser_run;
    json_parser_load; json_parser_run;
    tinyparse_*;
  local:
    *;
};
EOF
if [ "$FAIL" -eq 0 ]; then
  "$CC" -std=c11 -O2 -Wl,--version-script="$WORK/exe.map" -o "$WORK/embed" \
    "$WORK/embed.o" "$WORK/host_parser.o" "$WORK/json_parser.o" "$WORK/tp.o" || FAIL=1
fi
if [ "$FAIL" -eq 0 ] && [ -x "$WORK/embed" ]; then
  tpexe="$(nm -D --defined-only "$WORK/embed" 2>/dev/null | grep -cE ' tp_')"
  if [ "$tpexe" -ne 0 ]; then
    echo "  FAIL: hardened executable exports $tpexe tp_* symbol(s)"
    FAIL=1
  else
    echo "  OK: hardened executable exports 0 tp_* symbols"
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
  echo "== exports: RED =="
  exit 1
fi
echo "RESULT=1/1"
echo "== exports: GREEN (exactly the parser API + tinyparse_*; parse correct) =="
exit 0
