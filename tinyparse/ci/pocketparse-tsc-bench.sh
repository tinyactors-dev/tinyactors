#!/usr/bin/env bash
# ci/pocketparse-tsc-bench.sh -- W1 of the pocketparse exploration: calibrate
# pocketparse against the two PRODUCTION hand-written recursive-descent TypeScript
# parsers, parse-only, over the EXACT same React file set the react bench uses.
#
#   * tsc   -- the TypeScript compiler's parser, via the compiler API
#             (ts.createSourceFile(..., /*setParentNodes*/ false, ScriptKind.TSX)).
#             Parse only: no Program, no binder beyond the parser, no checker.
#   * tsgo  -- microsoft/typescript-go's parser (the native Go rewrite), driven
#             parse-only via parser.ParseSourceFile(..., ScriptKindTSX). No binder,
#             no checker, no module resolution.
#
# Both parse the identical bytes listed in
#   tools/pocketparse/bench/react-corpus/MANIFEST.txt
# (1070 React .js files, ~9.5 MB), the same manifest ci/pocketparse-react-bench.sh
# produces, so the table reads pocketparse vs tree-sitter vs tsc vs tsgo over one
# corpus. For context (measured elsewhere): tree-sitter ~1.10 s, pocketparse ~43 s
# adaptive / ~13 s memo-forced over this same set.
#
# GATED behind BENCH=1 like the sibling bench scripts. Run from the repo root:
#
#     BENCH=1 bash ci/pocketparse-tsc-bench.sh
#
# Re-runnably:
#   1. install typescript + @typescript/native-preview into a gitignored scratch
#      dir (tools/pocketparse/bench/tsbench) if not already present;
#   2. clone microsoft/typescript-go into a gitignored scratch dir and build the
#      parse-only Go harness against its internal parser package;
#   3. warm up, then time parsing the whole manifest, median over ITERS iters;
#   4. print median parse seconds + MB/s for tsc and tsgo.
set -uo pipefail

if [ "${BENCH:-0}" != "1" ]; then
  echo "ci/pocketparse-tsc-bench.sh is gated behind BENCH=1 (skipping)."
  echo "Run it with:  BENCH=1 bash ci/pocketparse-tsc-bench.sh"
  exit 0
fi

REPO="${CLAUDE_PROJECT_DIR:-$(cd "$(dirname "$0")/.." && pwd)}"
PP="$REPO/tools/pocketparse"
BENCHDIR="$PP/bench"
cd "$REPO" || exit 1

ITERS="${ITERS:-15}"
MANIFEST="$BENCHDIR/react-corpus/MANIFEST.txt"

log() { printf '[tsc-bench] %s\n' "$*" >&2; }

if [ ! -s "$MANIFEST" ]; then
  log "FATAL: corpus manifest missing at $MANIFEST"
  log "       generate it first with: BENCH=1 bash ci/pocketparse-react-bench.sh"
  exit 1
fi
CASES="$(wc -l < "$MANIFEST" | tr -d ' ')"
CORPUS_BYTES="$(tr '\n' '\0' < "$MANIFEST" | xargs -0 stat -c%s 2>/dev/null | awk '{s+=$1} END{print s}')"

# --- 1. tsc: install typescript ---------------------------------------------
TSBENCH="$BENCHDIR/tsbench"
if [ ! -d "$TSBENCH/node_modules/typescript" ] || [ ! -d "$TSBENCH/node_modules/@typescript/native-preview" ]; then
  log "installing typescript + @typescript/native-preview into $TSBENCH ..."
  mkdir -p "$TSBENCH"
  [ -f "$TSBENCH/package.json" ] || printf '{\n  "name": "tsbench",\n  "private": true,\n  "type": "module"\n}\n' > "$TSBENCH/package.json"
  ( cd "$TSBENCH" && npm install --no-audit --no-fund typescript @typescript/native-preview ) >/dev/null 2>&1 || {
    log "FATAL: npm install failed (network required)"; exit 1; }
fi
TS_VER="$(cd "$TSBENCH" && node -e "console.log(require('typescript').version)" 2>/dev/null)"

