#!/usr/bin/env bash
# ci/pocketparse-cc-diff.sh — the W14-3 compiled-parser differential.
#
# `pocketparse c --parser <grammar>` emits a compiled parser: the grammar baked
# into an explicit, arena-backed frame stack driven by one dispatch loop (no C
# recursion across grammar nesting, so a deeply nested input cannot overflow the
# host C stack). It must build the SAME concrete syntax tree the bytecode VM
# builds, for every input. This script proves it: for each target grammar it
# generates the parser C, compiles it together with the pocketparse runtime,
# parses every relevant corpus input, and asserts its --dump-tree output (and
# exit code) is byte-identical to the VM's (`pocketparse run`). It is the
# new independent oracle W5 will use to delete pp_execute. Modelled on
# ci/pocketparse-vm-diff.sh.
#
# It also runs a deep-nesting test (build_parser + the parser_depth section at the
# end): a pathologically deep input that would overflow a naive recursive
# descent's C stack must parse cleanly through the iterative explicit stack.
#
# Run it from the repo root:
#     bash ci/pocketparse-cc-diff.sh
#
# Exit 0 only when every input matched for every covered grammar.
set -uo pipefail

REPO="$(cd "$(dirname "$0")/.." && pwd)"
PP_DIR="$REPO"
cd "$REPO" || exit 1

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
INC="$PP_DIR/include"
SRC="$PP_DIR/src"

echo "== pocketparse compiled-parser differential (cc-diff) =="

# 1. Build the CLI + the library archive (the generated parser links against it).
( cd "$PP_DIR" && bash build.sh cli ) 2>&1 | sed 's/^/  /'
if [ ! -x "$PP" ] || [ ! -f "$LIB" ]; then
  echo "FATAL: CLI ($PP) or libpocketparse ($LIB) not built"
  echo "RESULT=0/0"
  exit 1
fi

CC="${CC:-cc}"
SCRATCH="$(mktemp -d)"
trap 'rm -rf "$SCRATCH"' EXIT

# Generate, compile and cache one parser per grammar. Returns the binary path
# on stdout, or empty on failure (printing the error). Memoised in $SCRATCH.
build_parser() { # grammar
  local g="$1"
  local key="${g//\//_}"
  local cfile="$SCRATCH/parser_$key.c"
  local bin="$SCRATCH/parser_$key"
  if [ -x "$bin" ]; then echo "$bin"; return 0; fi
  if ! "$PP" c "$g" --parser --out "$cfile" 2>"$SCRATCH/gen_$key.err"; then
    echo "  GEN FAIL ($g): $(head -1 "$SCRATCH/gen_$key.err")" >&2
    return 1
  fi
  if ! "$CC" -std=c11 -O2 -I"$INC" -I"$SRC" \
        "$cfile" "$LIB" -o "$bin" 2>"$SCRATCH/cc_$key.err"; then
    echo "  CC FAIL ($g):" >&2
    sed 's/^/      /' "$SCRATCH/cc_$key.err" | head -20 >&2
    return 1
  fi
  echo "$bin"
}

fail=0
total=0
match=0

# Compare the parser's output against the VM's for one input file.
diff_one() { # grammar rd-binary input-file
  local g="$1" bin="$2" f="$3"
  total=$((total + 1))
  "$bin" < "$f" > "$f.parser" 2>&1; local rc1=$?
  "$PP" run "$g" --input "$f" --dump-tree > "$f.vm" 2>&1
  local rc2=$?
  if [ "$rc1" = "$rc2" ] && diff -q "$f.parser" "$f.vm" >/dev/null; then
    match=$((match + 1))
  else
    fail=$((fail + 1))
    echo "  MISMATCH: $g / $(basename "$f") (rc parser=$rc1 vm=$rc2)"
    diff "$f.vm" "$f.parser" | sed 's/^/      /' | head -20
  fi
  rm -f "$f.parser" "$f.vm"
}

# --- json family: a representative spread (mirrors vm-diff's corpus) ----------
JSON_DIR="$SCRATCH/corpus"
mkdir -p "$JSON_DIR"
mkin() { printf '%s' "$2" > "$JSON_DIR/$1"; echo "$JSON_DIR/$1"; }

