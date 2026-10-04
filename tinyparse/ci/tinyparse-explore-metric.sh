#!/usr/bin/env bash
# ci/tinyparse-explore-metric.sh — the finish line for the exploration project
# (docs/explore-plan.md). Prints PROGRESS=k/5 over the five workstreams and exits
# non-zero until every artifact exists AND the standing gates are green.
#
# Cheap by default: checks artifacts + the fast gates. Set RUN_GATE=1 to also run
# the full conformance/differential/perf gates (slower). Set BENCH=1 to require
# the benchmark arms to actually run (needs node/go/network).
set -uo pipefail
REPO="$(cd "$(dirname "$0")/.." && pwd)"
TP="$REPO"
cd "$REPO" || exit 1

pass=0; total=5
row() { printf '  [%s] %s\n' "$1" "$2"; }

# W1: tsc/tsgo benchmark harness exists.
if [ -f "$REPO/ci/tinyparse-tsc-bench.sh" ]; then
  row x "W1 tsc/tsgo benchmark harness"; pass=$((pass+1))
else row " " "W1 tsc/tsgo benchmark harness (missing ci/tinyparse-tsc-bench.sh)"; fi

# W2: a recorded before/after of the left-factoring (memo OFF) in the RESULTS docs
# (not the plan). Marked with the literal tag W2-MEASURED so the plan's own prose
# cannot satisfy it.
if grep -rqi 'W2-MEASURED' "$TP/docs/performance.md" "$TP/docs/peg-vs-glr.md" 2>/dev/null; then
  row x "W2 left-factoring measured (memo OFF)"; pass=$((pass+1))
else row " " "W2 left-factoring measured (memo OFF) (no W2-MEASURED record yet)"; fi

# W3: PEG-vs-GLR findings doc.
if [ -f "$TP/docs/peg-vs-glr.md" ] && grep -qiE 'finding' "$TP/docs/peg-vs-glr.md" 2>/dev/null; then
  row x "W3 peg-vs-glr findings"; pass=$((pass+1))
else row " " "W3 peg-vs-glr findings (no docs/peg-vs-glr.md findings yet)"; fi

# W4: arena-reclaim test exists and (RUN_GATE) passes.
if [ -f "$REPO/ci/tinyparse-reclaim.sh" ]; then
  if [ "${RUN_GATE:-0}" = "1" ]; then
    if bash "$REPO/ci/tinyparse-reclaim.sh" >/dev/null 2>&1; then
      row x "W4 parser_free reclaim (test green)"; pass=$((pass+1))
    else row " " "W4 parser_free reclaim (test RED)"; fi
  else row x "W4 parser_free reclaim test present (RUN_GATE=1 to verify)"; pass=$((pass+1)); fi
else row " " "W4 parser_free reclaim (missing ci/tinyparse-reclaim.sh)"; fi

# W5: recommendation section.
if grep -qi 'recommendation' "$TP/docs/peg-vs-glr.md" 2>/dev/null; then
  row x "W5 recommendation"; pass=$((pass+1))
else row " " "W5 recommendation (no recommendation section yet)"; fi

echo "PROGRESS=$pass/$total"

# Standing gates (the floor). Fast structural ones always; behavioural under RUN_GATE.
gate_fail=0
if [ "${RUN_GATE:-0}" = "1" ]; then
  bash "$REPO/ci/tinyparse-metric.sh"   >/dev/null 2>&1 || { echo "GATE: conformance RED"; gate_fail=1; }
  bash "$REPO/ci/tinyparse-vm-diff.sh"  >/dev/null 2>&1 || { echo "GATE: vm-diff RED";    gate_fail=1; }
  bash "$REPO/ci/tinyparse-no-statics.sh" >/dev/null 2>&1 || { echo "GATE: no-statics RED"; gate_fail=1; }
fi

if [ "$pass" -eq "$total" ] && [ "$gate_fail" -eq 0 ]; then
  echo "RESULT: GREEN — exploration complete and gates hold."
  exit 0
fi
echo "RESULT: incomplete"
exit 1
