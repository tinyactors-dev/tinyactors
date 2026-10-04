#!/usr/bin/env bash
# ci/pocketparse-react-bench.sh -- Project B's real-workload benchmark: parse the
# ReactJS codebase with std.tsx and collect every function definition, compared
# against tree-sitter's TSX parser doing the identical task.
#
# Where ci/pocketparse-bench.sh measures parse-to-discard over the small
# tree-sitter JS test corpus, this measures a realistic pass over a real, large
# codebase -- the kind of work a linter, doc generator, or codemod does: parse
# every source file, then walk the tree collecting all function definitions.
#
# GATED behind BENCH=1 so the normal metric never pays its cost or its network
# dependency. Run from the repo root:
#
#     BENCH=1 bash ci/pocketparse-react-bench.sh
#
# End to end, re-runnably:
#   1. build the pocketparse dist (libpocketparse.a + the CLI);
#   2. build the tree-sitter runtime archive and the vendored tree-sitter-
#      typescript TSX parser.c + scanner.c;
#   3. assemble the corpus: every React packages/*/src/*.js file that std.tsx
#      parses cleanly (Flow-typed files std.tsx cannot accept are skipped so both
#      engines parse the exact same set). The clean manifest is cached; pass
#      REGEN=1 to rebuild it.
#   4. build two in-process harnesses and run each in "parse" and "collect" modes
#      for a stable median;
#   5. print a comparison table (parse time, collect time, function counts).
#
# Fairness: both engines parse the same files, build a tree, and run the same
# function-collection walk over precomputed kind ids. The one-time grammar/library
# load is excluded from the timed loop; a median over many iterations is reported.
set -uo pipefail

if [ "${BENCH:-0}" != "1" ]; then
  echo "ci/pocketparse-react-bench.sh is gated behind BENCH=1 (skipping)."
  echo "Run it with:  BENCH=1 bash ci/pocketparse-react-bench.sh"
  exit 0
fi

REPO="$(cd "$(dirname "$0")/.." && pwd)"
PP="$REPO"
BENCHDIR="$PP/bench"
cd "$REPO" || exit 1

ITERS="${ITERS:-50}"

platform() {
  case "$(uname -s)/$(uname -m)" in
    Linux/x86_64) echo linux-x86_64 ;;
    Darwin/arm64|Darwin/aarch64) echo macos-arm64 ;;
    *) echo linux-x86_64 ;;
  esac
}
PLAT="$(platform)"
DIST="$PP/dist/$PLAT"
CLI="$DIST/pocketparse"

log() { printf '[react-bench] %s\n' "$*" >&2; }

# --- 1. pocketparse dist -----------------------------------------------------
log "building pocketparse dist ..."
( cd "$PP" && bash build.sh all ) >/dev/null 2>&1
if [ ! -f "$DIST/libpocketparse.a" ] || [ ! -x "$CLI" ]; then
  log "FATAL: pocketparse dist did not build ($DIST)"
  exit 1
fi

# --- 2. tree-sitter runtime + TSX grammar ------------------------------------
TSTS_REF="$PP/reference/tree-sitter-typescript"
if [ ! -d "$TSTS_REF/tsx/src" ]; then
  log "FATAL: tree-sitter-typescript reference missing at $TSTS_REF"
  log "       re-fetch per tinyparse/reference/PINS.txt"
  exit 1
fi

TS_RUNTIME="$BENCHDIR/tree-sitter"
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

CC="${CC:-cc}"
TS_INC="$TS_RUNTIME/lib/include"
TS_SRC="$TS_RUNTIME/lib/src"
TSX_SRC="$TSTS_REF/tsx/src"

log "building tree-sitter runtime + TSX grammar ..."
if [ ! -f "$BENCHDIR/libtreesitter.a" ]; then
  $CC -std=c11 -O2 -D_DEFAULT_SOURCE -D_GNU_SOURCE \
      -c -I "$TS_INC" -I "$TS_SRC" "$TS_SRC/lib.c" -o "$BENCHDIR/ts_lib.o" || {
    log "FATAL: tree-sitter runtime did not compile"; exit 1; }
  ar rcs "$BENCHDIR/libtreesitter.a" "$BENCHDIR/ts_lib.o"
fi
# The TSX scanner includes ../../common/scanner.h relative to its own directory.
$CC -std=c11 -O2 -c -I "$TS_INC" -I "$TSX_SRC" \
    "$TSX_SRC/parser.c" -o "$BENCHDIR/tsx_parser.o" || {
  log "FATAL: tree-sitter-typescript TSX parser.c did not compile"; exit 1; }
