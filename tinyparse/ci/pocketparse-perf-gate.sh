#!/usr/bin/env bash
# ci/pocketparse-perf-gate.sh -- standing performance regression guard for
# pocketparse.
#
# Two gates. Correctness is the hard floor: ci/pocketparse-metric.sh must be
# GREEN (no optimization may break a corpus case). Speed is a ratchet: the
# pocketparse parse median must not exceed the ceiling recorded in
# tinyparse/test/perf-baseline.txt by more than a small noise margin.
# The ceiling only moves down, never up; a regression that raises the median
# above it fails here.
#
# Prints PERF=<ms> ceiling=<ms> ratio_vs_treesitter=<x>. Exit 0 only when
# correctness is GREEN and the median is within the ceiling.
set -uo pipefail
REPO="$(cd "$(dirname "$0")/.." && pwd)"
cd "$REPO" || exit 1
BASELINE="$REPO/test/perf-baseline.txt"
MARGIN="0.05"

echo "== perf gate: correctness =="
cm="$(bash ci/pocketparse-metric.sh 2>&1 | tail -1)"
echo "$cm"
case "$cm" in *GREEN*) ;; *) echo "RESULT: correctness not GREEN -- abort"; exit 1;; esac

echo "== perf gate: benchmark =="
bench="$(BENCH=1 bash ci/pocketparse-bench.sh 2>&1)"
pp="$(printf '%s\n' "$bench" | awk -F'median_seconds=' '/raw: BENCH parser=pocketparse /{split($2,a," "); print a[1]; exit}')"
ts="$(printf '%s\n' "$bench" | awk -F'median_seconds=' '/raw: BENCH parser=tree-sitter /{split($2,a," "); print a[1]; exit}')"
ceiling="$(grep -vE '^#|^[[:space:]]*$' "$BASELINE" | head -1 | tr -d '[:space:]')"
: "${pp:=0}"; : "${ts:=0}"; : "${ceiling:=0}"

awk -v pp="$pp" -v ts="$ts" -v ceil="$ceiling" -v margin="$MARGIN" 'BEGIN{
  ratio = (ts>0)? pp/ts : 0;
  limit = ceil*(1+margin);
  printf "PERF=%.1fms ceiling=%.1fms ratio_vs_treesitter=%.1fx\n", pp*1000, ceil*1000, ratio;
  if (pp>0 && pp<=limit) { print "RESULT: perf OK (within ceiling)"; exit 0 }
  printf "RESULT: perf REGRESSION (%.1fms > ceiling+%.0f%%)\n", pp*1000, margin*100; exit 1
}'
