#!/usr/bin/env bash
# ci/pocketparse-vm-diff.sh — the bytecode-VM host-consistency harness.
#
# Since W14-5 the bytecode VM (tools/pocketparse/src/vm.c, lowered by vmcompile.c)
# is the SOLE engine; the recursive-descent IR interpreter (pp_execute) that this
# differential once held as its independent reference has been deleted. The
# standing INDEPENDENT correctness anchor is now the conformance suite
# (ci/pocketparse-metric.sh, 296 independent expected outputs) together with
# ci/pocketparse-cc-diff.sh (the C-compiled parser vs the VM). This script is the
# remaining HOST-CONSISTENCY check: the SAME VM run in process ("vm") and run
# inside an ax actor ("vm-ax") must build the byte-identical concrete syntax tree
# for every input. It proves this for the json/jsonc/json5 grammar family, the
# full JS/TS corpora, and every other grammar the suite parses (the example
# grammars and the family/query fixtures), so the consistency contract covers
# every feature the suite exercises.
#
# POCKETPARSE_ENGINE selects the engine: unset / "vm-ax" is the actor-hosted VM
# (the default), "vm" is the in-process VM. This script holds the reference fixed
# at "vm" (the in-process VM) and compares the actor-hosted "vm-ax" against it.
#
# It then runs a speed sanity check: parse one document many times under each
# engine and report the ratio.
#
# Run it from the repo root:
#     bash ci/pocketparse-vm-diff.sh
#
# Exit 0 only when every input matched.
set -uo pipefail

REPO="${CLAUDE_PROJECT_DIR:-$(cd "$(dirname "$0")/.." && pwd)}"
PP_DIR="$REPO/tools/pocketparse"
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

echo "== pocketparse bytecode-VM differential =="

# 1. Build the CLI (carries both engines; the VM is selected at run time).
( cd "$PP_DIR" && bash build.sh cli ) 2>&1 | sed 's/^/  /'
if [ ! -x "$PP" ]; then
  echo "FATAL: CLI not built at $PP"
  echo "RESULT=0/0"
  exit 1
fi

# 2. Lay down a corpus per json-family grammar. There is no committed json corpus,
#    so we generate a representative spread: scalars, nesting, escapes, the empty
#    cases, plus grammar-specific features (comments for jsonc; identifier keys and
#    trailing commas for json5) and a few inputs that must FAIL identically.
CORPUS="$(mktemp -d)"
trap 'rm -rf "$CORPUS"' EXIT

write_case() { # grammar name bytes
  local dir="$CORPUS/$1"
  mkdir -p "$dir"
  printf '%s' "$3" > "$dir/$2.input"
}

# --- std.json: strict JSON ---------------------------------------------------
write_case std.json scalar_int       '123'
write_case std.json scalar_neg       '-42'
write_case std.json scalar_float     '3.14'
write_case std.json scalar_exp       '-1.5e3'
write_case std.json scalar_exp2      '6.022E+23'
write_case std.json scalar_true      'true'
write_case std.json scalar_false     'false'
write_case std.json scalar_null      'null'
write_case std.json scalar_string    '"hello world"'
write_case std.json string_escape    '"a\"b\\c\n\t"'
write_case std.json empty_object     '{}'
write_case std.json empty_array      '[]'
write_case std.json array_simple     '[1,2,3]'
write_case std.json array_mixed      '[1,true,null,"x",[2,3]]'
write_case std.json object_simple    '{"a":1}'
write_case std.json object_multi     '{"a":1,"b":2,"c":3}'
write_case std.json nested           '{"a":[1,{"b":[2,3]},true],"c":{"d":null}}'
write_case std.json whitespace       '  {  "a" : [ 1 , 2 ]  ,  "b" : true  }  '
write_case std.json deep             '[[[[[[[1]]]]]]]'
write_case std.json bignum           '123456789012345678901234567890'
# negative cases (must fail identically under both engines)
write_case std.json err_trailing     '1 2'
write_case std.json err_unterminated '{"a":'
write_case std.json err_garbage      '@@@'
write_case std.json err_empty        ''

# --- std.jsonc: JSON with comments ------------------------------------------
write_case std.jsonc line_comment    '{"a":1 // trailing
}'
write_case std.jsonc block_comment   '{"a":/* note */1}'
write_case std.jsonc lead_comment    '// header
{"a":1}'
write_case std.jsonc comment_array   '[1, /* two */ 2, 3] // done'
write_case std.jsonc plain_json      '{"a":[1,2,3]}'
write_case std.jsonc err_open_block  '{"a":/* unterminated 1}'