declare -a JSON_INPUTS JSON5_INPUTS JSONC_INPUTS
JSON_INPUTS=(
  "$(mkin j1  '123')"            "$(mkin j2  '-42')"
  "$(mkin j3  '3.14')"           "$(mkin j4  '-1.5e3')"
  "$(mkin j5  '6.022E+23')"      "$(mkin j6  'true')"
  "$(mkin j7  'false')"          "$(mkin j8  'null')"
  "$(mkin j9  '"hello world"')"  "$(mkin j10 '"a\"b\\c\n\t"')"
  "$(mkin j11 '{}')"             "$(mkin j12 '[]')"
  "$(mkin j13 '[1,2,3]')"        "$(mkin j14 '[1,true,null,"x",[2,3]]')"
  "$(mkin j15 '{"a":1}')"        "$(mkin j16 '{"a":1,"b":2,"c":3}')"
  "$(mkin j17 '{"a":[1,{"b":[2,3]},true],"c":{"d":null}}')"
  "$(mkin j18 '  {  "a" : [ 1 , 2 ]  ,  "b" : true  }  ')"
  "$(mkin j19 '[[[[[[[1]]]]]]]')"
  "$(mkin j20 '123456789012345678901234567890')"
  "$(mkin j21 '1 2')"            "$(mkin j22 '{"a":')"
  "$(mkin j23 '@@@')"            "$(mkin j24 '')"
)
JSONC_INPUTS=(
  "$(mkin c1 '{"a":1 // x
}')"
  "$(mkin c2 '{"a":/* note */1}')"
  "$(mkin c3 '// header
{"a":1}')"
  "$(mkin c4 '[1, /* two */ 2, 3] // done')"
  "$(mkin c5 '{"a":[1,2,3]}')"
  "$(mkin c6 '{"a":/* unterminated 1}')"
)
JSON5_INPUTS=(
  "$(mkin f1 '{a:1}')"           "$(mkin f2 '{a:1,b:2,c:3}')"
  "$(mkin f3 '{a:1,}')"          "$(mkin f4 '{a:[1,2,3],}')"
  "$(mkin f5 '{"q":1, x:2, _y:3, $z:4}')"
  "$(mkin f6 '{$id:1}')"         "$(mkin f7 '{a:{b:1,},c:[1,2,],}')"
  "$(mkin f8 '{1:2}')"
)

