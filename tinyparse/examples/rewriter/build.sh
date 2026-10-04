#!/usr/bin/env bash
# build.sh — compile the #define -> enum rewriter against the host dist.
#
# Invoked two ways, both supported:
#   * by examples/build.sh, which exports TP_ROOT, TP_DIST, TP_INC, TP_CLI;
#   * directly (cd examples/rewriter && bash build.sh), in which case we
#     default those variables from this script's own location.
set -uo pipefail

HERE="$(cd "$(dirname "$0")" && pwd)"

# Default the dispatcher's environment when run standalone.
: "${TP_ROOT:=$(cd "$HERE/../.." && pwd)}"
platform() {
  case "$(uname -s)/$(uname -m)" in
    Linux/x86_64) echo linux-x86_64 ;;
    Darwin/arm64|Darwin/aarch64) echo macos-arm64 ;;
    *) echo linux-x86_64 ;;
  esac
}
: "${TP_DIST:=$TP_ROOT/dist/$(platform)}"

CC="${CC:-cc}"

if [ ! -f "$TP_DIST/tinyparse.h" ] || [ ! -f "$TP_DIST/libtinyparse.a" ]; then
  echo "rewriter: missing dist at $TP_DIST (run: cd tinyparse && bash build.sh dist)" >&2
  exit 1
fi

# Embed the grammar so the program needs no .scm path at runtime. Turn the
# .scm bytes into a C array in cdefines_scm.h.
gen_header() {
  local scm="$HERE/cdefines.scm"
  local out="$HERE/cdefines_scm.h"
  {
    echo "/* cdefines_scm.h — generated from cdefines.scm by build.sh. */"
    echo "#ifndef CDEFINES_SCM_H"
    echo "#define CDEFINES_SCM_H"
    echo "#include <stddef.h>"
    echo "#include <stdint.h>"
    echo "static const uint8_t cdefines_scm[] = {"
    od -An -v -tu1 "$scm" | awk '{ for (i = 1; i <= NF; i++) printf "%s,", $i; print "" }'
    echo "};"
    echo "static const size_t cdefines_scm_length = sizeof cdefines_scm;"
    echo "#endif"
  } > "$out"
}
gen_header

"$CC" -std=c99 -O2 -Wall -Wextra -pedantic \
  -I"$TP_DIST" -I"$HERE" \
  -o "$HERE/rewriter" \
  "$HERE/rewriter.c" \
  "$TP_DIST/libtinyparse.a"

echo "rewriter: built $HERE/rewriter"
