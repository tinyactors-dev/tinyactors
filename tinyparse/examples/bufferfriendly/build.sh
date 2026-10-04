#!/usr/bin/env bash
# examples/bufferfriendly/build.sh -- the W17-4 BUFFER-FRIENDLY example.
#
# Generates the sexpr parser as a parser-lib translation unit (--name sx) and links
# it with the bufferfriendly.c driver, which drives the GENERATED native parser over
# a growing buffer via the POCKETPARSE_MORE (NEED_MORE_INPUT) signal. The grammar's
# start rule is a repeat of top-level items, the shallow boundary the generated
# parser suspends at.
#
# Invoked by examples/build.sh with PP_ROOT/PP_DIST/PP_INC/PP_CLI in the
# environment, or run directly: it derives those from its own location.
set -uo pipefail

HERE="$(cd "$(dirname "$0")" && pwd)"

if [ -z "${PP_DIST:-}" ]; then
  PP_ROOT="$(cd "$HERE/../.." && pwd)"
  case "$(uname -s)/$(uname -m)" in
    Linux/x86_64) PLAT=linux-x86_64 ;;
    Darwin/arm64|Darwin/aarch64) PLAT=macos-arm64 ;;
    *) PLAT=linux-x86_64 ;;
  esac
  PP_DIST="$PP_ROOT/dist/$PLAT"
  PP_INC="$PP_ROOT/include"
  PP_CLI="$PP_DIST/pocketparse"
fi

PP_ROOT="${PP_ROOT:-$(cd "$HERE/../.." && pwd)}"
SRC="$PP_ROOT/src"
case "$(uname -s)/$(uname -m)" in
  Linux/x86_64) PLAT=linux-x86_64 ;;
  Darwin/arm64|Darwin/aarch64) PLAT=macos-arm64 ;;
  *) PLAT=linux-x86_64 ;;
esac

CC="${CC:-cc}"

# 1. Generate the sexpr parser as a parser-lib TU with the --name prefix `sx'. The
#    generated file carries sx_load / sx_run (whole-input) and sx_run_chunk
#    (buffer-friendly), because the start rule is a top-level-item loop.
"$PP_CLI" c "$HERE/sexpr.scm" --parser-lib --name sx \
  --out "$HERE/sx.c" || exit 1

# 2. Compile the hand-written driver under -Wall -Wextra; the generated parser TU is
#    machine output (and #includes the engine's internal.h), so it builds at -O2
#    without -Wall -Wextra, exactly as the cc-diff / nesting gates do.
"$CC" -std=c11 -O2 -Wall -Wextra -c -o "$HERE/bufferfriendly.o" \
  "$HERE/bufferfriendly.c" -I"$PP_INC" -I"$SRC" || exit 1
"$CC" -std=c11 -O2 -o "$HERE/bufferfriendly" \
  "$HERE/bufferfriendly.o" "$HERE/sx.c" \
  -I"$PP_INC" -I"$SRC" \
  "$PP_DIST/libpocketparse.a"
