#!/usr/bin/env bash
# bench/javascript.sh — Phase 9 (W42): a fair, in-process benchmark of
# tinyparse's std.javascript grammar against tree-sitter's JavaScript parser.
#
# It is GATED behind BENCH=1 so the normal metric never pays its cost or its
# network dependency. Run it from the repo root:
#
#     mise run bench:javascript
#
# What it does, end to end and re-runnably:
#   1. builds the tinyparse dist (libtinyparse.a + the CLI);
#   2. clones the tree-sitter RUNTIME (pinned by commit) and builds its
#      amalgamated lib into a static archive, plus the vendored
#      tree-sitter-javascript parser.c + scanner.c;
#   3. extracts the INPUT halves of every case in the vendored tree-sitter
#      JavaScript corpus into per-document .js files + a manifest;
#   4. builds two tiny in-process harnesses (NOT CLI subprocesses — those would
#      measure process startup, not parsing) and runs each over the corpus for a
#      stable median;
#   5. captures the hardware (CPU model + cores, MemTotal, uname) and prints a
#      comparison table of median parse time, throughput (MB/s) and peak RSS.
#
# Every parser is measured the same way: warm-up passes, the same inputs, the
# one-time grammar/library load excluded from the timed loop, and a median over
# many iterations. The numbers are reported AS MEASURED.
set -uo pipefail

if [ "${BENCH:-0}" != "1" ]; then
  echo "bench/javascript.sh is gated behind BENCH=1 (skipping)."
  echo "Run it with:  mise run bench:javascript"
  exit 0
fi

REPO="$(cd "$(dirname "$0")/.." && pwd)"
TP="$REPO"
BENCHDIR="$TP/bench"
cd "$REPO" || exit 1

# Iteration count: enough passes for a stable median. Override with ITERS=.
ITERS="${ITERS:-200}"

LIB="${TP_LIB:?run it with mise run bench:javascript, which builds tinyparse and sets TP_CLI, TP_LIB, TP_CC and TP_AR}"

log() { printf '[tp-bench] %s\n' "$*" >&2; }

# --- 1. tinyparse dist -----------------------------------------------------
if [ ! -f "$LIB" ]; then
  log "FATAL: tinyparse is not built ($LIB)"
  exit 1
fi

# --- 2. tree-sitter runtime + grammar ---------------------------------------
TSJS_REF="$TP/reference/tree-sitter-javascript"
if [ ! -d "$TSJS_REF/src" ]; then
  log "FATAL: tree-sitter-javascript reference missing at $TSJS_REF"
  log "       re-fetch per tinyparse/reference/PINS.txt"
  exit 1
fi

TS_RUNTIME="$BENCHDIR/tree-sitter"
# Pin: record whatever HEAD we clone so the benchmark is reproducible.
TS_PIN_FILE="$BENCHDIR/tree-sitter-runtime.pin"
if [ ! -d "$TS_RUNTIME/lib/src" ]; then
  log "cloning tree-sitter runtime ..."
  rm -rf "$TS_RUNTIME"
  if ! git clone --depth 1 https://github.com/tree-sitter/tree-sitter "$TS_RUNTIME" >/dev/null 2>&1; then
    log "FATAL: could not clone the tree-sitter runtime (network required)"
    exit 1
  fi
fi
TS_PIN="$(git -C "$TS_RUNTIME" rev-parse HEAD 2>/dev/null)"
echo "$TS_PIN" > "$TS_PIN_FILE"
log "tree-sitter runtime pinned at $TS_PIN"

CC="$TP_CC"
TS_INC="$TS_RUNTIME/lib/include"
TS_SRC="$TS_RUNTIME/lib/src"

log "building tree-sitter runtime + grammar ..."
# The amalgamated runtime needs POSIX/GNU feature macros for le16toh/fdopen.
$CC -std=c11 -O2 -D_DEFAULT_SOURCE -D_GNU_SOURCE \
    -c -I "$TS_INC" -I "$TS_SRC" "$TS_SRC/lib.c" -o "$BENCHDIR/ts_lib.o" || {
  log "FATAL: tree-sitter runtime did not compile"; exit 1; }
"$TP_AR" rcs "$BENCHDIR/libtreesitter.a" "$BENCHDIR/ts_lib.o"
$CC -std=c11 -O2 -c -I "$TS_INC" -I "$TSJS_REF/src" \
    "$TSJS_REF/src/parser.c" -o "$BENCHDIR/ts_parser.o" || {
  log "FATAL: tree-sitter-javascript parser.c did not compile"; exit 1; }
