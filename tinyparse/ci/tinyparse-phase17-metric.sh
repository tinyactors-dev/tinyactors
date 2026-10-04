#!/usr/bin/env bash
# ci/tinyparse-phase17-metric.sh — the finish line for Phase 17
# (docs/phase17-plan.md): the generated C parser is idiomatic hand-written native C,
# really fast (past tree-sitter, toward tsc), and keeps EVERY tinyparse-special
# capability. PROGRESS=k/4 over the workstreams; GREEN only when all four land, the
# standing + new preservation gates hold, AND the headline goal (C-compiled >
# tree-sitter, tagged W17-FASTER) is recorded. Cheap by default; RUN_GATE=1 runs gates.
set -uo pipefail
REPO="$(cd "$(dirname "$0")/.." && pwd)"
TP="$REPO"
cd "$REPO" || exit 1
pass=0; total=4
row(){ printf '  [%s] %s\n' "$1" "$2"; }

# A workstream counts as recorded only when its tag heads a results SECTION
# (a Markdown heading like `## W17-2 — …`), not when prose forward-references it.
recorded(){ grep -rqiE "^#+[[:space:]].*$1" "$TP/docs/peg-vs-glr.md" "$TP/docs/performance.md" 2>/dev/null; }

if recorded 'W17-1'; then row x "W1 harden the preservation net (recorded)"; pass=$((pass+1));
else row " " "W1 harden the preservation net (no W17-1 record yet)"; fi

if recorded 'W17-2'; then row x "W2 idiomatic hand-written RD emitter (recorded)"; pass=$((pass+1));
else row " " "W2 hand-written RD emitter (no W17-2 record yet)"; fi

if recorded 'W17-3'; then row x "W3 prefix-by-name + nesting & import through C (recorded)"; pass=$((pass+1));
else row " " "W3 prefix/nesting/import (no W17-3 record yet)"; fi

if recorded 'W17-4'; then row x "W4 actor coordination + buffer-friendly generated parser (recorded)"; pass=$((pass+1));
else row " " "W4 actor coordination / buffer-friendly (no W17-4 record yet)"; fi

echo "PROGRESS=$pass/$total"

goal_ok=0
if grep -rqiE "^#+[[:space:]].*W17-FASTER" "$TP/docs/performance.md" 2>/dev/null; then
  row x "GOAL C-compiled > tree-sitter, closing on tsc (W17-FASTER recorded)"; goal_ok=1
else row " " "GOAL C-compiled > tree-sitter (not yet recorded)"; fi

gate_fail=0
if [ "${RUN_GATE:-0}" = "1" ]; then
  for g in metric cc-diff vm-diff no-statics vm-nocrash parser-nocrash multiparser exports; do
    bash "$REPO/ci/tinyparse-$g.sh" >/dev/null 2>&1 || { echo "GATE: $g RED"; gate_fail=1; }
  done
  # new preservation gates (present once their workstream lands): the W17-1 net,
  # W17-3 native nesting, and the W17-4 buffer-friendly boundary.
  for g in diag-diff import-c nesting bufferfriendly; do
    [ -f "$REPO/ci/tinyparse-$g.sh" ] && { bash "$REPO/ci/tinyparse-$g.sh" >/dev/null 2>&1 || { echo "GATE: $g RED"; gate_fail=1; }; }
  done
fi

if [ "$pass" -eq "$total" ] && [ "$goal_ok" -eq 1 ] && [ "$gate_fail" -eq 0 ]; then
  echo "RESULT: GREEN — Phase 17 complete (hand-written C parser, fast, keeps everything)."; exit 0
fi
echo "RESULT: incomplete"; exit 1
