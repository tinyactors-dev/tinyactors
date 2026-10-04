#!/usr/bin/env bash
# bench/parsers.sh -- ONE benchmark, SIX parsers, parse-only, over the
# same ReactJS corpus. (It was ci/tinyparse-bench5.sh, named in W14-4 when it ran
# five arms; W16-1 added meriyah as a sixth -- the owner's bar. It is
# `mise run bench:parsers` since the move into the tinyactors monorepo.) It
# always compares:
#
#   1. tsc                   -- the TypeScript compiler API parser (TS on V8, JIT)
#   2. tsgo                  -- microsoft/typescript-go's parser (native Go)
#   3. meriyah               -- the fastest JS-in-JS parser (ECMAScript + JSX, NOT
#                               TypeScript). The OWNER'S BAR: "if we can't get
#                               close to meriyah we're not in a good spot." Meriyah
#                               rejects the corpus's TS/Flow files, so its MB/s is
#                               over the JSX subset it parses (parsed/failed counts
#                               are reported); the figure is honest, not comparable
#                               byte-for-byte with the full-corpus arms.
#   4. tree-sitter (TSX)     -- GLR parse table + DFA lexer (compiled C)
#   5. tinyparse (VM)      -- the bytecode VM, the production engine
#   6. tinyparse (C)       -- the parser emitted by `tinyparse c std.tsx
#                               --parser-lib`, driven IN-PROCESS the SAME way as
#                               arm 5 (per-file fresh engine, only the parse call
#                               timed). cc-diff proves arms 5 and 6 build the same
#                               tree, so this is a like-for-like speed comparison.
#
# Corpus = tinyparse/bench/react-corpus/MANIFEST.txt (the clean-parsing
# React file set the react bench produces). Parse-only, warm, median of N, with
# the one-time setup (engine open + grammar load) off the meter -- IDENTICAL to
# how bench/react_tinyparse.c times the VM. Both tinyparse arms parse the same
# bytes through the same per-file-fresh-engine methodology.
#
# It reuses the existing benches' scratch (tsbench, typescript-go, tree-sitter
# runtime, the cached manifest) rather than re-cloning. GATED behind BENCH=1.
#
#     mise run bench:parsers
#
set -uo pipefail

if [ "${BENCH:-0}" != "1" ]; then
  echo "bench/parsers.sh is gated behind BENCH=1 (skipping)."
  echo "Run it with:  mise run bench:parsers"
  exit 0
fi

REPO="$(cd "$(dirname "$0")/.." && pwd)"
TP="$REPO"
BENCHDIR="$TP/bench"
cd "$REPO" || exit 1

# Two iteration knobs: the in-process arms (tree-sitter / tinyparse VM & C) are
# cheap to repeat; tsc/tsgo each pay node/Go startup, so they default lower.
ITERS="${ITERS:-30}"
TS_ITERS="${TS_ITERS:-15}"

LIB="${TP_LIB:?run it with mise run bench:parsers, which builds tinyparse and sets TP_CLI, TP_LIB, TP_CC and TP_AR}"
CLI="$TP_CLI"
CC="$TP_CC"

log() { printf '[bench5] %s\n' "$*" >&2; }
field() { sed -n "s/.*\b$2=\([^ ]*\).*/\1/p" <<<"$1"; }

# --- 0. corpus ---------------------------------------------------------------
MANIFEST="$BENCHDIR/react-corpus/MANIFEST.txt"
if [ ! -s "$MANIFEST" ]; then
  log "FATAL: corpus manifest missing at $MANIFEST"
  log "       generate it first with: REGEN=1 mise run bench:react"
  exit 1
fi
CASES="$(wc -l < "$MANIFEST" | tr -d ' ')"
CORPUS_BYTES="$(tr '\n' '\0' < "$MANIFEST" | xargs -0 stat -c%s 2>/dev/null | awk '{s+=$1} END{print s}')"
log "corpus: $CASES React files, $CORPUS_BYTES bytes"

# --- 1. tinyparse dist + CLI ----------------------------------------------
if [ ! -f "$LIB" ] || [ ! -x "$CLI" ]; then
  log "FATAL: tinyparse is not built ($LIB)"
  exit 1
fi