# --- 2. tsgo: clone typescript-go + build the parse-only Go harness ----------
TSGO_SRC="$BENCHDIR/typescript-go"
TSGO_HARNESS="$TSGO_SRC/parsebench"
TSGO_OK=0
TSGO_FAIL_REASON=""
if ! command -v go >/dev/null 2>&1; then
  TSGO_FAIL_REASON="go toolchain not on PATH"
else
  if [ ! -d "$TSGO_SRC/.git" ]; then
    log "cloning microsoft/typescript-go ..."
    rm -rf "$TSGO_SRC"
    if ! git clone --depth 1 https://github.com/microsoft/typescript-go "$TSGO_SRC" >/dev/null 2>&1; then
      TSGO_FAIL_REASON="could not clone microsoft/typescript-go (network required)"
    fi
  fi
  TSGO_PIN="$(git -C "$TSGO_SRC" rev-parse HEAD 2>/dev/null)"
fi

if [ -z "$TSGO_FAIL_REASON" ]; then
  # Drop the parse-only harness into the module so it can import the (internal)
  # parser package, then build it. The module pins a newer Go via its `toolchain`
  # directive; the go command fetches that toolchain automatically.
  mkdir -p "$TSGO_SRC/cmd/parsebench"
  cat > "$TSGO_SRC/cmd/parsebench/main.go" <<'GOEOF'
// parsebench: parse-only benchmark over a manifest, mirroring the tsc harness.
// parser.ParseSourceFile with ScriptKindTSX; no binder, checker, or resolution.
package main

import (
	"bufio"
	"fmt"
	"os"
	"sort"
	"strconv"
	"time"

	"github.com/microsoft/typescript-go/internal/ast"
	"github.com/microsoft/typescript-go/internal/core"
	"github.com/microsoft/typescript-go/internal/parser"
	"github.com/microsoft/typescript-go/internal/tspath"
)

func main() {
	if len(os.Args) < 3 {
		fmt.Fprintln(os.Stderr, "usage: parsebench MANIFEST ITERS")
		os.Exit(2)
	}
	manifest := os.Args[1]
	iters, err := strconv.Atoi(os.Args[2])
	if err != nil || iters < 1 {
		fmt.Fprintln(os.Stderr, "ITERS must be a positive integer")
		os.Exit(2)
	}

	mf, err := os.Open(manifest)
	if err != nil {
		fmt.Fprintln(os.Stderr, "open manifest:", err)
		os.Exit(1)
	}
	var paths []string
	sc := bufio.NewScanner(mf)
	sc.Buffer(make([]byte, 1024*1024), 1024*1024)
	for sc.Scan() {
		if line := sc.Text(); line != "" {
			paths = append(paths, line)
		}
	}
	mf.Close()

	texts := make([]string, len(paths))
	var totalBytes int64
	for i, p := range paths {
		b, err := os.ReadFile(p)
		if err != nil {
			fmt.Fprintln(os.Stderr, "read", p, ":", err)
			os.Exit(1)
		}
		texts[i] = string(b)
		totalBytes += int64(len(b))
	}

	parseAll := func() int {
		n := 0
		for i, text := range texts {
			opts := ast.SourceFileParseOptions{
				FileName:                       paths[i],
				Path:                           tspath.Path(paths[i]),
				ExternalModuleIndicatorOptions: ast.ExternalModuleIndicatorOptions{JSX: true},
			}
			sf := parser.ParseSourceFile(opts, text, core.ScriptKindTSX)
			if sf != nil {
				n += len(sf.Statements.Nodes)
			}
		}
		return n
	}

	warm := parseAll()

	secs := make([]float64, 0, iters)
	for it := 0; it < iters; it++ {
		start := time.Now()
		parseAll()
		secs = append(secs, time.Since(start).Seconds())
	}
	sort.Float64s(secs)
	median := secs[len(secs)/2]
	mb := float64(totalBytes) / (1024.0 * 1024.0)
	mbps := mb / median

	fmt.Printf("files=%d bytes=%d top_statements=%d iterations=%d median_seconds=%.6f throughput_mbps=%.2f\n",
		len(paths), totalBytes, warm, iters, median, mbps)
}
GOEOF
  log "building tsgo parse-only harness (Go may fetch the module's pinned toolchain) ..."
  if ( cd "$TSGO_SRC" && GOFLAGS=-mod=mod go build -o parsebench ./cmd/parsebench/ ) >/dev/null 2>&1 && [ -x "$TSGO_HARNESS" ]; then
    TSGO_OK=1
  else
    TSGO_FAIL_REASON="go build of the parse harness failed (see: cd $TSGO_SRC && go build ./cmd/parsebench/)"
  fi
