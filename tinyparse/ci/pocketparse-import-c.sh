#!/usr/bin/env bash
# ci/pocketparse-import-c.sh — the W17-1 import->C gate.
#
# pocketparse grammars compose with (import NAME): the compiler asks the host's
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
#     bash ci/pocketparse-import-c.sh
#
# Exit 0 only when the imported grammar compiles to C, builds, runs, and produces
# the golden tree, identical to the VM.
set -uo pipefail
REPO="${CLAUDE_PROJECT_DIR:-$(cd "$(dirname "$0")/.." && pwd)}"
PP_DIR="$REPO/tools/pocketparse"
platform() {
  case "$(uname -s)/$(uname -m)" in
    Linux/x86_64) echo linux-x86_64 ;;
    Darwin/arm64|Darwin/aarch64) echo macos-arm64 ;;
    *) echo linux-x86_64 ;;
  esac
}
PLAT="$(platform)"
PP="$PP_DIR/dist/$PLAT/pocketparse"
LIB="$PP_DIR/dist/$PLAT/libpocketparse.a"
AXDIR="$REPO/dist/$PLAT"
INC="$PP_DIR/include"
SRC="$PP_DIR/src"
CC="${CC:-cc}"

GRAMMAR="$PP_DIR/examples/import/main.scm"
SAMPLE="$PP_DIR/examples/import/fixtures/sample.words"
GOLDEN="$PP_DIR/examples/import/fixtures/expected.tree"

echo "== pocketparse import->C gate =="

# 1. Build the CLI + the library archive (the generated parser links against it).
( cd "$PP_DIR" && bash build.sh cli ) 2>&1 | sed 's/^/  /'
if [ ! -x "$PP" ] || [ ! -f "$LIB" ] || [ ! -f "$AXDIR/libax.a" ]; then
  echo "FATAL: CLI ($PP), libpocketparse ($LIB) or libax not built"; exit 1
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
if ! "$PP" c "$GRAMMAR" --parser --out "$CFILE" 2>"$SCRATCH/gen.err"; then
  echo "  GEN FAIL: $(head -3 "$SCRATCH/gen.err")"; exit 1
fi

# 3. Build it.
if ! "$CC" -std=c11 -O2 -I"$INC" -I"$AXDIR" -I"$SRC" \
      "$CFILE" "$LIB" "$AXDIR/libax.a" -o "$BIN" 2>"$SCRATCH/cc.err"; then
  echo "  CC FAIL:"; sed 's/^/      /' "$SCRATCH/cc.err" | head -20; exit 1
fi

# 4. Run it on the sample and compare to the golden AND the VM.
"$BIN" < "$SAMPLE" > "$SCRATCH/got.tree" 2>&1; rc=$?
POCKETPARSE_ENGINE=vm-ax "$PP" run "$GRAMMAR" --input "$SAMPLE" --dump-tree \
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