# --- 2. tree-sitter runtime + TSX grammar (reuse react-bench artifacts) ------
TSTS_REF="$TP/reference/tree-sitter-typescript"
TS_RUNTIME="$BENCHDIR/tree-sitter"
TS_INC="$TS_RUNTIME/lib/include"
TS_SRC="$TS_RUNTIME/lib/src"
TSX_SRC="$TSTS_REF/tsx/src"
if [ ! -d "$TSTS_REF/tsx/src" ]; then
  log "FATAL: tree-sitter-typescript reference missing at $TSTS_REF (see reference/PINS.txt)"
  exit 1
fi
if [ ! -d "$TS_RUNTIME/lib/src" ]; then
  log "cloning tree-sitter runtime ..."
  rm -rf "$TS_RUNTIME"
  git clone --depth 1 https://github.com/tree-sitter/tree-sitter "$TS_RUNTIME" >/dev/null 2>&1 || {
    log "FATAL: could not clone the tree-sitter runtime (network required)"; exit 1; }
fi
TS_PIN="$(git -C "$TS_RUNTIME" rev-parse HEAD 2>/dev/null)"
log "building tree-sitter runtime + TSX grammar ..."
if [ ! -f "$BENCHDIR/libtreesitter.a" ]; then
  $CC -std=c11 -O2 -D_DEFAULT_SOURCE -D_GNU_SOURCE -c -I "$TS_INC" -I "$TS_SRC" \
      "$TS_SRC/lib.c" -o "$BENCHDIR/ts_lib.o" || { log "FATAL: ts runtime"; exit 1; }
  "$TP_AR" rcs "$BENCHDIR/libtreesitter.a" "$BENCHDIR/ts_lib.o"
fi
$CC -std=c11 -O2 -c -I "$TS_INC" -I "$TSX_SRC" "$TSX_SRC/parser.c" \
    -o "$BENCHDIR/tsx_parser.o" || { log "FATAL: tsx parser.c"; exit 1; }
$CC -std=c11 -O2 -c -I "$TS_INC" -I "$TSX_SRC" "$TSX_SRC/scanner.c" \
    -o "$BENCHDIR/tsx_scanner.o" || { log "FATAL: tsx scanner.c"; exit 1; }
$CC -std=c11 -O2 -Wall -Wextra -I "$TS_INC" \
    "$BENCHDIR/react_treesitter.c" "$BENCHDIR/tsx_parser.o" "$BENCHDIR/tsx_scanner.o" \
    "$BENCHDIR/libtreesitter.a" -o "$BENCHDIR/react_treesitter" || {
  log "FATAL: tree-sitter harness did not compile"; exit 1; }

# --- 3. tinyparse VM harness + packed grammar ------------------------------
GRAMMAR_TPG="$BENCHDIR/react-corpus/std.tsx.tpg"
log "packing std.tsx -> $GRAMMAR_TPG ..."
"$CLI" pack std.tsx --out "$GRAMMAR_TPG" || { log "FATAL: pack std.tsx"; exit 1; }
$CC -std=c11 -O2 -Wall -Wextra -I "$TP/include" \
    "$BENCHDIR/react_tinyparse.c" "$LIB" \
    -o "$BENCHDIR/react_tinyparse" 2>/dev/null || {
  log "FATAL: tinyparse VM harness did not compile"; exit 1; }

# --- 4. tinyparse C-COMPILED arm: emit the parser, link the in-proc harness -
GEN_PARSER="$BENCHDIR/react-corpus/react_compiled_parser.c"
log "emitting compiled parser (tinyparse c std.tsx --parser-lib) ..."
# --name tp_compiled_parser pins the historical entry-symbol names the in-process
# harness (react_compiled.c) links against. Since W17-3 defaults the symbol prefix
# from the grammar name (std.tsx -> tsx_load/tsx_run), so without an explicit name
# the lib entries would be tsx_* and the harness would not link.
"$CLI" c std.tsx --parser-lib --name tp_compiled_parser --out "$GEN_PARSER" || {
  log "FATAL: could not emit the compiled parser"; exit 1; }