fi

# --- 3. run -----------------------------------------------------------------
log "running tsc parse ($ITERS iterations) ..."
TSC_RAW="$(node "$BENCHDIR/tsc_bench.mjs" "$MANIFEST" "$ITERS")" || {
  log "FATAL: tsc harness failed"; exit 1; }

TSGO_RAW=""
if [ "$TSGO_OK" = "1" ]; then
  log "running tsgo parse ($ITERS iterations) ..."
  TSGO_RAW="$("$TSGO_HARNESS" "$MANIFEST" "$ITERS")" || {
    log "tsgo harness failed at runtime"; TSGO_OK=0; TSGO_FAIL_REASON="harness ran but exited non-zero"; }
fi

# Hardware capture (same shape as the react bench).
CPU_MODEL="$(grep -m1 'model name' /proc/cpuinfo 2>/dev/null | sed 's/.*: //')"
[ -z "$CPU_MODEL" ] && CPU_MODEL="$(uname -p 2>/dev/null)"
CPU_CORES="$(grep -c '^processor' /proc/cpuinfo 2>/dev/null)"
MEM_KB="$(grep -m1 MemTotal /proc/meminfo 2>/dev/null | awk '{print $2}')"
MEM_GB="?"; [ -n "$MEM_KB" ] && MEM_GB="$(awk "BEGIN{printf \"%.1f\", $MEM_KB/1048576}")"
HARDWARE="$CPU_MODEL ($CPU_CORES cores), ${MEM_GB} GiB RAM, $(uname -srm)"

field() { sed -n "s/.*\b$2=\([^ ]*\).*/\1/p" <<<"$1"; }
fmt_s() { awk "BEGIN{printf \"%.3f s\", $1}"; }

tsc_t="$(field "$TSC_RAW" median_seconds)"; tsc_mbps="$(field "$TSC_RAW" throughput_mbps)"

echo
echo "============ pocketparse calibration -- production TS parsers (parse-only) ============"
echo "Hardware:   $HARDWARE"
echo "Corpus:     $CASES React .js files, $CORPUS_BYTES bytes (same set as the react bench)"
echo "Iterations: $ITERS (median reported)"
echo "tsc:        typescript@$TS_VER, ts.createSourceFile(setParentNodes=false, ScriptKind.TSX)"
if [ "$TSGO_OK" = "1" ]; then
  echo "tsgo:       microsoft/typescript-go @ ${TSGO_PIN:-?}, parser.ParseSourceFile(ScriptKindTSX), single-threaded"
fi
echo
printf '%-30s | %-14s | %-14s\n' "parser (parse-only)" "median parse" "throughput"
printf '%-30s-+-%-14s-+-%-14s\n' "------------------------------" "--------------" "--------------"
printf '%-30s | %-14s | %-14s\n' "tsc (compiler API)" "$(fmt_s "$tsc_t")" "${tsc_mbps} MB/s"
if [ "$TSGO_OK" = "1" ]; then
  tsgo_t="$(field "$TSGO_RAW" median_seconds)"; tsgo_mbps="$(field "$TSGO_RAW" throughput_mbps)"
  printf '%-30s | %-14s | %-14s\n' "tsgo (typescript-go)" "$(fmt_s "$tsgo_t")" "${tsgo_mbps} MB/s"
else
  printf '%-30s | %-14s | %-14s\n' "tsgo (typescript-go)" "n/a" "n/a"
fi
echo
echo "Context (measured elsewhere, same corpus): tree-sitter ~1.10 s; pocketparse ~43 s"
echo "adaptive / ~13 s memo-forced. tsc and tsgo are non-table hand-written recursive descent."
if [ "$TSGO_OK" != "1" ]; then
  echo
  echo "tsgo: NOT OBTAINED -- $TSGO_FAIL_REASON"
fi
echo "======================================================================================"
echo
echo "raw tsc:  $TSC_RAW"
[ -n "$TSGO_RAW" ] && echo "raw tsgo: $TSGO_RAW"
