#!/usr/bin/env bash
# ci/pocketparse-phase15-metric.sh — the finish line for Phase 15
# (docs/phase15-plan.md): the generated C parser must beat tree-sitter. PROGRESS=k/3
# over the profile-ranked workstreams; GREEN only when all three land, the standing
# gates hold, AND the headline goal (C-compiled > tree-sitter, tagged W15-FASTER) is
# recorded. Cheap by default; RUN_GATE=1 runs the behavioural gates.
set -uo pipefail
REPO="${CLAUDE_PROJECT_DIR:-$(cd "$(dirname "$0")/.." && pwd)}"
PP="$REPO/tools/pocketparse"
cd "$REPO" || exit 1
pass=0; total=3
row(){ printf '  [%s] %s\n' "$1" "$2"; }

# A workstream counts as recorded only when its tag heads a results SECTION
# (a Markdown heading like `## W15-2 — …`), not when prose forward-references the
# next item (`the W15-2 lexer is next`). The heading anchor is what makes the
# detection unambiguous and kills the recurring forward-reference false positive.
recorded(){ grep -rqiE "^#+[[:space:]].*$1" "$PP/docs/peg-vs-glr.md" "$PP/docs/performance.md" 2>/dev/null; }

# W1: zeroing cut — recorded before/after memset record.
if recorded 'W15-1'; then row x "W1 cut the zeroing (recorded)"; pass=$((pass+1));
else row " " "W1 cut the zeroing (no W15-1 record yet)"; fi

# W2: lexer compiled into the generated parser.
if recorded 'W15-2'; then row x "W2 compile the lexer (recorded)"; pass=$((pass+1));
else row " " "W2 compile the lexer (no W15-2 record yet)"; fi

# W3: rule dispatch baked into the generated parser.
if recorded 'W15-3'; then row x "W3 bake the rule dispatch (recorded)"; pass=$((pass+1));
else row " " "W3 bake the rule dispatch (no W15-3 record yet)"; fi

echo "PROGRESS=$pass/$total"

# The headline goal: generated C strictly faster than tree-sitter, recorded.
goal_ok=0
if grep -rqi 'W15-FASTER' "$PP/docs/performance.md" 2>/dev/null; then
  row x "GOAL C-compiled > tree-sitter (W15-FASTER recorded)"; goal_ok=1
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
  echo "RESULT: GREEN — Phase 15 complete (generated C beats tree-sitter)."; exit 0
fi
echo "RESULT: incomplete"; exit 1