$CC -std=c11 -O2 -c -I "$TS_INC" -I "$TSX_SRC" \
    "$TSX_SRC/scanner.c" -o "$BENCHDIR/tsx_scanner.o" || {
  log "FATAL: tree-sitter-typescript TSX scanner.c did not compile"; exit 1; }

# --- 3. corpus: React files std.tsx parses cleanly ---------------------------
REACT="$PP/reference/react"
if [ ! -d "$REACT/packages" ]; then
  log "FATAL: ReactJS reference missing at $REACT"
  log "       re-fetch per tinyparse/reference/PINS.txt"
  exit 1
fi
CORPUS_OUT="$BENCHDIR/react-corpus"
MANIFEST="$CORPUS_OUT/MANIFEST.txt"
mkdir -p "$CORPUS_OUT"
if [ "${REGEN:-0}" = "1" ] || [ ! -s "$MANIFEST" ]; then
  log "assembling clean-parsing React corpus (this scans every source file) ..."
  : > "$MANIFEST"
  total=0; clean=0
  while IFS= read -r f; do
    total=$((total+1))
    if "$CLI" run std.tsx --input "$f" >/dev/null 2>&1; then
      echo "$f" >> "$MANIFEST"
      clean=$((clean+1))
    fi
  done < <(find "$REACT/packages" -path '*/src/*' -name '*.js' | sort)
  log "corpus: $clean of $total React source files parse cleanly under std.tsx"
else
  log "corpus: reusing cached manifest ($(wc -l < "$MANIFEST") files); REGEN=1 to rebuild"
fi
CASES="$(wc -l < "$MANIFEST" | tr -d ' ')"
if [ -z "$CASES" ] || [ "$CASES" -eq 0 ]; then
  log "FATAL: no clean-parsing React files found"
  exit 1
fi
CORPUS_BYTES="$(tr '\n' '\0' < "$MANIFEST" | xargs -0 stat -c%s 2>/dev/null | awk '{s+=$1} END{print s}')"

# --- 3b. pack std.tsx (resolves extends + params) ----------------------------
# std.tsx extends std.typescript -> std.javascript; packing it through the CLI
# performs the full module resolution and applies the package params, yielding a
# self-contained .ppg the harness loads (parses identically to `run std.tsx').
GRAMMAR_PPG="$CORPUS_OUT/std.tsx.ppg"
log "packing std.tsx -> $GRAMMAR_PPG ..."
"$CLI" pack std.tsx --out "$GRAMMAR_PPG" || {
  log "FATAL: could not pack std.tsx"; exit 1; }

# --- 4. build harnesses ------------------------------------------------------
log "building React benchmark harnesses ..."
$CC -std=c11 -O2 -Wall -Wextra -I "$TS_INC" \
    "$BENCHDIR/react_treesitter.c" "$BENCHDIR/tsx_parser.o" "$BENCHDIR/tsx_scanner.o" \
    "$BENCHDIR/libtreesitter.a" -o "$BENCHDIR/react_treesitter" || {
  log "FATAL: tree-sitter React harness did not compile"; exit 1; }
$CC -std=c11 -O2 -Wall -Wextra -I "$PP/include" \
    "$BENCHDIR/react_pocketparse.c" "$DIST/libpocketparse.a" \
    -o "$BENCHDIR/react_pocketparse" || {
  log "FATAL: pocketparse React harness did not compile"; exit 1; }

# --- 5. run + capture --------------------------------------------------------
log "running tree-sitter parse ($ITERS iterations) ..."
TS_PARSE="$("$BENCHDIR/react_treesitter" "$MANIFEST" "$ITERS" parse)" || {
  log "FATAL: tree-sitter parse failed"; exit 1; }
log "running pocketparse parse ($ITERS iterations) ..."
PP_PARSE="$("$BENCHDIR/react_pocketparse" "$MANIFEST" "$ITERS" "$GRAMMAR_PPG" parse)" || {
  log "FATAL: pocketparse parse failed"; exit 1; }
log "running tree-sitter collect ($ITERS iterations) ..."
TS_COLL="$("$BENCHDIR/react_treesitter" "$MANIFEST" "$ITERS" collect)" || {
  log "FATAL: tree-sitter collect failed"; exit 1; }
