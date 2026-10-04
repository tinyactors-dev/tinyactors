#!/usr/bin/env bash
# ci/pocketparse-js-coverage.sh — corpus coverage for the std.javascript grammar.
#
# A helper, NOT wired into the gate. For every case in the vendored tree-sitter
# JavaScript corpus it extracts the input, runs `pocketparse run std.javascript`
# on it, and counts a case as "parsed" when the CLI exits 0 AND the dumped tree
# carries no ERROR or MISSING node. It prints the failing case titles and a final
#
#     JS_COVERAGE=<parsed>/<total>
#
# line. Run from the repo root:  bash ci/pocketparse-js-coverage.sh
set -uo pipefail

REPO="${CLAUDE_PROJECT_DIR:-$(cd "$(dirname "$0")/.." && pwd)}"
PP_DIR="$REPO/tools/pocketparse"
CORPUS="$PP_DIR/reference/tree-sitter-javascript/test/corpus"

platform() {
  case "$(uname -s)/$(uname -m)" in
    Linux/x86_64) echo linux-x86_64 ;;
    Darwin/arm64|Darwin/aarch64) echo macos-arm64 ;;
    *) echo linux-x86_64 ;;
  esac
}
PLAT="$(platform)"
PP="$PP_DIR/dist/$PLAT/pocketparse"

# Build the CLI if it is not already present, so the script is self-contained.
if [ ! -x "$PP" ]; then
  ( cd "$PP_DIR" && bash build.sh dist ) >/dev/null 2>&1
fi
if [ ! -x "$PP" ]; then
  echo "FATAL: could not build/find pocketparse at $PP"
  echo "JS_COVERAGE=0/0"
  exit 1
fi

if [ ! -d "$CORPUS" ]; then
  echo "FATAL: corpus not found at $CORPUS"
  echo "JS_COVERAGE=0/0"
  exit 1
fi

tmp="$(mktemp -d)"
trap 'rm -rf "$tmp"' EXIT

# Split the corpus into per-case input files. Tree-sitter case format:
#   ====...        (a run of '=' >= 3)
#   Title
#   ====...
#   <input bytes>
#   ----...        (a run of '-' >= 3, the input/expected separator)
#   (expected tree)
# We keep only the input between the title's closing '=' line and the '---' line.
python3 - "$CORPUS" "$tmp" <<'PY'
import sys, os, re, glob
corpus, out = sys.argv[1], sys.argv[2]
idx = 0
manifest = open(os.path.join(out, "cases.tsv"), "w")
for path in sorted(glob.glob(os.path.join(corpus, "*.txt"))):
    txt = open(path, encoding="utf-8").read()
    # Each case: header block (=== / title / ===), then body up to next header.
    pat = re.compile(r'(?m)^={3,}[ \t]*\n(.*?)\n={3,}[ \t]*\n')
    parts = pat.split(txt)
    # parts = [preamble, title1, body1, title2, body2, ...]
    base = os.path.basename(path).replace(".txt", "")
    for i in range(1, len(parts), 2):
        title = parts[i].strip()
        body = parts[i+1]
        # body = input  '----'  expected-tree   (and maybe trailing cases already split)
        m = re.search(r'(?m)^-{3,}[ \t]*\n', body)
        if not m:
            continue
        inp = body[:m.start()]
        # strip a single leading blank line tree-sitter inserts, keep the rest verbatim
        if inp.startswith("\n"):
            inp = inp[1:]
        # trim only trailing newlines so we feed exactly the source
        inp = inp.rstrip("\n") + "\n"
        fn = os.path.join(out, f"case_{idx:04d}.js")
        with open(fn, "w", encoding="utf-8") as f:
            f.write(inp)
        manifest.write(f"{idx:04d}\t{base}\t{title}\n")
        idx += 1
manifest.close()
print(idx)
PY

total=0
parsed=0
declare -a fails
while IFS=$'\t' read -r id base title; do
  total=$((total + 1))
  f="$tmp/case_${id}.js"
  out="$("$PP" run std.javascript --input "$f" --dump-tree 2>/dev/null)"
  rc=$?
  if [ "$rc" -eq 0 ] && ! printf '%s' "$out" | grep -qE 'ERROR|MISSING'; then
    parsed=$((parsed + 1))
  else
    fails+=("[$base] $title")
  fi
done < "$tmp/cases.tsv"

if [ "${#fails[@]}" -gt 0 ]; then
  echo "== failing cases (${#fails[@]}) =="
  for t in "${fails[@]}"; do
    echo "  FAIL $t"
  done
fi

echo "JS_COVERAGE=$parsed/$total"