log "building the in-process compiled-parser harness ..."
$CC -std=c11 -O2 -Wall -Wextra -I "$TP/include" -I "$TP/src" \
    "$BENCHDIR/react_compiled.c" "$GEN_PARSER" "$LIB" \
    -o "$BENCHDIR/react_compiled" 2>/dev/null || {
  log "FATAL: compiled-parser harness did not compile"; exit 1; }

# --- 5. tsc + tsgo (reuse the tsc-bench scratch dirs) ------------------------
TSBENCH="$BENCHDIR/tsbench"
if [ ! -d "$TSBENCH/node_modules/typescript" ] || [ ! -d "$TSBENCH/node_modules/@typescript/native-preview" ]; then
  log "installing typescript + @typescript/native-preview into $TSBENCH ..."
  mkdir -p "$TSBENCH"
  [ -f "$TSBENCH/package.json" ] || printf '{\n  "name": "tsbench",\n  "private": true,\n  "type": "module"\n}\n' > "$TSBENCH/package.json"
  ( cd "$TSBENCH" && npm install --no-audit --no-fund typescript @typescript/native-preview ) >/dev/null 2>&1 || {
    log "FATAL: npm install failed (network required)"; exit 1; }
fi
TS_VER="$(cd "$TSBENCH" && node -e "console.log(require('typescript').version)" 2>/dev/null)"

# meriyah (the owner's bar) installs into the SAME tsbench scratch (gitignored).
if [ ! -d "$TSBENCH/node_modules/meriyah" ]; then
  log "installing meriyah into $TSBENCH ..."
  mkdir -p "$TSBENCH"
  ( cd "$TSBENCH" && npm install --no-audit --no-fund meriyah ) >/dev/null 2>&1 || {
    log "FATAL: meriyah npm install failed (network required)"; exit 1; }
fi

TSGO_SRC="$BENCHDIR/typescript-go"
TSGO_HARNESS="$TSGO_SRC/parsebench"
TSGO_OK=0; TSGO_FAIL_REASON=""; TSGO_PIN=""
if ! command -v go >/dev/null 2>&1; then
  TSGO_FAIL_REASON="go toolchain not on PATH"
elif [ ! -x "$TSGO_HARNESS" ]; then
  TSGO_FAIL_REASON="tsgo parse harness not built (run mise run bench:tsc once to build it)"
  [ -d "$TSGO_SRC/.git" ] && TSGO_PIN="$(git -C "$TSGO_SRC" rev-parse HEAD 2>/dev/null)"
else
  TSGO_OK=1
  TSGO_PIN="$(git -C "$TSGO_SRC" rev-parse HEAD 2>/dev/null)"
fi

# --- 6. run all five ---------------------------------------------------------
log "running tsc parse ($TS_ITERS iters) ..."
TSC_RAW="$(node "$BENCHDIR/tsc_bench.mjs" "$MANIFEST" "$TS_ITERS")" || { log "FATAL: tsc"; exit 1; }
log "running meriyah parse ($TS_ITERS iters) ..."
MERIYAH_RAW="$(node "$BENCHDIR/meriyah_bench.mjs" "$MANIFEST" "$TS_ITERS")" || { log "FATAL: meriyah"; exit 1; }
TSGO_RAW=""
if [ "$TSGO_OK" = "1" ]; then
  log "running tsgo parse ($TS_ITERS iters) ..."
  TSGO_RAW="$("$TSGO_HARNESS" "$MANIFEST" "$TS_ITERS")" || {
    log "tsgo harness failed at runtime"; TSGO_OK=0; TSGO_FAIL_REASON="harness exited non-zero"; }
fi
log "running tree-sitter parse ($ITERS iters) ..."
TS_RAW="$("$BENCHDIR/react_treesitter" "$MANIFEST" "$ITERS" parse)" || { log "FATAL: tree-sitter"; exit 1; }
log "running tinyparse VM parse ($ITERS iters) ..."
TPVM_RAW="$("$BENCHDIR/react_tinyparse" "$MANIFEST" "$ITERS" "$GRAMMAR_TPG" parse)" || { log "FATAL: tp-vm"; exit 1; }
log "running tinyparse C-compiled parse, BOTH arms ($ITERS iters) ..."
# "both" asserts the persistent-engine arm builds the fresh arm's tree byte-for-byte
# (the silent-corruption guard, 86c4317) BEFORE timing, then reports two BENCH lines:
#   parser=tinyparse-compiled   -- fresh-per-file, RSS-bounded (historical arm)
#   parser=tinyparse-persistent -- persistent engine, whole-codebase reuse
# A divergence aborts with a non-zero exit, so the bench can never time corrupt parses.
TPBOTH_RAW="$("$BENCHDIR/react_compiled" "$MANIFEST" "$ITERS" both)" || {
  log "FATAL: tp-compiled byte-identical assertion failed or harness errored"; exit 1; }
