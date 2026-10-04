#!/usr/bin/env bash
# ci/pocketparse-ts-coverage.sh — corpus coverage for the std.typescript family.
#
# The TS-family sibling of ci/pocketparse-js-coverage.sh. For every case in the
# vendored tree-sitter TypeScript corpus it extracts the input, picks the dialect
# the case's `:language(...)` directive names (tsx, else typescript), runs
# `pocketparse run std.<dialect>` on it, and counts a case as "parsed" when the
# CLI exits 0 AND the dumped tree carries no ERROR or MISSING node. It prints the
# failing case titles and a final
#
#     TS_COVERAGE=<parsed>/<total>
#
# line. A helper, NOT wired into the gate. Run from the repo root:
#     bash ci/pocketparse-ts-coverage.sh
set -uo pipefail

REPO="${CLAUDE_PROJECT_DIR:-$(cd "$(dirname "$0")/.." && pwd)}"
PP_DIR="$REPO/tools/pocketparse"
CORPUS="$PP_DIR/reference/tree-sitter-typescript/test/corpus"

platform() {
  case "$(uname -s)/$(uname -m)" in
    Linux/x86_64) echo linux-x86_64 ;;
    Darwin/arm64|Darwin/aarch64) echo macos-arm64 ;;
    *) echo linux-x86_64 ;;
  esac
}
PLAT="$(platform)"
PP="$PP_DIR/dist/$PLAT/pocketparse"

if [ ! -x "$PP" ]; then
  ( cd "$PP_DIR" && bash build.sh dist ) >/dev/null 2>&1
fi
if [ ! -x "$PP" ]; then
  echo "FATAL: could not build/find pocketparse at $PP"
  echo "TS_COVERAGE=0/0"
  exit 1
fi

if [ ! -d "$CORPUS" ]; then
  echo "FATAL: corpus not found at $CORPUS"
  echo "TS_COVERAGE=0/0"
  exit 1
fi

tmp="$(mktemp -d)"
trap 'rm -rf "$tmp"' EXIT

# Split the corpus into per-case input files plus a manifest carrying the dialect
# each case's :language(...) directive selects (tsx, else typescript).
python3 - "$CORPUS" "$tmp" <<'PY'
import sys, os, re, glob
corpus, out = sys.argv[1], sys.argv[2]
idx = 0
manifest = open(os.path.join(out, "cases.tsv"), "w")
HEADER = re.compile(r'(?ms)^={3,}[ \t]*\n(.*?)\n={3,}[ \t]*\n')
SEP = re.compile(r'(?m)^-{3,}[ \t]*\n')
for path in sorted(glob.glob(os.path.join(corpus, "*.txt"))):
    txt = open(path, encoding="utf-8").read()
    base = os.path.basename(path).replace(".txt", "")
    parts = HEADER.split(txt)
    for i in range(1, len(parts), 2):
        header = parts[i]
        title = header.splitlines()[0].strip() if header.strip() else ""
        body = parts[i+1]
        m = SEP.search(body)
        if not m:
            continue
        dm = re.search(r':language\(([a-z]+)\)', header)
        dialect = dm.group(1) if dm else "typescript"
        if dialect not in ("typescript", "tsx"):
            dialect = "typescript"
        ext = "tsx" if dialect == "tsx" else "ts"
        inp = body[:m.start()]
        if inp.startswith("\n"):
            inp = inp[1:]
        inp = inp.rstrip("\n") + "\n"
        fn = os.path.join(out, f"case_{idx:04d}.{ext}")
        with open(fn, "w", encoding="utf-8") as f:
            f.write(inp)
        manifest.write(f"{idx:04d}\t{ext}\t{dialect}\t{base}\t{title}\n")
        idx += 1
manifest.close()
print(idx)
PY

total=0
parsed=0
fails=()
while IFS=$'\t' read -r id ext dialect base title; do
  total=$((total + 1))
  f="$tmp/case_${id}.${ext}"
  out="$("$PP" run "std.${dialect}" --input "$f" --dump-tree 2>/dev/null)"
  rc=$?
  if [ "$rc" -eq 0 ] && ! printf '%s' "$out" | grep -qE 'ERROR|MISSING'; then
    parsed=$((parsed + 1))
  else
    fails+=("[$base/$dialect] $title")
  fi
done < "$tmp/cases.tsv"

if [ "${#fails[@]}" -gt 0 ]; then
  echo "== failing cases (${#fails[@]}) =="
  for t in "${fails[@]}"; do
    echo "  FAIL $t"
  done
fi

echo "TS_COVERAGE=$parsed/$total"