# --- std.json5: identifier keys + trailing commas ---------------------------
write_case std.json5 ident_key       '{a:1}'
write_case std.json5 ident_keys      '{a:1,b:2,c:3}'
write_case std.json5 trailing_comma  '{a:1,}'
write_case std.json5 trailing_array  '{a:[1,2,3],}'
write_case std.json5 mixed_keys      '{"q":1, x:2, _y:3, $z:4}'
write_case std.json5 dollar_ident    '{$id:1}'
write_case std.json5 nested_json5    '{a:{b:1,},c:[1,2,],}'
write_case std.json5 err_bad_key     '{1:2}'

# --- Every other grammar the suite parses ------------------------------------
# Beyond the json family and the JS/TS corpora, the suite exercises a spread of
# example grammars and family/query fixtures, each reaching a different VM
# feature: the Tcl grammar's brace/command-substitution nesting and deferred
# bodies, the URI grammar's fields and optionals, the s-expression smoke
# grammar, the lexical-goal mini-JS (regex-vs-divide disambiguation), the
# import/module resolution grammar, the (class NAME) host binding grammar, the C
# #define rewriter, the recovery-DSL list grammar (MISSING insertion + an
# error-severity diagnostic), and the grammar-family fixtures (extends, override,
# and a (when PARAM:) gate, run both with and without the param). Each tuple is
# "grammar | input | extra-args"; the inputs mirror the suite's own cases so the
# differential covers exactly the features the suite depends on. The in-process VM
# and the actor-hosted VM must agree byte-for-byte on every one.
CASES_FILE="$CORPUS/cases.tsv"
: > "$CASES_FILE"
add_case() { # label grammar input [extra-args...]
  local label="$1" grammar="$2" input="$3"; shift 3
  printf '%s\t%s\t%s\t%s\n' "$label" "$grammar" "$input" "$*" >> "$CASES_FILE"
}
EX="$PP_DIR/examples"
FX="$PP_DIR/test/suite/family/fixtures"
QX="$PP_DIR/test/suite/query/fixtures"
add_case tcl-basic        "$EX/tcl/tcl.scm"            'set x 1'
add_case tcl-braces       "$EX/tcl/tcl.scm"            'set y {a {b} c}'
add_case tcl-cmdsub       "$EX/tcl/tcl.scm"            'set z [expr {1 + [foo]}]'
add_case tcl-mixed        "$EX/tcl/tcl.scm"            'puts "hi $world"; if {$x} { set a b }'
add_case uri-full         "$EX/uri/uri.scm"            'https://user@example.com:8080/a/b?x=1#frag'
add_case uri-bare         "$EX/uri/uri.scm"            'http://example.com'
add_case sexpr            "$EX/sexpr/sexpr.scm"        '(a (b c) d)'
add_case lexgoal          "$EX/lexgoal/mini-js.scm"    'a = /ab/ ; b = c / d ;'
add_case import           "$EX/import/main.scm"        'a b c'
add_case bindings-class   "$EX/bindings/ident.scm"     'foo bar123 x'
add_case jit              "$EX/jit/jit.scm"            'int f(int x){return x*x+1;}'
add_case rewriter-define  "$EX/rewriter/cdefines.scm"  '#define MAX_USERS 100'
add_case rewriter-sample  "$EX/rewriter/cdefines.scm"  "@FILE:$EX/rewriter/sample.c"
add_case recover-ok       "$EX/recover/list.scm"       '(a (b c))'
add_case recover-missing  "$EX/recover/list.scm"       '(a (b c)'
add_case family-base      "$FX/base.scm"               'hello world'
add_case family-merge     "$FX/ext-merge.scm"          'hello world'
add_case family-override  "$FX/ext-override.scm"       'hello world'
add_case family-bare      "$FX/ext-bare.scm"           'hello world'
add_case family-param-on  "$FX/ext-param.scm"          'hi !'  --param feature
add_case family-param-off "$FX/ext-param.scm"          'hi'
add_case query-word       "$QX/word.scm"               'a b c'
add_case query-word-eq    "$QX/word-eq.scm"            'a b c'