TPC_RAW="$(grep 'parser=tinyparse-compiled' <<<"$TPBOTH_RAW")"
TPP_RAW="$(grep 'parser=tinyparse-persistent' <<<"$TPBOTH_RAW")"

# --- 7. hardware + assemble the table ----------------------------------------
CPU_MODEL="$(grep -m1 'model name' /proc/cpuinfo 2>/dev/null | sed 's/.*: //')"
[ -z "$CPU_MODEL" ] && CPU_MODEL="$(uname -p 2>/dev/null)"
CPU_CORES="$(grep -c '^processor' /proc/cpuinfo 2>/dev/null)"
MEM_KB="$(grep -m1 MemTotal /proc/meminfo 2>/dev/null | awk '{print $2}')"
MEM_GB="?"; [ -n "$MEM_KB" ] && MEM_GB="$(awk "BEGIN{printf \"%.1f\", $MEM_KB/1048576}")"
HARDWARE="$CPU_MODEL ($CPU_CORES cores), ${MEM_GB} GiB, $(uname -srm)"

tsc_t="$(field "$TSC_RAW" median_seconds)";   tsc_mbps="$(field "$TSC_RAW" throughput_mbps)"
mer_t="$(field "$MERIYAH_RAW" median_seconds)"; mer_mbps="$(field "$MERIYAH_RAW" throughput_mbps)"
mer_parsed="$(field "$MERIYAH_RAW" parsed)";   mer_failed="$(field "$MERIYAH_RAW" failed)"
mer_ver="$(field "$MERIYAH_RAW" meriyah_version)"
ts_t="$(field "$TS_RAW" median_seconds)";      ts_mbps="$(field "$TS_RAW" throughput_mbps)"
tpvm_t="$(field "$TPVM_RAW" median_seconds)";  tpvm_mbps="$(field "$TPVM_RAW" throughput_mbps)"
tpc_t="$(field "$TPC_RAW" median_seconds)";    tpc_mbps="$(field "$TPC_RAW" throughput_mbps)"
tpp_t="$(field "$TPP_RAW" median_seconds)";    tpp_mbps="$(field "$TPP_RAW" throughput_mbps)"
tpc_rss="$(field "$TPC_RAW" peak_rss_kb)";      tpp_rss="$(field "$TPP_RAW" peak_rss_kb)"
if [ "$TSGO_OK" = "1" ]; then
  tsgo_t="$(field "$TSGO_RAW" median_seconds)"; tsgo_mbps="$(field "$TSGO_RAW" throughput_mbps)"
else
  tsgo_t=""; tsgo_mbps=""
fi

# Fastest MB/s among the available arms, for the ratio column.
FASTEST="$(printf '%s\n%s\n%s\n%s\n%s\n%s\n%s\n' "$tsc_mbps" "$tsgo_mbps" "$mer_mbps" "$ts_mbps" "$tpvm_mbps" "$tpc_mbps" "$tpp_mbps" \
  | awk 'NF{if($1>m)m=$1} END{print m}')"
ratio() { awk "BEGIN{ if($1>0) printf \"%.1fx\", $FASTEST/$1; else printf \"n/a\" }"; }

row() { # label  median_s  mbps
  if [ -z "$2" ]; then
    printf '%-26s | %-13s | %-12s | %-8s\n' "$1" "n/a" "n/a" "n/a"
  else
    printf '%-26s | %-13s | %-12s | %-8s\n' "$1" \
      "$(awk "BEGIN{printf \"%.3f s\", $2}")" "$3 MB/s" "$(ratio "$3")"
  fi
}