$CC -std=c11 -O2 -c -I "$TS_INC" -I "$TSJS_REF/src" \
    "$TSJS_REF/src/scanner.c" -o "$BENCHDIR/ts_scanner.o" || {
  log "FATAL: tree-sitter-javascript scanner.c did not compile"; exit 1; }

# --- 3. corpus extraction ----------------------------------------------------
CORPUS_SRC="$TSJS_REF/test/corpus"
CORPUS_OUT="$BENCHDIR/corpus"
log "extracting corpus inputs ..."
rm -rf "$CORPUS_OUT"
mkdir -p "$CORPUS_OUT"
CASES="$(python3 - "$CORPUS_SRC" "$CORPUS_OUT" <<'PY'
import sys, os, re, glob
corpus, out = sys.argv[1], sys.argv[2]
idx = 0
manifest = open(os.path.join(out, "MANIFEST.txt"), "w")
concat = open(os.path.join(out, "corpus.js"), "w", encoding="utf-8")
# tree-sitter case format: ===\nTitle\n===\n<input>\n---\n<expected tree>.
# We keep only the input half of each case (see bench/javascript-coverage.sh).
for path in sorted(glob.glob(os.path.join(corpus, "*.txt"))):
    txt = open(path, encoding="utf-8").read()
    pat = re.compile(r'(?m)^={3,}[ \t]*\n(.*?)\n={3,}[ \t]*\n')
    parts = pat.split(txt)
    for i in range(1, len(parts), 2):
        body = parts[i+1]
        m = re.search(r'(?m)^-{3,}[ \t]*\n', body)
        if not m:
            continue
        inp = body[:m.start()]
        if inp.startswith("\n"):
            inp = inp[1:]
        inp = inp.rstrip("\n") + "\n"
        fn = os.path.join(out, "case_%04d.js" % idx)
        with open(fn, "w", encoding="utf-8") as f:
            f.write(inp)
        manifest.write(os.path.abspath(fn) + "\n")
        concat.write(inp); concat.write("\n")
        idx += 1
manifest.close(); concat.close()
print(idx)
PY
)"
if [ -z "$CASES" ] || [ "$CASES" -eq 0 ]; then
  log "FATAL: corpus extraction produced no cases"
  exit 1
fi
MANIFEST="$CORPUS_OUT/MANIFEST.txt"
CORPUS_BYTES="$(wc -c < "$CORPUS_OUT/corpus.js" | tr -d ' ')"
log "corpus: $CASES documents extracted"

# --- 4. build harnesses ------------------------------------------------------
log "building benchmark harnesses ..."
$CC -std=c11 -O2 -Wall -Wextra -I "$TS_INC" \
    "$BENCHDIR/bench_treesitter.c" "$BENCHDIR/ts_parser.o" "$BENCHDIR/ts_scanner.o" \
    "$BENCHDIR/libtreesitter.a" -o "$BENCHDIR/bench_treesitter" || {
  log "FATAL: tree-sitter harness did not compile"; exit 1; }
$CC -std=c11 -O2 -Wall -Wextra -I "$TP/include" \
    "$BENCHDIR/bench_tinyparse.c" "$LIB" \
    -o "$BENCHDIR/bench_tinyparse" || {
  log "FATAL: tinyparse harness did not compile"; exit 1; }

# --- 5. run + capture --------------------------------------------------------
# Every parser is measured the same way: warm-up, the same bytes, parse-to-tree
# with NO post-parse walk on either side (tinyparse builds its CST eagerly at
# finish, so neither side traverses), one-time setup excluded, median over many
# iterations. We report TWO variants:
#   idiomatic  -- each parser used as its API intends (tree-sitter reuses one
#                 parser; tinyparse creates one per document).
#   symmetric  -- tree-sitter ALSO creates+deletes a parser per document, so the
#                 parser-setup cost both sides pay is isolated and visible.
GRAMMAR="$TP/grammars/std/javascript/grammar.scm"
log "running tree-sitter harness, idiomatic ($ITERS iterations) ..."
TS_LINE="$("$BENCHDIR/bench_treesitter" "$MANIFEST" "$ITERS" idiomatic)" || {
  log "FATAL: tree-sitter harness (idiomatic) failed"; exit 1; }
log "running tree-sitter harness, per-document lifecycle ($ITERS iterations) ..."
TSL_LINE="$("$BENCHDIR/bench_treesitter" "$MANIFEST" "$ITERS" lifecycle)" || {
  log "FATAL: tree-sitter harness (lifecycle) failed"; exit 1; }
