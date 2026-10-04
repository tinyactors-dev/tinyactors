#!/usr/bin/env bash
# ci/tinyparse-metric.sh — the finish line for tinyparse, as a program.
#
# It builds what it can, then runs the DATA-driven test harness over the seeded
# suite and the xfail ledger, and prints PROGRESS=k/n. It exits 0 ONLY when
# every test passes AND test/xfail.list is empty.
#
# Fast by default (host build + harness). RUN_GATE=1 also cross-builds the full
# dist matrix and forces the examples to build — the thorough proof.
#
# See docs/build-plan.md.
set -uo pipefail

REPO="$(cd "$(dirname "$0")/.." && pwd)"   # the tinyparse project root
TP="$REPO"
cd "$REPO" || exit 1

platform() {
  case "$(uname -s)/$(uname -m)" in
    Linux/x86_64) echo linux-x86_64 ;;
    Darwin/arm64|Darwin/aarch64) echo macos-arm64 ;;
    *) echo linux-x86_64 ;;
  esac
}
PLAT="$(platform)"

echo "== tinyparse metric =="

# 1. Build the harness (host dev tool; libc is fine here).
HARNESS="$TP/build/harness"
mkdir -p "$TP/build"
if ! cc -std=c11 -O2 -Wall -Wextra -o "$HARNESS" "$TP/test/harness.c" 2>"$TP/build/harness.log"; then
  echo "FATAL: test harness did not compile:"
  cat "$TP/build/harness.log"
  echo "PROGRESS=0/0"
  exit 1
fi

# 2. Build tinyparse itself (best-effort; missing pieces are reported as red).
#    Always cross-build the full dist matrix + examples so the result is
#    self-contained and truthful on a fresh container. RUN_GATE is reserved for
#    any extra-expensive behavioural proof a workstream wires in later.
( cd "$TP" && bash build.sh dist ) 2>&1 | sed 's/^/  /'
( cd "$TP" && bash build.sh examples ) 2>&1 | sed 's/^/  /'

# 3. The no-global-mutable-state guard: a hard precondition, not a test case.
#    tinyparse forbids global mutable state (every engine is independent and
#    reentrant), so a static mutable variable creeping back into src/*.c fails the metric outright
#    (it is exactly the class of bug W64 fixed). Skipped only when ast-grep is
#    absent, so a host without it still gets the behavioural metric.
if command -v ast-grep >/dev/null 2>&1; then
  if ! bash "$REPO/ci/tinyparse-no-statics.sh"; then
    echo "== metric: RED (static mutable state in src/*.c) =="
    echo "PROGRESS=0/0"
    exit 1
  fi
else
  echo "== no-statics guard skipped (ast-grep not installed) =="
fi

# 4. Point the suite at the freshly built CLI and run it.
export TP="$TP/dist/$PLAT/tinyparse"
SUITE="$REPO/test/suite"
XFAIL="$REPO/test/xfail.list"

echo "== harness =="
"$HARNESS" --suite "$SUITE" --xfail "$XFAIL" --cwd "$REPO"
rc=$?

case "$rc" in
  0) echo "== metric: GREEN ==" ;;
  1) echo "== metric: RED (regression) ==" ;;
  *) echo "== metric: pending ==" ;;
esac
exit "$rc"
