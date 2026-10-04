#!/usr/bin/env bash
# ci/pocketparse-vm-nocrash.sh — the W14-6 bytecode-VM crash-prevention proof.
#
# The bytecode VM (`pocketparse run ... ` under POCKETPARSE_ENGINE=vm / vm-ax) is
# now ITERATIVE: rule calls run on an explicit, arena-backed frame stack driven by
# one dispatch loop (pp_vm_run in src/vm.c), with NO C recursion across rule calls.
# So its working set is bounded by the parse ARENA, not by the host's C call stack.
# Before W14-6 the VM C-recursed per rule call (pp_vm_run -> pp_vm_run_call ->
# pp_vm_run), so a deeply nested input overflowed the C stack and the VM SEGFAULTED
# under a small stack — the same crash the W3 compiled parser already fixed. The
# iterative VM cannot.
#
# This script proves the guarantee with a TOOL, not a depth search (see the
# profiling skill: a no-crash claim is "exits without a crash signal on
# pathological input," verified once under a bound — never "depth N works" for a
# magic N). It:
#   1. builds the CLI (which carries the VM, selected at run time),
#   2. generates ONE pathologically deep input (~200k nested '['), deep enough
#      that a recursive descent would overflow even a normal C stack,
#   3. runs the VM (POCKETPARSE_ENGINE=vm and =vm-ax) under a DELIBERATELY SMALL C
#      stack (ulimit -s 512, in a subshell) and asserts it exits WITHOUT a crash
#      signal: exit code < 128. A clean parse or a graceful syntax error are both
#      fine; only a death by signal (SIGSEGV -> 139, the 128+N range) is a failure.
#
# The test's MEANINGFULNESS was established when the recursive IR interpreter
# (pp_execute) still existed: it C-recursed per rule and SEGFAULTED (139) on this
# exact input under this exact small stack, where the iterative VM survives. That
# interpreter was deleted in W14-5, so the inverse proof is no longer runnable from
# this script; it is recorded in docs/decisions.md (W14-6) and the bound has not
# changed since. The positive assertion below — the VM exits without a crash signal
# — remains the live gate.
#
# One representative deep input under a reduced stack is the whole test. Do not
# sweep depths: the guarantee is the arena bound, verified here once.
#
# Run it from the repo root:
#     bash ci/pocketparse-vm-nocrash.sh
#
# Exit 0 only when the VM survives the deep input under the small stack.
set -uo pipefail

REPO="${CLAUDE_PROJECT_DIR:-$(cd "$(dirname "$0")/.." && pwd)}"
PP_DIR="$REPO/tools/pocketparse"
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

echo "== pocketparse bytecode-VM crash-prevention (vm-nocrash) =="

# Build the CLI (carries the VM; the engine is selected at run time).
( cd "$PP_DIR" && bash build.sh cli ) 2>&1 | sed 's/^/  /'
if [ ! -x "$PP" ]; then
  echo "FATAL: CLI not built at $PP"
  exit 1
fi

SCRATCH="$(mktemp -d)"
trap 'rm -rf "$SCRATCH"' EXIT

# ONE pathologically deep input: ~200k nested '['. Deliberately malformed (never
# closed); it does not need to parse cleanly — it only needs to drive the dispatch
# loop to ~200k frames of rule-call nesting, far past what a recursive C stack
# survives. std.json is self-contained (needs no other package).
DEPTH=200000
INPUT="$SCRATCH/deep.json"
yes '[' | head -n "$DEPTH" | tr -d '\n' > "$INPUT"
echo "  deep input: $DEPTH nested '[' ($(wc -c < "$INPUT") bytes)"

# Run one engine under a deliberately SMALL C stack (512 KiB) in a subshell, so a
# C-recursion bug surfaces immediately while the arena-bounded iterative VM
# survives. Echoes the exit code; the caller decides pass/fail.
run_under_small_stack() { # engine
  (
    ulimit -s 512
    POCKETPARSE_ENGINE="$1" "$PP" run std.json --input "$INPUT" --dump-tree \
      > /dev/null 2>&1
  )
  echo $?
}

fail=0
for ENGINE in vm vm-ax; do
  rc="$(run_under_small_stack "$ENGINE")"
  echo "  POCKETPARSE_ENGINE=$ENGINE under ulimit -s 512: exit code = $rc"
  if [ "$rc" -ge 128 ]; then
    sig=$((rc - 128))
    echo "    FAIL: VM died by signal $sig (exit $rc) — the C stack overflowed."
    fail=1
  else
    echo "    PASS: VM exited without a crash signal (graceful, arena-bounded)."
  fi
done

# W15-2 lexer angle: a SINGLE token whose repeat must consume an enormous run of
# bytes. Before W15-2 the lexer matched tokens with the recursive pp_match_ir; its
# repeat looped (not recursed) per byte, so this never crashed the lexer — but the
# point of W15-2 is that token matching is now ITERATIVE bytecode (a jump-threaded
# loop, the repeat fused into one tight C loop), so a token spanning the whole input
# stays bounded too. A ~400k-byte number literal is one token; matching it drives the
# fused repeat loop over 400k bytes in constant C-stack space.
LEXINPUT="$SCRATCH/bignum.json"
yes '1234567890' | head -n 40000 | tr -d '\n' > "$LEXINPUT"
echo "  big-token input: one $(wc -c < "$LEXINPUT")-byte number literal (lexer repeat loop)"
for ENGINE in vm vm-ax; do
  rc="$( ( ulimit -s 512; POCKETPARSE_ENGINE="$ENGINE" "$PP" run std.json --input "$LEXINPUT" --dump-tree > /dev/null 2>&1 ); echo $? )"
  echo "  POCKETPARSE_ENGINE=$ENGINE big-token under ulimit -s 512: exit code = $rc"
  if [ "$rc" -ge 128 ]; then
    sig=$((rc - 128))
    echo "    FAIL: lexer died by signal $sig (exit $rc) on a huge single token."
    fail=1
  else
    echo "    PASS: lexer exited without a crash signal (iterative token bytecode)."
  fi
done

# The inverse proof (the recursive IR interpreter crashing on this same input under
# this same stack) ran while pp_execute existed; it was deleted in W14-5, so it is
# no longer runnable here. The bound is unchanged and the meaningfulness is recorded
# in docs/decisions.md (W14-6).
echo "  cross-check: the recursive IR interpreter that crashed here was retired in W14-5"
echo "    (its segfault under this bound is recorded in docs/decisions.md; bound unchanged)."

if [ "$fail" -ne 0 ]; then
  echo "== vm-nocrash: RED =="
  exit 1
fi
echo "== vm-nocrash: GREEN =="
exit 0