log "running tinyparse harness ($ITERS iterations) ..."
TP_OUT="$("$BENCHDIR/bench_tinyparse" "$MANIFEST" "$ITERS" "$GRAMMAR")" || {
  log "FATAL: tinyparse harness failed"; exit 1; }
TP_LINE="$(grep -m1 'parser=tinyparse ' <<<"$TP_OUT")"
TP_FOOT="$(grep -m1 'parser=tinyparse-footprint' <<<"$TP_OUT")"

# Traversal: parse each document UNTIMED to build the tree, then time ONLY a
# preorder depth-first cursor walk that touches every node. Both harnesses walk
# the same way; each reports its OWN node count and we normalize by it (ns/node)
# so the comparison is per-node work, not per-tree (the trees differ in shape).
log "running tree-sitter harness, walk ($ITERS iterations) ..."
TSW_LINE="$("$BENCHDIR/bench_treesitter" "$MANIFEST" "$ITERS" walk)" || {
  log "FATAL: tree-sitter harness (walk) failed"; exit 1; }
log "running tinyparse harness, walk ($ITERS iterations) ..."
TPW_LINE="$("$BENCHDIR/bench_tinyparse" "$MANIFEST" "$ITERS" "$GRAMMAR" walk)" || {
  log "FATAL: tinyparse harness (walk) failed"; exit 1; }

# Hardware capture.
CPU_MODEL="$(grep -m1 'model name' /proc/cpuinfo 2>/dev/null | sed 's/.*: //')"
[ -z "$CPU_MODEL" ] && CPU_MODEL="$(uname -p 2>/dev/null)"
CPU_CORES="$(grep -c '^processor' /proc/cpuinfo 2>/dev/null)"
[ -z "$CPU_CORES" ] && CPU_CORES="?"
MEM_KB="$(grep -m1 MemTotal /proc/meminfo 2>/dev/null | awk '{print $2}')"
if [ -n "$MEM_KB" ]; then
  MEM_GB="$(awk "BEGIN{printf \"%.1f\", $MEM_KB/1048576}")"
else
  MEM_GB="?"
fi
UNAME="$(uname -srm)"
HARDWARE="$CPU_MODEL ($CPU_CORES cores), ${MEM_GB} GiB RAM, $UNAME"

# Pull fields out of a BENCH line.
field() { sed -n "s/.*\b$2=\([^ ]*\).*/\1/p" <<<"$1"; }
fmt_ms() { awk "BEGIN{printf \"%.3f ms\", $1*1000}"; }
fmt_rss() { awk "BEGIN{printf \"%.1f MiB\", $1/1024}"; }

fmt_mib() { awk "BEGIN{printf \"%.1f MiB\", $1/1048576}"; }

ts_t="$(field "$TS_LINE" median_seconds)"
ts_mbps="$(field "$TS_LINE" throughput_mbps)"
ts_rss="$(field "$TS_LINE" peak_rss_kb)"
tsl_t="$(field "$TSL_LINE" median_seconds)"
tsl_mbps="$(field "$TSL_LINE" throughput_mbps)"
tp_t="$(field "$TP_LINE" median_seconds)"
tp_mbps="$(field "$TP_LINE" throughput_mbps)"
tp_rss="$(field "$TP_LINE" peak_rss_kb)"

# Fair tinyparse footprint: the bytes it actually pulled (base grant + grown
# blocks), split into the one-time grammar compile and the per-corpus-pass parse
# arena -- NOT the over-provisioned grant size and NOT process RSS.
tp_foot_compile="$(field "$TP_FOOT" compile_bytes)"
tp_foot_parse="$(field "$TP_FOOT" parse_bytes)"
tp_foot_total="$(field "$TP_FOOT" high_water_bytes)"

# Traversal fields, per parser (each walks its own tree; node counts differ).
tpw_nodes="$(field "$TPW_LINE" nodes)"
tpw_t="$(field "$TPW_LINE" median_seconds)"
tpw_nps="$(field "$TPW_LINE" nodes_per_sec)"
tpw_nsn="$(field "$TPW_LINE" ns_per_node)"
tsw_nodes="$(field "$TSW_LINE" nodes)"
tsw_t="$(field "$TSW_LINE" median_seconds)"
tsw_nps="$(field "$TSW_LINE" nodes_per_sec)"
tsw_nsn="$(field "$TSW_LINE" ns_per_node)"

# Ratios, idiomatic and symmetric.
ratio_idio="$(awk "BEGIN{printf \"%.0f\", $tp_t/$ts_t}")"
ratio_sym="$(awk "BEGIN{printf \"%.0f\", $tp_t/$tsl_t}")"