# std.shell here-documents: the deferred-slot / parametrized-goal opcodes exercised
# inside the std POSIX-shell grammar. Multi-line inputs come from committed fixture
# files (@FILE:) so the newline-significant heredoc bodies survive verbatim. The
# in-process and actor-hosted VM must agree on the deferred-slot drain, the
# parametrized HeredocDelimLine instantiation, and the FIFO multi-heredoc order.
SHX="$PP_DIR/test/suite/shellfamily/fixtures"
add_case shell-heredoc-single   std.shell "@FILE:$SHX/heredoc-single.input"
add_case shell-heredoc-multiple std.shell "@FILE:$SHX/heredoc-multiple.input"
add_case shell-heredoc-in-if    std.shell "@FILE:$SHX/heredoc-in-if.input"
add_case shell-heredoc-dash     std.shell "@FILE:$SHX/heredoc-dash.input"
add_case shell-heredoc-pipeline std.shell "@FILE:$SHX/heredoc-pipeline.input"

# The candidate executor compared against the reference in-process VM
# (POCKETPARSE_ENGINE=vm). "vm-ax" is the SAME VM hosted inside an ax actor (W60);
# it must produce --dump-tree output byte-identical to the in-process VM for every
# input. The differential below runs once per candidate engine; the script is
# GREEN only when EVERY candidate matched.
ENGINES="${ENGINES:-vm-ax}"
fail=0

# Exercise the W67 rule memo's replay path. The memo is adaptive -- dormant until a
# parse goes super-linear -- so the small differential inputs never wake it on
# their own. Re-run the whole differential once with the memo FORCED on (the run
# commands inherit POCKETPARSE_RULEMEMO), so the snapshot/replay path is held to
# the same byte-identity contract as the default path. The guard variable stops it
# from recursing past one extra pass.
if [ -z "${PP_RULEMEMO_PASS:-}" ] && [ -z "${POCKETPARSE_RULEMEMO:-}" ]; then
  echo "== vm-diff: extra pass with the rule memo forced on =="
  if ! PP_RULEMEMO_PASS=1 POCKETPARSE_RULEMEMO=always bash "$0" "$@"; then
    echo "== vm-diff: RED (rule-memo-forced pass diverged) =="
    exit 1
  fi
  echo "== vm-diff: rule-memo-forced pass GREEN; continuing with the default pass =="
fi

