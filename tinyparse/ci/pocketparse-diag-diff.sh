#!/usr/bin/env bash
# ci/pocketparse-diag-diff.sh — the W17-1 diagnostics-parity gate.
#
# cc-diff already pins the recovered TREE (the MISSING nodes a (recover ...) form
# synthesizes) byte-for-byte between the VM and the generated C parser. It does
# NOT separately pin the DIAGNOSTICS: the message/help/label text, the severity,
# and that error severity is what drives the non-zero exit. This gate does, on a
# committed corpus of BROKEN inputs to a (recover ...) grammar.
#
# Phase 17 rewrites the generated parser's emitter to idiomatic recursive descent
# (W17-2). The owner's explicit fear is that the rewrite quietly drops the
# detailed error reporting that distinguishes pocketparse. This gate is the net
# that keeps it honest: for every broken input it runs BOTH the bytecode VM
# (`pocketparse run`, the differential oracle) and the freshly generated C
# parser, and asserts that
#
#   - STDOUT (the --dump-tree output, including each MISSING node in its pre-order
#     position) is byte-identical, so the recovered tree and the placement of the
#     synthesized missing nodes match; and
#   - STDERR (every diagnostic: the primary message, the [code], the help line --
#     printed only at the severity that prints it) is byte-identical; and
#   - the EXIT CODE matches, so the severity that drives a non-zero exit (an
#     error-severity diagnostic) is preserved.
#
# Together these pin message / help / label / severity AND the missing-node
# positions, which is the requirement W17-2 must not regress. Both sides render
# diagnostics through the same code (cli.c's render_diagnostic and the emitter's
# byte-identical pp_parser_diag), so a divergence here is a real behavioural drift,
# not a formatting artifact.
#
# Run it from the repo root:
#     bash ci/pocketparse-diag-diff.sh
#
# Exit 0 only when stdout, stderr, and exit code matched for every broken input.
set -uo pipefail

REPO="$(cd "$(dirname "$0")/.." && pwd)"
PP_DIR="$REPO"
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
LIB="$PP_DIR/dist/$PLAT/libpocketparse.a"
INC="$PP_DIR/include"
SRC="$PP_DIR/src"

echo "== pocketparse diagnostics-parity differential (diag-diff) =="

# 1. Build the CLI + the library archive (the generated parser links against it).
( cd "$PP_DIR" && bash build.sh cli ) 2>&1 | sed 's/^/  /'
if [ ! -x "$PP" ] || [ ! -f "$LIB" ] || [ ! -f ]; then
  echo "FATAL: CLI ($PP) or libpocketparse ($LIB) not built"
  echo "RESULT=0/0"
  exit 1
fi

CC="${CC:-cc}"
SCRATCH="$(mktemp -d)"
trap 'rm -rf "$SCRATCH"' EXIT

# Generate, compile and cache the parser for one grammar; echo its binary path.
build_parser() { # grammar
  local g="$1"
  local key="${g//\//_}"
  local cfile="$SCRATCH/parser_$key.c"
  local bin="$SCRATCH/parser_$key"
  if [ -x "$bin" ]; then echo "$bin"; return 0; fi
  if ! "$PP" c "$g" --parser --out "$cfile" 2>"$SCRATCH/gen_$key.err"; then
    echo "  GEN FAIL ($g): $(head -1 "$SCRATCH/gen_$key.err")" >&2
    return 1
  fi
  if ! "$CC" -std=c11 -O2 -I"$INC" -I"$SRC" \
        "$cfile" "$LIB" -o "$bin" 2>"$SCRATCH/cc_$key.err"; then
    echo "  CC FAIL ($g):" >&2
    sed 's/^/      /' "$SCRATCH/cc_$key.err" | head -20 >&2
    return 1
  fi
  echo "$bin"
}

fail=0
total=0
match=0

# Compare the generated parser's diagnostics + tree against the VM's, for one
# broken input. Captures stdout (the dumped tree with its MISSING nodes) and
# stderr (the diagnostics) SEPARATELY, plus the exit code, and asserts all three
# match. This is strictly stronger than cc-diff's combined `2>&1' capture.
diag_one() { # grammar parser-binary input-file
  local g="$1" bin="$2" f="$3"
  total=$((total + 1))
  "$bin" < "$f" > "$f.gen.out" 2> "$f.gen.err"; local rc1=$?
  "$PP" run "$g" --input "$f" --dump-tree \
      > "$f.vm.out" 2> "$f.vm.err"; local rc2=$?
  local ok=1
  [ "$rc1" = "$rc2" ] || ok=0
  diff -q "$f.gen.out" "$f.vm.out" >/dev/null || ok=0
  diff -q "$f.gen.err" "$f.vm.err" >/dev/null || ok=0
  if [ "$ok" = 1 ]; then
    match=$((match + 1))
  else
    fail=$((fail + 1))
    echo "  MISMATCH: $g / $(basename "$f") (rc gen=$rc1 vm=$rc2)"
    echo "    --- tree (stdout) diff (vm vs gen) ---"
    diff "$f.vm.out" "$f.gen.out" | sed 's/^/      /' | head -20
    echo "    --- diagnostics (stderr) diff (vm vs gen) ---"
    diff "$f.vm.err" "$f.gen.err" | sed 's/^/      /' | head -20
  fi
  rm -f "$f.gen.out" "$f.gen.err" "$f.vm.out" "$f.vm.err"
}

# The (recover ...) grammar and its committed broken-input corpus. list.scm wraps
# its List rule in (recover ... (on eof message:/label:/help:/insert:)); each
# input under broken/ ends with a list still open, so recovery fires, a MISSING )
# is synthesized, and the "unterminated list" diagnostic is raised.
RECOVER_GRAMMAR="$PP_DIR/examples/recover/list.scm"
BROKEN_DIR="$PP_DIR/examples/recover/broken"

echo "== recover grammar: $(basename "$RECOVER_GRAMMAR") =="
if ! bin="$(build_parser "$RECOVER_GRAMMAR")"; then
  echo "FATAL: could not build the recover parser"
  echo "RESULT=0/0"
  exit 1
fi

shopt -s nullglob
inputs=("$BROKEN_DIR"/*.list)
shopt -u nullglob
if [ "${#inputs[@]}" -eq 0 ]; then
  echo "FATAL: no broken-input corpus under $BROKEN_DIR"
  echo "RESULT=0/0"
  exit 1
fi
for f in "${inputs[@]}"; do
  diag_one "$RECOVER_GRAMMAR" "$bin" "$f"
done

echo "== diag-diff TOTAL: $match/$total diagnostics+tree byte-identical =="
echo "RESULT=$match/$total"

if [ "$fail" -eq 0 ] && [ "$total" -gt 0 ]; then
  echo "== diag-diff: GREEN (VM and generated parser report identical diagnostics) =="
  exit 0
fi
echo "== diag-diff: RED =="
exit 1
