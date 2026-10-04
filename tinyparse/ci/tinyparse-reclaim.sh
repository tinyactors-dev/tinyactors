#!/usr/bin/env bash
# ci/tinyparse-reclaim.sh -- W-explore-4 gate: prove tinyparse_parser_free
# reclaims. Parse the SAME generated file many times in ONE engine, freeing each
# parser, and assert peak RSS stays bounded. Before the per-parser-child-arena fix
# the engine arena never reclaimed, so N parses cost ~N trees (the React benchmark
# had to re-open the engine per file to avoid OOM); with the fix N parses cost
# ~one tree, so a whole codebase parses in one engine session.
#
# Self-contained: generates its own input and uses the self-contained
# std.javascript grammar (no module resolver needed), so it does not depend on the
# gitignored React reference. Run it with: mise run gate reclaim
set -uo pipefail
REPO="$(cd "$(dirname "$0")/.." && pwd)"
cd "$REPO" || exit 1
: "${TP:?run the gates through mise run gate, which builds what they need and sets TP, TP_LIB, TP_CC and TP_NM}"
CLI="$TP"; LIB="$TP_LIB"; CC="$TP_CC"
ITERS="${ITERS:-300}"; CEILING_KB="${CEILING_KB:-1048576}"   # 1 GiB; reclaim keeps it far under
log(){ printf '[reclaim] %s\n' "$*" >&2; }

[ -f "$LIB" ] && [ -x "$CLI" ] || { log "FATAL: the CLI ($CLI) or the library ($LIB) is not built"; exit 1; }

SCRATCH="$(mktemp -d)"; trap 'rm -rf "$SCRATCH"' EXIT
TPG="$SCRATCH/std.javascript.tpg"
log "packing std.javascript ..."
"$CLI" pack "$REPO/grammars/std/javascript/grammar.scm" --out "$TPG" || { log "FATAL: pack failed"; exit 1; }

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
"$CC" -std=c11 -O2 -Wall -I "$REPO/include" \
    "$REPO/bench/reclaim_test.c" "$LIB" \
    -o "$SCRATCH/reclaim_test" 2>/dev/null || { log "FATAL: harness did not compile"; exit 1; }

log "running ($ITERS iterations, ceiling ${CEILING_KB} kb) ..."
OUT="$("$SCRATCH/reclaim_test" "$TPG" "$INPUT" "$ITERS" "$CEILING_KB")"; RC=$?
echo "$OUT"
# Per-iteration ceiling check, for context: bounded means peak ~ a few parses, not N.
PEAK=$(sed -n 's/.*peak_rss_kb=\([0-9]*\).*/\1/p' <<<"$OUT")
if [ "$RC" -eq 0 ]; then
  log "GREEN: $ITERS parses in one engine peaked at ${PEAK} kb (~$((${PEAK:-0}/1024)) MiB); arena reclaims."
else
  log "RED: peak ${PEAK} kb exceeded ceiling -- parser_free is not reclaiming."
fi
exit $RC