log "running pocketparse collect ($ITERS iterations) ..."
PP_COLL="$("$BENCHDIR/react_pocketparse" "$MANIFEST" "$ITERS" "$GRAMMAR_PPG" collect)" || {
  log "FATAL: pocketparse collect failed"; exit 1; }

# Hardware capture.
CPU_MODEL="$(grep -m1 'model name' /proc/cpuinfo 2>/dev/null | sed 's/.*: //')"
[ -z "$CPU_MODEL" ] && CPU_MODEL="$(uname -p 2>/dev/null)"
CPU_CORES="$(grep -c '^processor' /proc/cpuinfo 2>/dev/null)"
MEM_KB="$(grep -m1 MemTotal /proc/meminfo 2>/dev/null | awk '{print $2}')"
MEM_GB="?"; [ -n "$MEM_KB" ] && MEM_GB="$(awk "BEGIN{printf \"%.1f\", $MEM_KB/1048576}")"
HARDWARE="$CPU_MODEL ($CPU_CORES cores), ${MEM_GB} GiB RAM, $(uname -srm)"

field() { sed -n "s/.*\b$2=\([^ ]*\).*/\1/p" <<<"$1"; }
fmt_ms() { awk "BEGIN{printf \"%.2f ms\", $1*1000}"; }

ts_pt="$(field "$TS_PARSE" median_seconds)"; ts_pmbps="$(field "$TS_PARSE" throughput_mbps)"
pp_pt="$(field "$PP_PARSE" median_seconds)"; pp_pmbps="$(field "$PP_PARSE" throughput_mbps)"
ts_ct="$(field "$TS_COLL" median_seconds)"; ts_cfn="$(field "$TS_COLL" functions)"
pp_ct="$(field "$PP_COLL" median_seconds)"; pp_cfn="$(field "$PP_COLL" functions)"

ratio_parse="$(awk "BEGIN{printf \"%.2f\", $pp_pt/$ts_pt}")"
# Total task = parse + collect, both per engine.
pp_tot="$(awk "BEGIN{print $pp_pt+$pp_ct}")"
ts_tot="$(awk "BEGIN{print $ts_pt+$ts_ct}")"
ratio_total="$(awk "BEGIN{printf \"%.2f\", $pp_tot/$ts_tot}")"

echo
echo "============== pocketparse vs tree-sitter -- ReactJS (std.tsx) =============="
echo "Hardware:   $HARDWARE"
echo "Corpus:     $CASES React source files, $CORPUS_BYTES bytes (clean-parsing under std.tsx)"
echo "Iterations: $ITERS (median reported)"
echo "TS runtime: tree-sitter @ $TS_PIN; grammar tree-sitter-typescript TSX"
echo "Task:       parse every file, then collect every function definition."
echo
echo "-- Parse (parse every file to a tree, discard) ------------------------------"
printf '%-22s | %-16s | %-14s\n' "parser" "median parse" "throughput"
printf '%-22s-+-%-16s-+-%-14s\n' "----------------------" "----------------" "--------------"
printf '%-22s | %-16s | %-14s\n' "pocketparse" "$(fmt_ms "$pp_pt")" "${pp_pmbps} MB/s"
printf '%-22s | %-16s | %-14s\n' "tree-sitter" "$(fmt_ms "$ts_pt")" "${ts_pmbps} MB/s"
echo "tree-sitter is ${ratio_parse}x faster on parse, measured."
echo
echo "-- Collect (walk the parsed trees, count function definitions) --------------"
printf '%-22s | %-16s | %-14s\n' "parser" "median collect" "functions"
printf '%-22s-+-%-16s-+-%-14s\n' "----------------------" "----------------" "--------------"
printf '%-22s | %-16s | %-14s\n' "pocketparse" "$(fmt_ms "$pp_ct")" "${pp_cfn}"
printf '%-22s | %-16s | %-14s\n' "tree-sitter" "$(fmt_ms "$ts_ct")" "${ts_cfn}"
echo "(function counts are a cross-check; the grammars categorize a few"
echo " constructs at different granularity, so they are close, not identical.)"
echo
echo "-- Total task (parse + collect) ---------------------------------------------"
printf '%-22s | %-16s\n' "pocketparse" "$(fmt_ms "$pp_tot")"
printf '%-22s | %-16s\n' "tree-sitter" "$(fmt_ms "$ts_tot")"
echo "tree-sitter is ${ratio_total}x faster end to end, measured."
echo "============================================================================"
echo
echo "raw: $PP_PARSE"
echo "raw: $TS_PARSE"
echo "raw: $PP_COLL"
echo "raw: $TS_COLL"
