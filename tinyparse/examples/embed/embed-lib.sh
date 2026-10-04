#!/usr/bin/env bash
# examples/embed/embed-lib.sh -- build an embeddable artifact that exports only
# its intended API (W16-4).
#
# A host that drops generated parsers into its own app should see exactly the
# parsers' public entries (each <prefix>_load / <prefix>_run) plus the public
# tinyparse_* API it calls -- and none of the library's internals: no
# tinyparse-internal tp_* symbol belongs in the exported table.
#
# This is the recipe that delivers that. It compiles, in ONE artifact:
#   - the tinyparse LIBRARY AMALGAMATION (dist/amalgamation/tinyparse.c);
#   - one or more generated parser-lib translation units (each `tinyparse c
#     <grammar> --parser-lib --name <prefix>'), whose own per-rule functions are
#     `static' already (the no-statics gate proves it), leaving only the two
#     <prefix>_load / <prefix>_run entries external;
# and links them behind a LINKER VERSION SCRIPT generated from the prefixes, which
# exports exactly { <prefix>_load, <prefix>_run, ..., tinyparse_* } and localizes
# everything else. The version script is what hides the tinyparse-internal tp_*
# symbols the amalgamation leaves at default visibility (it keeps them default so
# the normal shared library exports its public API; the embedding build wants a
# tighter surface, so it localizes them here). The result: `nm -D' on the shared
# object shows the parser API and the public tinyparse_* API and NOTHING else.
#
# Usage:
#   embed-lib.sh OUT.so PREFIX1:GRAMMAR1 [PREFIX2:GRAMMAR2 ...]
# e.g.
#   embed-lib.sh libembed.so host_parser:host.scm json_parser:std.json
#
# Honours TP_ROOT, TP_INC, TP_CLI (the CLI; build/bin/tinyparse after `mise run
# cli`), TP_AMALGAMATION (dist/amalgamation after `mise run amalgamate`) and CC
# when set; otherwise derives them from its own location and the host platform.
set -uo pipefail

HERE="$(cd "$(dirname "$0")" && pwd)"

platform() {
  case "$(uname -s)/$(uname -m)" in
    Linux/x86_64) echo linux-x86_64 ;;
    Darwin/arm64|Darwin/aarch64) echo macos-arm64 ;;
    *) echo linux-x86_64 ;;
  esac
}
PLAT="$(platform)"

TP_ROOT="${TP_ROOT:-$(cd "$HERE/../.." && pwd)}"
TP_INC="${TP_INC:-$TP_ROOT/include}"
TP_CLI="${TP_CLI:-$TP_ROOT/build/bin/tinyparse}"
SRC="$TP_ROOT/src"
AMALG="${TP_AMALGAMATION:-$TP_ROOT/dist/amalgamation}"
# The generated parser-lib TU #includes tinyparse.h and src/internal.h.
CC="${CC:-cc}"

if [ "$#" -lt 2 ]; then
  echo "usage: $0 OUT.so PREFIX:GRAMMAR [PREFIX:GRAMMAR ...]" >&2
  exit 2
fi
OUT="$1"; shift

# The amalgamation must exist. Build it on demand so the
# recipe is self-contained.
if [ ! -f "$AMALG/tinyparse.c" ]; then
  echo "embed-lib: amalgamation $AMALG/tinyparse.c missing (run mise run amalgamate)" >&2
  exit 1
fi

WORK="$(mktemp -d)"
trap 'rm -rf "$WORK"' EXIT

# 1. Compile the amalgamated runtime once.
"$CC" -std=c11 -O2 -fPIC -c "$AMALG/tinyparse.c" -I"$AMALG" \
  -o "$WORK/tinyparse.o" || exit 1

# 2. Generate and compile each parser-lib TU with its own --name prefix, and
#    collect the exported entries the version script must keep global.
OBJS="$WORK/tinyparse.o"
ENTRIES=""
for spec in "$@"; do
  prefix="${spec%%:*}"
  grammar="${spec#*:}"
  if [ -z "$prefix" ] || [ "$prefix" = "$spec" ]; then
    echo "embed-lib: bad spec '$spec' (want PREFIX:GRAMMAR)" >&2
    exit 2
  fi
  # A grammar argument may be a path or an embedded package name; pass it through
  # as the CLI resolves it.
  gpath="$grammar"
  [ -f "$HERE/$grammar" ] && gpath="$HERE/$grammar"
  "$TP_CLI" c "$gpath" --parser-lib --name "$prefix" \
    --out "$WORK/$prefix.c" || exit 1
  # The generated TU is machine output; compile it like ci/tinyparse-cc-diff.sh
  # does (no -Wall -Wextra), at -O2 -fPIC.
  "$CC" -std=c11 -O2 -fPIC -c "$WORK/$prefix.c" \
    -I"$TP_INC" -I"$SRC" -o "$WORK/$prefix.o" || exit 1
  OBJS="$OBJS $WORK/$prefix.o"
  ENTRIES="$ENTRIES ${prefix}_load; ${prefix}_run;"
done

# 3. The exported-symbol list: export each parser's two entries plus the public
#    tinyparse_* API; localize EVERYTHING else (the tp_* internals). GNU ld
#    spells this a version script; the macOS linker an -exported_symbols_list.
case "$PLAT" in
  macos-*)
    {
      for spec in "$@"; do prefix="${spec%%:*}"; echo "_${prefix}_load"; echo "_${prefix}_run"; done
      echo "_tinyparse_*"
    } > "$WORK/export.syms"
    LINKFLAGS="-Wl,-exported_symbols_list,$WORK/export.syms"
    ;;
  *)
    {
      echo "{"
      echo "  global:"
      for e in $ENTRIES; do echo "    $e"; done
      echo "    tinyparse_*;"
      echo "  local:"
      echo "    *;"
      echo "};"
    } > "$WORK/export.map"
    LINKFLAGS="-Wl,--version-script=$WORK/export.map"
    ;;
esac

# 4. Link the embeddable shared object.
# shellcheck disable=SC2086
"$CC" -shared -fPIC $LINKFLAGS -o "$OUT" $OBJS || exit 1

echo "embed-lib: wrote $OUT (parser entries +$(echo "$ENTRIES" | wc -w | tr -d ' ')/2 +tinyparse_*; internals hidden)"