# 3b. Differential over the FULL committed JS and TS corpora (W61). These are the
#     backtracking-heavy real grammars the VM exists to accelerate, so the
#     byte-identity contract matters most here. Each suite input names its grammar
#     by file extension: .js -> std.javascript, .ts -> std.typescript, .tsx ->
#     std.tsx. We parse every input with both engines and assert identical
#     --dump-tree output and exit code, exactly as for json above.
diff_suite() { # engine suite-dir default-grammar
  local engine="$1" sdir="$2" default="$3"
  local stotal=0 smatch=0
  local f g
  for f in "$PP_DIR/test/suite/$sdir/inputs"/*; do
    [ -f "$f" ] || continue
    case "$f" in
      *.tsx) g="std.tsx" ;;
      *.ts)  g="std.typescript" ;;
      *.js)  g="std.javascript" ;;
      *)     g="$default" ;;
    esac
    stotal=$((stotal + 1))
    POCKETPARSE_ENGINE=vm "$PP" run "$g" --input "$f" --dump-tree > "$f.ref" 2>&1
    local rrc=$?
    POCKETPARSE_ENGINE="$engine" "$PP" run "$g" --input "$f" --dump-tree > "$f.vm" 2>&1
    local vrc=$?
    if [ "$rrc" = "$vrc" ] && diff -q "$f.ref" "$f.vm" >/dev/null; then
      smatch=$((smatch + 1))
    else
      fail=$((fail + 1))
      echo "  MISMATCH: $sdir / $(basename "$f") (rc ref=$rrc $engine=$vrc)"
      diff "$f.ref" "$f.vm" | sed 's/^/      /' | head -20
    fi
    rm -f "$f.ref" "$f.vm"
  done
  SUITE_MATCH=$smatch
  SUITE_TOTAL=$stotal
  echo "== differential ($engine): $smatch/$stotal byte-identical ($sdir) =="
}

# 3c. Differential over every OTHER grammar the suite parses: the example grammars
#     and the family/query fixtures laid down in CASES_FILE above. Each line is
#     "label<TAB>grammar<TAB>input<TAB>extra-args"; the input is literal bytes
#     unless it begins "@FILE:", which names a file to parse instead (so a real
#     source file like rewriter/sample.c is covered too). We parse with the
#     reference and the candidate engine and assert identical --dump-tree output
#     and exit code, exactly as for json and the JS/TS corpora.
diff_cases() { # engine
  local engine="$1"
  local ctotal=0 cmatch=0
  local label grammar input extra
  while IFS=$'\t' read -r label grammar input extra; do
    [ -n "$label" ] || continue
    local inpath
    case "$input" in
      @FILE:*) inpath="${input#@FILE:}" ;;
      *)       inpath="$CORPUS/case-$label.input"; printf '%s' "$input" > "$inpath" ;;
    esac
    ctotal=$((ctotal + 1))
    # shellcheck disable=SC2086
    POCKETPARSE_ENGINE=vm "$PP" run "$grammar" --input "$inpath" \
      --dump-tree $extra > "$inpath.ref" 2>&1
    local rrc=$?
    # shellcheck disable=SC2086
    POCKETPARSE_ENGINE="$engine" "$PP" run "$grammar" --input "$inpath" \
      --dump-tree $extra > "$inpath.vm" 2>&1
    local vrc=$?
    if [ "$rrc" = "$vrc" ] && diff -q "$inpath.ref" "$inpath.vm" >/dev/null; then
      cmatch=$((cmatch + 1))
    else
      fail=$((fail + 1))
      echo "  MISMATCH: case $label (rc ref=$rrc $engine=$vrc)"
      diff "$inpath.ref" "$inpath.vm" | sed 's/^/      /' | head -20
    fi
    rm -f "$inpath.ref" "$inpath.vm"
  done < "$CASES_FILE"
  SUITE_MATCH=$cmatch
  SUITE_TOTAL=$ctotal
  echo "== differential ($engine): $cmatch/$ctotal byte-identical (examples + fixtures) =="
}

# Run the whole differential (json/jsonc/json5 + jsfamily + tsfamily) for one
# candidate engine, accumulating into the global fail counter and printing the
# engine's TOTAL.
run_differential() { # engine
  local engine="$1"
  local total=0 match=0
  echo "== candidate engine: POCKETPARSE_ENGINE=$engine =="
  local gdir grammar input ref_rc vm_rc
  for gdir in "$CORPUS"/*/; do
    grammar="$(basename "$gdir")"
    for input in "$gdir"*.input; do
      [ -f "$input" ] || continue
      total=$((total + 1))
      POCKETPARSE_ENGINE=vm "$PP" run "$grammar" --input "$input" \
        --dump-tree > "$input.ref" 2>&1
      ref_rc=$?
      POCKETPARSE_ENGINE="$engine" "$PP" run "$grammar" --input "$input" \
        --dump-tree > "$input.vm" 2>&1
      vm_rc=$?
      if [ "$ref_rc" = "$vm_rc" ] && diff -q "$input.ref" "$input.vm" >/dev/null; then
        match=$((match + 1))
      else
        fail=$((fail + 1))
        echo "  MISMATCH: $grammar / $(basename "$input") (rc ref=$ref_rc $engine=$vm_rc)"
        diff "$input.ref" "$input.vm" | sed 's/^/      /' | head -20
      fi
      rm -f "$input.ref" "$input.vm"
    done
  done
  echo "== differential ($engine): $match/$total byte-identical (json/jsonc/json5) =="

  diff_suite "$engine" jsfamily std.javascript
  match=$((match + SUITE_MATCH)); total=$((total + SUITE_TOTAL))
  diff_suite "$engine" tsfamily std.typescript
  match=$((match + SUITE_MATCH)); total=$((total + SUITE_TOTAL))
  diff_suite "$engine" shellfamily std.shell
  match=$((match + SUITE_MATCH)); total=$((total + SUITE_TOTAL))
  diff_cases "$engine"
  match=$((match + SUITE_MATCH)); total=$((total + SUITE_TOTAL))

  echo "== differential TOTAL ($engine): $match/$total byte-identical (json + jsfamily + tsfamily + examples + fixtures) =="
  echo "RESULT[$engine]=$match/$total"
}

for ENGINE in $ENGINES; do
  run_differential "$ENGINE"
done

