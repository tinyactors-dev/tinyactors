#!/usr/bin/env bash
# ci/pocketparse-metric.sh — the finish line for pocketparse, as a program.
#
# It builds what it can, then runs the DATA-driven test harness over the seeded
# suite and the xfail ledger, and prints PROGRESS=k/n. It exits 0 ONLY when
# every test passes AND tools/pocketparse/test/xfail.list is empty.
#
# Fast by default (host build + harness). RUN_GATE=1 also cross-builds the full
# dist matrix and forces the examples to build — the thorough proof.
#
# See tools/pocketparse/docs/build-plan.md.
set -uo pipefail

REPO="${CLAUDE_PROJECT_DIR:-$(cd "$(dirname "$0")/.." && pwd)}"
PP="$REPO/tools/pocketparse"
cd "$REPO" || exit 1

platform() {
  case "$(uname -s)/$(uname -m)" in
    Linux/x86_64) echo linux-x86_64 ;;
    Darwin/arm64|Darwin/aarch64) echo macos-arm64 ;;
    *) echo linux-x86_64 ;;
  esac
}
PLAT="$(platform)"

echo "== pocketparse metric =="

# 1. Build the harness (host dev tool; libc is fine here).
HARNESS="$PP/build/harness"
mkdir -p "$PP/build"
if ! cc -std=c11 -O2 -Wall -Wextra -o "$HARNESS" "$PP/test/harness.c" 2>"$PP/build/harness.log"; then
  echo "FATAL: test harness did not compile:"
  cat "$PP/build/harness.log"
  echo "PROGRESS=0/0"
  exit 1
fi

# 2. Build pocketparse itself (best-effort; missing pieces are reported as red).
#    Always cross-build the full dist matrix + examples so the result is
#    self-contained and truthful on a fresh container. RUN_GATE is reserved for
#    any extra-expensive behavioural proof a workstream wires in later.
( cd "$PP" && bash build.sh dist ) 2>&1 | sed 's/^/  /'
( cd "$PP" && bash build.sh examples ) 2>&1 | sed 's/^/  /'

# 3. The no-global-mutable-state guard: a hard precondition, not a test case.
#    ax forbids global mutable state and pocketparse inherits the rule, so a
#    static mutable variable creeping back into src/*.c fails the metric outright
#    (it is exactly the class of bug W64 fixed). Skipped only when ast-grep is
#    absent, so a host without it still gets the behavioural metric.
if command -v ast-grep >/dev/null 2>&1; then
  if ! bash "$REPO/ci/pocketparse-no-statics.sh"; then
    echo "== metric: RED (static mutable state in src/*.c) =="
    echo "PROGRESS=0/0"
    exit 1
  fi
else
  echo "== no-statics guard skipped (ast-grep not installed) =="
fi

# 4. Point the suite at the freshly built CLI and run it.
export PP="$PP/dist/$PLAT/pocketparse"
SUITE="$REPO/tools/pocketparse/test/suite"
XFAIL="$REPO/tools/pocketparse/test/xfail.list"

echo "== harness =="
"$HARNESS" --suite "$SUITE" --xfail "$XFAIL" --cwd "$REPO/tools/pocketparse"
rc=$?

case "$rc" in
  0) echo "== metric: GREEN ==" ;;
  1) echo "== metric: RED (regression) ==" ;;
  *) echo "== metric: pending ==" ;;
esac
exit "$rc"
