#!/usr/bin/env bash
# ci/pocketparse-reclaim.sh -- W-explore-4 gate: prove pocketparse_parser_free
# reclaims. Parse the SAME generated file many times in ONE engine, freeing each
# parser, and assert peak RSS stays bounded. Before the per-parser-child-arena fix
# the engine arena never reclaimed, so N parses cost ~N trees (the React benchmark
# had to re-open the engine per file to avoid OOM); with the fix N parses cost
# ~one tree, so a whole codebase parses in one engine session.
#
# Self-contained: generates its own input and uses the self-contained
# std.javascript grammar (no module resolver needed), so it does not depend on the
# gitignored React reference. Run from the repo root: bash ci/pocketparse-reclaim.sh
set -uo pipefail
REPO="$(cd "$(dirname "$0")/.." && pwd)"
PP="$REPO"
cd "$REPO" || exit 1
PLAT=linux-x86_64
case "$(uname -s)/$(uname -m)" in Darwin/arm64|Darwin/aarch64) PLAT=macos-arm64;; esac
DIST="$PP/dist/$PLAT"; CLI="$DIST/pocketparse"; CC="${CC:-cc}"
ITERS="${ITERS:-300}"; CEILING_KB="${CEILING_KB:-1048576}"   # 1 GiB; reclaim keeps it far under
log(){ printf '[reclaim] %s\n' "$*" >&2; }

log "building pocketparse dist ..."
( cd "$PP" && bash build.sh all ) >/dev/null 2>&1
[ -f "$DIST/libpocketparse.a" ] && [ -f ] && [ -x "$CLI" ] || { log "FATAL: dist did not build"; exit 1; }

SCRATCH="$PP/bench/reclaim-scratch"; mkdir -p "$SCRATCH"
PPG="$SCRATCH/std.javascript.ppg"
log "packing std.javascript ..."
"$CLI" pack "$PP/grammars/std/javascript/grammar.scm" --out "$PPG" || { log "FATAL: pack failed"; exit 1; }

# Generate a non-trivial JS file that exercises the expression parser across many
# statements (the path the W2 left-factoring touched).
INPUT="$SCRATCH/input.js"
awk 'BEGIN{
  for(i=0;i<1500;i++){
    printf("const v%d = a + b * c - d / (e + f) && g || h ?? i;\n", i);
    printf("function f%d(x, y){ return x.a.b.c(y, 1, 2, [3,4,{k:5}]); }\n", i);
    printf("const g%d = (p, q) => p.map(z => z.v).filter(z => z > %d);\n", i, i);
  }
}' > "$INPUT"
BYTES=$(stat -c%s "$INPUT" 2>/dev/null || wc -c < "$INPUT")
log "generated input: $BYTES bytes; will parse it $ITERS times in ONE engine"

log "building reclaim harness ..."
$CC -std=c11 -O2 -Wall -I "$PP/include" \
    "$PP/bench/reclaim_test.c" "$DIST/libpocketparse.a" \
    -o "$SCRATCH/reclaim_test" 2>/dev/null || { log "FATAL: harness did not compile"; exit 1; }

log "running ($ITERS iterations, ceiling ${CEILING_KB} kb) ..."
OUT="$("$SCRATCH/reclaim_test" "$PPG" "$INPUT" "$ITERS" "$CEILING_KB")"; RC=$?
echo "$OUT"
# Per-iteration ceiling check, for context: bounded means peak ~ a few parses, not N.
PEAK=$(sed -n 's/.*peak_rss_kb=\([0-9]*\).*/\1/p' <<<"$OUT")
if [ "$RC" -eq 0 ]; then
  log "GREEN: $ITERS parses in one engine peaked at ${PEAK} kb (~$((${PEAK:-0}/1024)) MiB); arena reclaims."
else
  log "RED: peak ${PEAK} kb exceeded ceiling -- parser_free is not reclaiming."
fi
exit $RC
