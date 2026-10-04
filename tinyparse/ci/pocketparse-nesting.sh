#!/usr/bin/env bash
# ci/pocketparse-nesting.sh — the W17-3 NATIVE NESTING gate.
#
# A GENERATED host parser must be able to nest GENERATED embedded parsers, in one
# binary, coordinated by ax's language registry — and produce the SAME composite
# tree the VM produces. This is the inject hook the codegen blueprint describes:
# the host grammar (std.fence) names embedded languages in its (inject grammar:
# NAME ...) forms but is not linked against their parsers; the host program
# registers each embedded GENERATED parser on the engine with
# pocketparse_register_language(engine, "std.json", {grammar, json_lang_run}), and
# the generated host parser's baked inject sites resolve them by name at runtime.
#
# The gate proves it end to end through examples/nesting:
#   - std.fence       -> the GENERATED host parser (fence_host_*),
#   - std.json        -> a GENERATED embedded parser (json_lang_*),
#   - std.javascript  -> a GENERATED embedded parser (js_lang_*),
# all three compiled with --parser-lib + distinct --name prefixes and linked with
# the nesting.c driver into ONE program. The gate:
#   1. builds the CLI + the host-platform dist (the example links the archives),
#   2. builds the example (which generates all three parsers and links them),
#   3. asserts the example's composite tree is BYTE-IDENTICAL to the VM's
#      `run std.fence --dump-tree' on the same document (the real oracle), and to
#      the committed golden expected.txt,
#   4. asserts the linked binary carries all three parsers' distinct entries and no
#      exported ax_ symbol (the hide-ax property: ax is the runtime, never leaked).
#
# Run it from the repo root:
#     bash ci/pocketparse-nesting.sh
#
# Exit 0 only when the generated host nests the generated embedded parsers and the
# composite tree matches the VM byte for byte.
set -uo pipefail

REPO="${CLAUDE_PROJECT_DIR:-$(cd "$(dirname "$0")/.." && pwd)}"
PP_DIR="$REPO/tools/pocketparse"
EX="$PP_DIR/examples/nesting"
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
AXLIB="$REPO/dist/$PLAT/libax.a"

echo "== pocketparse native-nesting gate (W17-3) =="

# 1. Build the CLI + the library archive the generated parsers link against.
( cd "$PP_DIR" && bash build.sh cli ) 2>&1 | sed 's/^/  /'
if [ ! -x "$PP" ] || [ ! -f "$LIB" ] || [ ! -f "$AXLIB" ]; then
  echo "FATAL: CLI ($PP), libpocketparse ($LIB) or libax ($AXLIB) not built"
  echo "RESULT=0/0"
  exit 1
fi

# 2. Build the example: generate all THREE parsers (distinct --name prefixes) and
#    link them with the driver. A link-time symbol collision fails here.
if ! ( cd "$EX" && rm -f nesting nesting.o fence_host.c json_lang.c js_lang.c && \
       bash build.sh ) 2>&1 | sed 's/^/  /'; then
  echo "FAIL: example build failed (symbol collision or compile error)"
  echo "== nesting: RED =="
  exit 1
fi
if [ ! -x "$EX/nesting" ]; then
  echo "FAIL: example binary $EX/nesting was not produced"
  echo "== nesting: RED =="
  exit 1
fi

# 3a. The composite tree the generated host+embedded parsers build must be
#     BYTE-IDENTICAL to the VM's --dump-tree of the SAME std.fence grammar on the
#     SAME document. This is the oracle: native nesting changes nothing about the CST.
GOT="$("$EX/nesting" "$EX/sample.md")"
RC=$?
if [ "$RC" -ne 0 ]; then
  echo "FAIL: example exited $RC (expected 0)"
  echo "== nesting: RED =="
  exit 1
fi
VM="$(POCKETPARSE_ENGINE=vm-ax "$PP" run std.fence --input "$EX/sample.md" --dump-tree)"
if ! diff -u <(printf '%s\n' "$VM") <(printf '%s\n' "$GOT") ; then
  echo "FAIL: generated nesting tree differs from the VM (native nesting must match)"
  echo "== nesting: RED =="
  exit 1
fi
echo "  generated host nests generated embedded: tree byte-identical to the VM"

# 3b. And to the committed golden, so a silent drift in either is caught.
if ! diff -u "$EX/expected.txt" <(printf '%s\n' "$GOT") ; then
  echo "FAIL: example output does not match the committed expected.txt"
  echo "== nesting: RED =="
  exit 1
fi

# 4. All three parsers' distinct generated entries are present in the linked
#    binary (proof the prefixes disjointed three full parser symbol sets).
if command -v nm >/dev/null 2>&1; then
  SYMS="$(nm "$EX/nesting" 2>/dev/null || true)"
  for s in fence_host_load fence_host_run json_lang_load json_lang_run \
           js_lang_load js_lang_run; do
    if ! printf '%s\n' "$SYMS" | grep -qE " T $s$"; then
      echo "FAIL: expected generated entry '$s' missing from the linked binary"
      echo "== nesting: RED =="
      exit 1
    fi
  done
  echo "  all three parsers' entries present (no prefix collision)"
fi

# 5. The hide-ax property for the nesting set: build a SHARED OBJECT of all three
#    parsers over the amalgamated runtime behind a version script, and assert
#    `nm -D' exports the parser + public pocketparse_* API and ZERO ax_ symbols.
#    A plain static executable always shows ax_ in its full symbol table; the
#    exported surface of a hide-ax artifact is what must be clean, and a host that
#    nests these parsers ships that artifact, not the static executable.
if command -v nm >/dev/null 2>&1; then
  SO="$EX/libnesting.so"
  rm -f "$SO"
  if ! ( cd "$PP_DIR/examples/embed" && \
         bash hide-ax.sh "$SO" \
           fence_host:std.fence json_lang:std.json js_lang:std.javascript ) \
       2>&1 | sed 's/^/  /'; then
    echo "FAIL: hide-ax shared object for the nesting set did not build"
    echo "== nesting: RED =="
    exit 1
  fi
  DYN="$(nm -D "$SO" 2>/dev/null || true)"
  if printf '%s\n' "$DYN" | grep -qE " [TtWwiD] ax_"; then
    echo "FAIL: an ax_ symbol is EXPORTED from the hide-ax shared object"
    printf '%s\n' "$DYN" | grep -E " [TtWwiD] ax_" | head -5 | sed 's/^/      /'
    echo "== nesting: RED =="
    exit 1
  fi
  for s in fence_host_run json_lang_run js_lang_run; do
    if ! printf '%s\n' "$DYN" | grep -qE " T $s$"; then
      echo "FAIL: hide-ax shared object does not export generated entry '$s'"
      echo "== nesting: RED =="
      exit 1
    fi
  done
  rm -f "$SO"
  echo "  hide-ax shared object exports the parsers + pocketparse_* and zero ax_"
fi

echo "RESULT=1/1"
echo "== nesting: GREEN (generated host nests generated embedded, == VM) =="
exit 0
