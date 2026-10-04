#!/usr/bin/env bash
# ci/tinyparse-import-c.sh — the W17-1 import->C gate.
#
# tinyparse grammars compose with (import NAME): the compiler asks the host's
# module resolver for the module's bytes and MERGES its definitions at COMPILE
# time, so a grammar that imports `words' has `Word' in scope without declaring it.
# The "build from std/library pieces, then ship a generated C parser" use case
# depends on that merge surviving all the way into the emitted C — i.e. the packed
# grammar the generated parser embeds must already carry the imported definitions.
#
# This gate proves it end to end through examples/import (main.scm imports
# words.scm): it compiles main.scm to a standalone C parser, builds and runs it on
# a committed sample, and asserts the tree matches BOTH a committed golden AND the
# VM (defence in depth: the golden pins the expected shape, the VM cross-checks the
# generated parser against the reference engine on the same composed grammar).
#
# Run it from the repo root:
#     bash ci/tinyparse-import-c.sh
#
# Exit 0 only when the imported grammar compiles to C, builds, runs, and produces
# the golden tree, identical to the VM.
set -uo pipefail
REPO="$(cd "$(dirname "$0")/.." && pwd)"
TP_DIR="$REPO"
platform() {
  case "$(uname -s)/$(uname -m)" in
    Linux/x86_64) echo linux-x86_64 ;;
    Darwin/arm64|Darwin/aarch64) echo macos-arm64 ;;
    *) echo linux-x86_64 ;;
  esac
}
PLAT="$(platform)"
TP="$TP_DIR/dist/$PLAT/tinyparse"
LIB="$TP_DIR/dist/$PLAT/libtinyparse.a"
INC="$TP_DIR/include"
SRC="$TP_DIR/src"
CC="${CC:-cc}"

GRAMMAR="$TP_DIR/examples/import/main.scm"
SAMPLE="$TP_DIR/examples/import/fixtures/sample.words"
GOLDEN="$TP_DIR/examples/import/fixtures/expected.tree"

echo "== tinyparse import->C gate =="

# 1. Build the CLI + the library archive (the generated parser links against it).
( cd "$TP_DIR" && bash build.sh cli ) 2>&1 | sed 's/^/  /'
if [ ! -x "$TP" ] || [ ! -f "$LIB" ]; then
  echo "FATAL: CLI ($TP) or libtinyparse ($LIB) not built"; exit 1
fi
for f in "$GRAMMAR" "$SAMPLE" "$GOLDEN"; do
  [ -f "$f" ] || { echo "FATAL: missing fixture $f"; exit 1; }
done

SCRATCH="$(mktemp -d)"
trap 'rm -rf "$SCRATCH"' EXIT
CFILE="$SCRATCH/import_parser.c"
BIN="$SCRATCH/import_parser"

# 2. Compile the IMPORTING grammar to a standalone C parser. The compile-time
#    module resolver (installed by the CLI, relative to the grammar's directory)
#    merges words.scm; the emitted parser must embed the merged grammar.
if ! "$TP" c "$GRAMMAR" --parser --out "$CFILE" 2>"$SCRATCH/gen.err"; then
  echo "  GEN FAIL: $(head -3 "$SCRATCH/gen.err")"; exit 1
fi

# 3. Build it.
if ! "$CC" -std=c11 -O2 -I"$INC" -I"$SRC" \
      "$CFILE" "$LIB" -o "$BIN" 2>"$SCRATCH/cc.err"; then
  echo "  CC FAIL:"; sed 's/^/      /' "$SCRATCH/cc.err" | head -20; exit 1
fi

# 4. Run it on the sample and compare to the golden AND the VM.
"$BIN" < "$SAMPLE" > "$SCRATCH/got.tree" 2>&1; rc=$?
"$TP" run "$GRAMMAR" --input "$SAMPLE" --dump-tree \
  > "$SCRATCH/vm.tree" 2>&1

ok=1
if [ "$rc" != "0" ]; then echo "  RUN FAIL: generated parser exit=$rc"; ok=0; fi
if ! diff -q "$GOLDEN" "$SCRATCH/got.tree" >/dev/null; then
  echo "  MISMATCH vs golden:"; diff "$GOLDEN" "$SCRATCH/got.tree" | sed 's/^/      /' | head -20; ok=0
fi
if ! diff -q "$SCRATCH/vm.tree" "$SCRATCH/got.tree" >/dev/null; then
  echo "  MISMATCH vs VM:"; diff "$SCRATCH/vm.tree" "$SCRATCH/got.tree" | sed 's/^/      /' | head -20; ok=0
fi

if [ "$ok" = "1" ]; then
  echo "== import-c: GREEN (imported grammar compiles to C; tree matches golden + VM) =="
  exit 0
fi
echo "== import-c: RED =="
exit 1