# 3c. Structural proof that the vm-ax path genuinely runs the parse on ax (the
#     inverse of the Phase 11 gap, where ax was used only as an arena allocator).
#     First, the source proof: the parse path calls ax_open/ax_define/ax_spawn/
#     ax_post/ax_step. Second, the runtime proof: POCKETPARSE_TRACE_ACTOR makes the
#     CLI report whether the parser actor's input.end action genuinely ran.
echo "== structural proof: ax calls on the vm-ax parse path =="
if grep -nE 'ax_(open|define|spawn|post|tell|step)' "$PP_DIR/src"/*.c \
     | grep -vE '/(cli|amalgamation)' | sed 's/^/  /'; then
  :
else
  echo "  FATAL: no ax_open/define/spawn/post/step on the parse path"
  fail=$((fail + 1))
fi

echo "== structural proof: the actor actually ran (POCKETPARSE_TRACE_ACTOR) =="
PROOF_INPUT="$CORPUS/std.json/nested.input"
actor_line="$(POCKETPARSE_ENGINE=vm-ax POCKETPARSE_TRACE_ACTOR=1 \
  "$PP" run std.json --input "$PROOF_INPUT" --dump-tree 2>&1 >/dev/null)"
echo "  $actor_line"
if [ "$actor_line" = "actor: parse ran on ax = 1" ]; then
  echo "  OK: the vm-ax parse executed inside the ax actor"
else
  echo "  FATAL: the vm-ax parse did NOT run inside the ax actor"
  fail=$((fail + 1))
fi
# W63: the DEFAULT path (no POCKETPARSE_ENGINE set) must now ALSO report the actor
# ran -- this is the whole point of the flip: parsing runs on ax for everyone.
default_line="$(POCKETPARSE_TRACE_ACTOR=1 \
  "$PP" run std.json --input "$PROOF_INPUT" --dump-tree 2>&1 >/dev/null)"
echo "  default path: $default_line"
if [ "$default_line" = "actor: parse ran on ax = 1" ]; then
  echo "  OK: the DEFAULT parse (no env var) executed inside the ax actor"
else
  echo "  FATAL: the default path did NOT run on the actor (the flip did not take)"
  fail=$((fail + 1))
fi
# The in-process VM opt-out must NOT report the actor ran (proves the opt-out, the
# reference for this differential, genuinely runs off the actor).
optout_line="$(POCKETPARSE_ENGINE=vm POCKETPARSE_TRACE_ACTOR=1 \
  "$PP" run std.json --input "$PROOF_INPUT" --dump-tree 2>&1 >/dev/null)"
echo "  in-process VM opt-out: $optout_line"
if [ "$optout_line" != "actor: parse ran on ax = 0" ]; then
  echo "  FATAL: the in-process VM opt-out unexpectedly ran on the actor"
  fail=$((fail + 1))
fi

# 4. Speed sanity check: parse one document many times under each engine and
#    report the wall-clock ratio. This is a coarse, subprocess-level signal (it
#    includes process startup, grammar compile, and the IR->bytecode lowering the
#    VM pays per run), not the in-process parse-only number ci/pocketparse-bench.sh
#    measures; it is reported AS MEASURED.
SPEED_INPUT="$CORPUS/std.json/nested.input"
ITERS="${VM_DIFF_ITERS:-400}"
if [ -f "$SPEED_INPUT" ]; then
  echo "== speed sanity ($ITERS iterations of std.json on $(basename "$SPEED_INPUT")) =="
  time_engine() { # engine-env
    local t0 t1
    t0=$(date +%s.%N)
    local i
    for ((i = 0; i < ITERS; i++)); do
      env $1 "$PP" run std.json --input "$SPEED_INPUT" >/dev/null 2>&1
    done
    t1=$(date +%s.%N)
    awk -v a="$t0" -v b="$t1" 'BEGIN{printf "%.3f", b-a}'
  }
  vm_t="$(time_engine 'POCKETPARSE_ENGINE=vm')"
  vma_t="$(time_engine 'POCKETPARSE_ENGINE=vm-ax')"
  echo "  bytecode VM (in process):       ${vm_t}s"
  echo "  bytecode VM on ax (default):    ${vma_t}s"
  awk -v v="$vm_t" -v a="$vma_t" 'BEGIN{
    if (a > 0) printf "  ratio (VM / VM-ax):         %.2fx (>1 means VM-ax faster)\n", v/a;
  }'
fi

if [ "$fail" -eq 0 ]; then
  echo "== vm-diff: GREEN (engines: $ENGINES) =="
  exit 0
fi
echo "== vm-diff: RED (engines: $ENGINES) =="
exit 1