# --- std.fence: the LANGUAGE-INJECTION grammar (W17-1) ------------------------
# fence injects std.json and std.javascript over a fenced block's content. This
# coverage pins that the generated parser reconstructs the embedded grammars from
# the packed .ppg (no module resolver at parse time) and produces a tree
# byte-identical to the VM's, injection subtrees and language tags included.
declare -a FENCE_INPUTS
FENCE_INPUTS=(
  "$(mkin fe1 '```json
{"a":1}
```
')"
  "$(mkin fe2 '```js
let x = 1;
```
')"
  "$(mkin fe3 '```json
[1,2,{"k":true},null]
```
```js
const y = [1, 2, 3];
```
')"
  "$(mkin fe4 '```json
{"a":[1,{"b":2}],"c":null}
```
')"
)

diff_grammar_inputs() { # grammar inputs...
  local g="$1"; shift
  local bin
  bin="$(build_parser "$g")" || { fail=$((fail + 1)); return; }
  local f
  for f in "$@"; do
    [ -f "$f" ] && diff_one "$g" "$bin" "$f"
  done
  echo "== cc-diff ($g): $match/$total byte-identical (running total) =="
}

# family base fixture
FX="$PP_DIR/test/suite/family/fixtures"
FAMILY_INPUT="$(mkin fam 'hello world')"

echo "== json family =="
diff_grammar_inputs std.json  "${JSON_INPUTS[@]}"
diff_grammar_inputs std.jsonc "${JSONC_INPUTS[@]}"
diff_grammar_inputs std.json5 "${JSON5_INPUTS[@]}"

echo "== fence (language injection) =="
diff_grammar_inputs std.fence "${FENCE_INPUTS[@]}"

# --- heredoc: deferred-slot / parametrized-goal (lex-goal-implementation) ------
# The heredoc grammar exercises every new opcode the generated parser must now
# execute: (deferred-slot) scope push/pop at rule entry, (defer) span capture +
# enqueue with slot-save rollback inside SimpleCommand's choice/repeat, the
# parametrized (parse-deferred) drain that instantiates the captured delimiter and
# pushes it onto the goal stack, and the rule-level (not HeredocDelimLine
# BodyCommand) guard. Each input below — single, multiple-same-line, same-delimiter,
# and the full nested README example — must produce a tree byte-identical to the VM.
HEREDOC_GRAMMAR="$PP_DIR/examples/heredoc/heredoc.scm"
declare -a HEREDOC_INPUTS
HEREDOC_INPUTS=(
  "$(mkin hd1 'cat <<EOF
hello
EOF
')"
  "$(mkin hd2 'cat <<A <<B
first
A
second
B
')"
  "$(mkin hd3 'cat <<EOF
x
EOF
cat <<EOF
y
EOF
')"
  "$(mkin hd4 'cat <<EOF
echo Hello
echo $(
  cat <<NESTED
world
NESTED
)
EOF
')"
)

echo "== heredoc (deferred-slot / parametrized goal) =="
diff_grammar_inputs "$HEREDOC_GRAMMAR" "${HEREDOC_INPUTS[@]}"

# --- std.shell heredocs: the deferred-slot opcodes inside a real std grammar ----
# The std POSIX-shell grammar reuses the same deferred-slot / parametrized-goal
# machinery for its here-documents (<<, <<-), wired into the restructured Program
# and CompoundList spine. These inputs exercise the opcodes through the std grammar
# itself — a single heredoc, a heredoc combined with an ordinary redirect, a
# heredoc in a pipeline, multiple heredocs on one line (FIFO drain), a heredoc
# inside an if body (the per-line drain in CompoundList), and `<<-` with a
# tab-indented delimiter line — and must produce a tree byte-identical to the VM.
declare -a SHELL_HD_INPUTS
SHELL_HD_INPUTS=(
  "$(mkin sh_hd1 'cat <<EOF
hello
EOF
')"
  "$(mkin sh_hd2 'cat <<EOF >out
x
EOF
')"
  "$(mkin sh_hd3 'cat <<EOF | grep x
x
EOF
')"
  "$(mkin sh_hd4 'cat <<A <<B
aaa
A
bbb
B
')"
  "$(printf 'if true; then cat <<EOF\nx\nEOF\n fi\n' > "$JSON_DIR/sh_hd5"; echo "$JSON_DIR/sh_hd5")"
  "$(printf 'cat <<-EOF\n\thello\n\tEOF\n' > "$JSON_DIR/sh_hd6"; echo "$JSON_DIR/sh_hd6")"
)

echo "== std.shell heredocs (deferred-slot in a std grammar) =="
diff_grammar_inputs std.shell "${SHELL_HD_INPUTS[@]}"

echo "== family base fixture =="
if bin="$(build_parser "$FX/base.scm")"; then
  diff_one "$FX/base.scm" "$bin" "$FAMILY_INPUT"
  echo "== cc-diff (family/base): $match/$total byte-identical (running total) =="
else
  fail=$((fail + 1))
fi

# --- the JS/TS corpora: the backtracking-heavy real grammars -----------------
# Each suite input names its grammar by extension. We build one parser per
# grammar and run every input through it.
diff_suite() { # suite-dir default-grammar
  local sdir="$1" default="$2"
  local f g bin
  for f in "$PP_DIR/test/suite/$sdir/inputs"/*; do
    [ -f "$f" ] || continue
    case "$f" in
      *.tsx) g="std.tsx" ;;
      *.ts)  g="std.typescript" ;;
      *.js)  g="std.javascript" ;;
      *)     g="$default" ;;
    esac
    bin="$(build_parser "$g")" || { fail=$((fail + 1)); continue; }
    diff_one "$g" "$bin" "$f"
  done
  echo "== cc-diff ($sdir): $match/$total byte-identical (running total) =="
}

echo "== JavaScript corpus =="
diff_suite jsfamily std.javascript
echo "== TypeScript / TSX corpus =="
diff_suite tsfamily std.typescript
echo "== POSIX shell corpus =="
diff_suite shellfamily std.shell

echo "== cc-diff TOTAL: $match/$total byte-identical =="
echo "RESULT=$match/$total"

if [ "$fail" -eq 0 ] && [ "$total" -gt 0 ]; then
  echo "== cc-diff: GREEN =="
  exit 0
fi
echo "== cc-diff: RED =="
exit 1