# The throughput denominator is the bytes the harnesses actually parse (the sum
# of the per-document inputs), which both report identically; use it as the
# corpus size so the size column and the MB/s column agree.
CORPUS_BYTES="$(field "$TP_LINE" corpus_bytes)"

# Coverage, for honest framing (not apples-to-apples on grammar coverage).
COV="$(bash "$REPO/bench/javascript-coverage.sh" 2>/dev/null | grep -m1 '^JS_COVERAGE=' | cut -d= -f2)"
[ -z "$COV" ] && COV="(unknown)"

echo
echo "================ tinyparse vs tree-sitter (std.javascript) ================"
echo "Hardware:   $HARDWARE"
echo "Corpus:     $CASES documents, $CORPUS_BYTES bytes total (tree-sitter JS test corpus inputs)"
echo "Iterations: $ITERS (median reported)"
echo "TS runtime: tree-sitter @ $TS_PIN"
echo "Coverage:   tinyparse parses $COV of the corpus cases cleanly"
echo "Method:     parse-to-tree-discard, NO post-parse walk on either side."
echo
echo "-- Idiomatic (each parser used as its API intends) --------------------------"
printf '%-22s | %-12s | %-16s | %-14s\n' \
  "parser" "corpus size" "median parse" "throughput"
printf '%-22s-+-%-12s-+-%-16s-+-%-14s\n' \
  "----------------------" "------------" "----------------" "--------------"
printf '%-22s | %-12s | %-16s | %-14s\n' \
  "tinyparse (per-doc)" "${CORPUS_BYTES} B" "$(fmt_ms "$tp_t")" "${tp_mbps} MB/s"
printf '%-22s | %-12s | %-16s | %-14s\n' \
  "tree-sitter (reused)" "${CORPUS_BYTES} B" "$(fmt_ms "$ts_t")" "${ts_mbps} MB/s"
echo "tree-sitter is ${ratio_idio}x faster on this corpus, measured."
echo
echo "-- Symmetric (both create+delete a parser per document) ---------------------"
printf '%-22s | %-12s | %-16s | %-14s\n' \
  "parser" "corpus size" "median parse" "throughput"
printf '%-22s-+-%-12s-+-%-16s-+-%-14s\n' \
  "----------------------" "------------" "----------------" "--------------"
printf '%-22s | %-12s | %-16s | %-14s\n' \
  "tinyparse (per-doc)" "${CORPUS_BYTES} B" "$(fmt_ms "$tp_t")" "${tp_mbps} MB/s"
printf '%-22s | %-12s | %-16s | %-14s\n' \
  "tree-sitter (per-doc)" "${CORPUS_BYTES} B" "$(fmt_ms "$tsl_t")" "${tsl_mbps} MB/s"
echo "tree-sitter is ${ratio_sym}x faster on this corpus, measured."
echo
echo "-- Footprint ----------------------------------------------------------------"
echo "tinyparse high-water (measured: base grant + grown blocks):"
echo "    grammar compile (one-time): $(fmt_mib "$tp_foot_compile")"
echo "    parse arena (one corpus pass, accretes): $(fmt_mib "$tp_foot_parse")"
echo "    total high-water: $(fmt_mib "$tp_foot_total")"
echo "tree-sitter peak RSS (grows on demand): $(fmt_rss "$ts_rss")"
echo "(tinyparse process peak RSS, incl. corpus + libc: $(fmt_rss "$tp_rss"))"
echo
echo "-- Traversal (parse untimed, time only a full preorder DFS cursor walk) -----"
echo "Each parser walks its OWN tree; node counts differ, so we normalize by node."
printf '%-22s | %-10s | %-16s | %-14s | %-10s\n' \
  "parser" "nodes" "median walk" "nodes/sec" "ns/node"
printf '%-22s-+-%-10s-+-%-16s-+-%-14s-+-%-10s\n' \
  "----------------------" "----------" "----------------" "--------------" "----------"
printf '%-22s | %-10s | %-16s | %-14s | %-10s\n' \
  "tinyparse" "${tpw_nodes}" "$(fmt_ms "$tpw_t")" "${tpw_nps}" "${tpw_nsn}"
printf '%-22s | %-10s | %-16s | %-14s | %-10s\n' \
  "tree-sitter" "${tsw_nodes}" "$(fmt_ms "$tsw_t")" "${tsw_nps}" "${tsw_nsn}"
echo "============================================================================"
echo
echo "raw: $TP_LINE"
echo "raw: $TP_FOOT"
echo "raw: $TS_LINE"
echo "raw: $TSL_LINE"
echo "raw: $TPW_LINE"
echo "raw: $TSW_LINE"
