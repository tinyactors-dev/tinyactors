#!/usr/bin/env bash
# ci/pocketparse-phase14-metric.sh — the finish line for Phase 14
# (docs/phase14-plan.md). PROGRESS=k/5 over the workstreams; non-zero until 5/5 and
# the standing gates hold. Cheap by default; RUN_GATE=1 also runs the behavioural
# gates (conformance, differentials).
set -uo pipefail
REPO="${CLAUDE_PROJECT_DIR:-$(cd "$(dirname "$0")/.." && pwd)}"
PP="$REPO/tools/pocketparse"
cd "$REPO" || exit 1
pass=0; total=6
row(){ printf '  [%s] %s\n' "$1" "$2"; }

# W1: left-factor skill + a recorded memo-OFF before/after.
if [ -f "$PP/.claude/skills/left-factor/SKILL.md" ] && \
   grep -rqi 'W14-1-MEASURED' "$PP/docs/" 2>/dev/null; then
  row x "W1 left-factor skill + measured"; pass=$((pass+1))
else row " " "W1 left-factor skill + measured (need skill + W14-1-MEASURED record)"; fi

# W2: no production path calls pp_execute (injection routed to the VM). The only
# permitted pp_execute reference in src/ is its own definition + the oracle wiring;
# assert inject.c no longer calls it.
if [ -f "$PP/src/inject.c" ] && ! grep -q 'pp_execute(' "$PP/src/inject.c" 2>/dev/null; then
  row x "W2 VM is the sole production engine (inject off pp_execute)"; pass=$((pass+1))
else row " " "W2 VM is the sole production engine (inject still calls pp_execute)"; fi

# W3: RD-style C codegen + the cc-diff gate exist.
if [ -f "$REPO/ci/pocketparse-cc-diff.sh" ] && grep -rqi 'W14-3' "$PP/docs/" 2>/dev/null; then
  if [ "${RUN_GATE:-0}" = "1" ]; then
    if bash "$REPO/ci/pocketparse-cc-diff.sh" >/dev/null 2>&1; then row x "W3 RD-style C codegen (cc-diff green)"; pass=$((pass+1));
    else row " " "W3 RD-style C codegen (cc-diff RED)"; fi
  else row x "W3 RD-style C codegen present (RUN_GATE=1 to verify)"; pass=$((pass+1)); fi
else row " " "W3 RD-style C codegen (need ci/pocketparse-cc-diff.sh + W14-3 record)"; fi

# W4: five-way benchmark naming all five parsers.
if grep -rqi 'C-compiled' "$PP/docs/performance.md" 2>/dev/null && \
   [ -f "$REPO/ci/pocketparse-bench5.sh" -o -f "$REPO/ci/pocketparse-react-bench.sh" ]; then
  if grep -rqi 'W14-4' "$PP/docs/" 2>/dev/null; then row x "W4 five-way benchmark"; pass=$((pass+1));
  else row " " "W4 five-way benchmark (no W14-4 numbers recorded)"; fi
else row " " "W4 five-way benchmark (no C-compiled row recorded)"; fi

# W5: pp_execute eliminated from src/.
if ! grep -rq 'pp_execute\b' "$PP/src/" 2>/dev/null; then
  row x "W5 IR interpreter eliminated"; pass=$((pass+1))
else row " " "W5 IR interpreter eliminated (pp_execute still in src/)"; fi

# W6: the bytecode VM is iterative (proven by a deep-input no-crash gate) + TCO record.
if [ -f "$REPO/ci/pocketparse-vm-nocrash.sh" ] && grep -rqi 'W14-6' "$PP/docs/" 2>/dev/null; then
  if [ "${RUN_GATE:-0}" = "1" ]; then
    if bash "$REPO/ci/pocketparse-vm-nocrash.sh" >/dev/null 2>&1; then row x "W6 VM iterative (no-crash gate green)"; pass=$((pass+1));
    else row " " "W6 VM iterative (vm-nocrash RED)"; fi
  else row x "W6 VM iterative present (RUN_GATE=1 to verify)"; pass=$((pass+1)); fi
else row " " "W6 VM iterative (need ci/pocketparse-vm-nocrash.sh + W14-6 record)"; fi

echo "PROGRESS=$pass/$total"
gate_fail=0
if [ "${RUN_GATE:-0}" = "1" ]; then
  bash "$REPO/ci/pocketparse-metric.sh"     >/dev/null 2>&1 || { echo "GATE: conformance RED"; gate_fail=1; }
  bash "$REPO/ci/pocketparse-no-statics.sh" >/dev/null 2>&1 || { echo "GATE: no-statics RED"; gate_fail=1; }
fi
if [ "$pass" -eq "$total" ] && [ "$gate_fail" -eq 0 ]; then echo "RESULT: GREEN — Phase 14 complete."; exit 0; fi
echo "RESULT: incomplete"; exit 1
