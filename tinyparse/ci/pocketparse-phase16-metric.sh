#!/usr/bin/env bash
# ci/pocketparse-phase16-metric.sh — the finish line for Phase 16
# (docs/phase16-plan.md): the generated C parser parses like tsgo/meriyah — faster
# than tree-sitter. PROGRESS=k/4 over the workstreams; GREEN only when all four
# land, the standing gates hold, AND the headline goal (C-compiled > tree-sitter,
# tagged W16-FASTER) is recorded. Cheap by default; RUN_GATE=1 runs the gates.
set -uo pipefail
REPO="$(cd "$(dirname "$0")/.." && pwd)"
PP="$REPO"
cd "$REPO" || exit 1
pass=0; total=4
row(){ printf '  [%s] %s\n' "$1" "$2"; }

# A workstream counts as recorded only when its tag heads a results SECTION
# (a Markdown heading like `## W16-2 — …`), not when prose forward-references the
# next item. The heading anchor kills the recurring forward-reference false positive.
recorded(){ grep -rqiE "^#+[[:space:]].*$1" "$PP/docs/peg-vs-glr.md" "$PP/docs/performance.md" 2>/dev/null; }

if recorded 'W16-1'; then row x "W1 design-independent wins, both engines (recorded)"; pass=$((pass+1));
else row " " "W1 design-independent wins (no W16-1 record yet)"; fi

if recorded 'W16-2'; then row x "W2 generated C → depth-guarded native recursion (recorded)"; pass=$((pass+1));
else row " " "W2 native recursive descent (no W16-2 record yet)"; fi

if recorded 'W16-3'; then row x "W3 multi-parser example/test (recorded)"; pass=$((pass+1));
else row " " "W3 multi-parser example (no W16-3 record yet)"; fi

if recorded 'W16-4'; then row x "W4 hide ax in the generated parser (recorded)"; pass=$((pass+1));
else row " " "W4 hide ax symbols (no W16-4 record yet)"; fi

echo "PROGRESS=$pass/$total"

# The headline goal: generated C strictly faster than tree-sitter, recorded.
goal_ok=0
if grep -rqiE "^#+[[:space:]].*W16-FASTER" "$PP/docs/performance.md" 2>/dev/null; then
  row x "GOAL C-compiled > tree-sitter (W16-FASTER recorded)"; goal_ok=1
else row " " "GOAL C-compiled > tree-sitter (not yet recorded)"; fi

gate_fail=0
if [ "${RUN_GATE:-0}" = "1" ]; then
  bash "$REPO/ci/pocketparse-metric.sh"        >/dev/null 2>&1 || { echo "GATE: conformance RED"; gate_fail=1; }
  bash "$REPO/ci/pocketparse-cc-diff.sh"       >/dev/null 2>&1 || { echo "GATE: cc-diff RED"; gate_fail=1; }
  bash "$REPO/ci/pocketparse-vm-diff.sh"       >/dev/null 2>&1 || { echo "GATE: vm-diff RED"; gate_fail=1; }
  bash "$REPO/ci/pocketparse-no-statics.sh"    >/dev/null 2>&1 || { echo "GATE: no-statics RED"; gate_fail=1; }
  bash "$REPO/ci/pocketparse-vm-nocrash.sh"    >/dev/null 2>&1 || { echo "GATE: vm-nocrash RED"; gate_fail=1; }
  bash "$REPO/ci/pocketparse-parser-nocrash.sh">/dev/null 2>&1 || { echo "GATE: parser-nocrash RED"; gate_fail=1; }
fi

if [ "$pass" -eq "$total" ] && [ "$goal_ok" -eq 1 ] && [ "$gate_fail" -eq 0 ]; then
  echo "RESULT: GREEN — Phase 16 complete (generated C parses like tsgo/meriyah, beats tree-sitter)."; exit 0
fi
echo "RESULT: incomplete"; exit 1
