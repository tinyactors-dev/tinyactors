#!/usr/bin/env bash
# ci/pocketparse-parser-nocrash.sh — the W14-3 crash-prevention proof.
#
# The compiled parser (`pocketparse c --parser`) is ITERATIVE: the grammar runs
# on an explicit, arena-backed frame stack driven by one dispatch loop, with no
# C recursion across grammar nesting. So its working set is bounded by the parse
# ARENA, not by the host's C call stack. A naive recursive descent would recurse
# once per nesting level and blow the C stack on a deeply nested input; the
# iterative parser cannot.
#
# This script proves the guarantee with a TOOL, not a depth search (see the
# profiling skill: a no-crash claim is "exits without a crash signal on
# pathological input," verified once under a bound — never "depth N works" for a
# magic N). It:
#   1. compiles the std.json parser (self-contained; no React/gitignored tree),
#   2. generates ONE pathologically deep input (~200k nested '['), deep enough
#      that a recursive descent would overflow even a normal C stack,
#   3. runs the parser under a DELIBERATELY SMALL C stack (ulimit -s 512, in a
#      subshell) and asserts it exits WITHOUT a crash signal: exit code < 128.
#      A clean parse or a graceful syntax error are both fine; only a death by
#      signal (SIGSEGV → 139, SIGABRT → 134, the 128+N range) is a failure.
#
# One representative deep input under a reduced stack is the whole test. Do not
# sweep depths: the guarantee is the arena bound, verified here once.
#
# Run it from the repo root:
#     bash ci/pocketparse-parser-nocrash.sh
#
# Exit 0 only when the parser survives the deep input under the small stack.
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

echo "== pocketparse compiled-parser crash-prevention (nocrash) =="

# Build the CLI + the library archive (the generated parser links against it).
( cd "$PP_DIR" && bash build.sh cli ) 2>&1 | sed 's/^/  /'
if [ ! -x "$PP" ] || [ ! -f "$LIB" ]; then
  echo "FATAL: CLI ($PP) or libpocketparse ($LIB) not built"
  exit 1
fi

CC="${CC:-cc}"
SCRATCH="$(mktemp -d)"
trap 'rm -rf "$SCRATCH"' EXIT

# 1. Compile the std.json parser (self-contained; std.json needs no other pkg).
CFILE="$SCRATCH/json_parser.c"
BIN="$SCRATCH/json_parser"
if ! "$PP" c std.json --parser --out "$CFILE" 2>"$SCRATCH/gen.err"; then
  echo "FATAL: parser generation failed: $(head -1 "$SCRATCH/gen.err")"
  exit 1
fi
if ! "$CC" -std=c11 -O2 -I"$INC" -I"$SRC" \
      "$CFILE" "$LIB" -o "$BIN" 2>"$SCRATCH/cc.err"; then
  echo "FATAL: parser compilation failed:"
  sed 's/^/    /' "$SCRATCH/cc.err" | head -20
  exit 1
fi

# 2. ONE pathologically deep input: ~200k nested '['. This is deliberately
#    malformed (never closed); it does not need to parse cleanly — it only needs
#    to drive the dispatch loop to ~200k frames of nesting, far past what a
#    recursive C stack survives.
DEPTH=200000
INPUT="$SCRATCH/deep.json"
yes '[' | head -n "$DEPTH" | tr -d '\n' > "$INPUT"
echo "  deep input: $DEPTH nested '[' ($(wc -c < "$INPUT") bytes)"

# 3. Run under a deliberately SMALL C stack (512 KiB) in a subshell, so a
#    recursion bug surfaces immediately while the arena-bounded iterative parser
#    survives. We only care that it does NOT die by signal.
(
  ulimit -s 512
  "$BIN" < "$INPUT" > /dev/null 2>&1
)
rc=$?

echo "  ran under ulimit -s 512; exit code = $rc"

if [ "$rc" -ge 128 ]; then
  sig=$((rc - 128))
  echo "  FAIL: parser died by signal $sig (exit $rc) — the C stack overflowed."
  echo "== nocrash: RED =="
  exit 1
fi

echo "  PASS: parser exited without a crash signal (graceful, arena-bounded)."
echo "== nocrash: GREEN =="
exit 0