# C-vs-VM verdict.
VERDICT="$(awk "BEGIN{
  vm=$tpvm_mbps+0; c=$tpc_mbps+0;
  if(vm<=0||c<=0){print \"indeterminate\"; exit}
  r=c/vm;
  if(r>=1.05) printf \"C-compiled is FASTER than the VM: %.2fx the VM throughput (%.2f vs %.2f MB/s).\", r, c, vm;
  else if(r<=0.95) printf \"C-compiled is SLOWER than the VM: %.2fx the VM throughput (%.2f vs %.2f MB/s).\", r, c, vm;
  else printf \"C-compiled is NOT meaningfully faster than the VM: %.2fx the VM throughput (%.2f vs %.2f MB/s, within noise).\", r, c, vm;
}")"

echo
echo "=========== Six parsers, parse-only, ReactJS (std.tsx) -- W16-1 ==========="
echo "Hardware:   $HARDWARE"
echo "Corpus:     $CASES React files, $CORPUS_BYTES bytes (clean-parsing under std.tsx)"
echo "Iterations: in-process arms $ITERS; tsc/tsgo/meriyah $TS_ITERS (median reported)"
echo "tsc:        typescript@${TS_VER:-?}, ScriptKind.TSX, parse-only"
echo "meriyah:    meriyah@${mer_ver:-?}, parseModule jsx, parse-only -- parsed ${mer_parsed:-?}/$CASES"
echo "             (failed ${mer_failed:-?}: TS/Flow files meriyah rejects); MB/s over the parsed subset"
[ "$TSGO_OK" = "1" ] && echo "tsgo:       microsoft/typescript-go @ ${TSGO_PIN:-?}, ScriptKindTSX, parse-only"
echo "tree-sitter: @ ${TS_PIN:-?}, grammar tree-sitter-typescript TSX"
echo "tinyparse: VM and C-compiled arms parse the same bytes per-file-fresh-engine,"
echo "             only the parse call timed -- cc-diff proves they build the same tree."
echo
printf '%-26s | %-13s | %-12s | %-8s\n' "parser (parse-only)" "median parse" "throughput" "ratio"
printf '%-26s-+-%-13s-+-%-12s-+-%-8s\n' "--------------------------" "-------------" "------------" "--------"
row "tsc (compiler API)"          "$tsc_t"  "$tsc_mbps"
row "tsgo (typescript-go)"        "$tsgo_t" "$tsgo_mbps"
row "meriyah (JS+JSX subset)"     "$mer_t"  "$mer_mbps"
row "tree-sitter (TSX, GLR)"      "$ts_t"   "$ts_mbps"
row "tinyparse (bytecode VM)"   "$tpvm_t" "$tpvm_mbps"
row "tp C, fresh-per-file"        "$tpc_t"  "$tpc_mbps"
row "tp C, persistent-engine"     "$tpp_t"  "$tpp_mbps"
echo "(ratio = how many times slower than the fastest arm; lower median is better.)"
echo
echo "tinyparse (C-compiled), the SAME corpus two ways -- byte-identical (asserted):"
echo "  fresh-per-file, RSS-bounded:    ${tpc_mbps:-?} MB/s, peak RSS ${tpc_rss:-?} KB"
echo "  persistent-engine, whole-codebase: ${tpp_mbps:-?} MB/s, peak RSS ${tpp_rss:-?} KB"
echo "  The fresh arm reopens the engine + reloads the grammar per file (bounding"
echo "  peak memory to one file); the persistent arm reuses one engine across the"
echo "  corpus (parser_free reclaims each parse's arena). Both time only push+run."
echo
echo "C-vs-VM verdict: $VERDICT"
[ "$TSGO_OK" != "1" ] && echo "tsgo: NOT OBTAINED -- $TSGO_FAIL_REASON"
echo "==========================================================================="
echo
echo "raw tsc:         $TSC_RAW"
[ -n "$TSGO_RAW" ] && echo "raw tsgo:        $TSGO_RAW"
echo "raw meriyah:     $MERIYAH_RAW"
echo "raw tree-sitter: $TS_RAW"
echo "raw tp-vm:       $TPVM_RAW"
echo "raw tp-compiled: $TPC_RAW"
echo "raw tp-persistent: $TPP_RAW"
