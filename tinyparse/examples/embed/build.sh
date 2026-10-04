#!/usr/bin/env bash
# examples/embed/build.sh -- the W16-3 multi-parser example.
#
# Generates TWO parser-lib translation units from TWO grammars, each with its
# OWN --name symbol prefix so their symbols do not collide, then links both with
# the embed.c driver into ONE program. This is the proof that the generator emits
# independent, non-colliding parsers that coexist in one binary.
#
# Invoked by examples/build.sh with PP_ROOT/PP_DIST/PP_INC/PP_CLI in the
# environment, or run directly: it falls back to deriving those from its own
# location and the host platform.
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

# PP_ROOT is the pocketparse project root; the generated parser-lib TUs
# #include "internal.h" (in src/).
PP_ROOT="${PP_ROOT:-$(cd "$HERE/../.." && pwd)}"
SRC="$PP_ROOT/src"
case "$(uname -s)/$(uname -m)" in
  Linux/x86_64) PLAT=linux-x86_64 ;;
  Darwin/arm64|Darwin/aarch64) PLAT=macos-arm64 ;;
  *) PLAT=linux-x86_64 ;;
esac

CC="${CC:-cc}"

# 1. Generate the two parsers, each with a DISTINCT --name prefix. Without the
#    prefixes their exported entries (pp_compiled_parser_load/_run) and every
#    emitted file-scope symbol would collide when linked into one program.
"$PP_CLI" c "$HERE/host.scm" --parser-lib --name host_parser \
  --out "$HERE/host_parser.c" || exit 1
"$PP_CLI" c std.json --parser-lib --name json_parser \
  --out "$HERE/json_parser.c" || exit 1

# 2. Compile and link the driver with BOTH generated parsers and the shared
#    runtime (libpocketparse is ONE shared archive, linked once). Two
#    parser TUs + the shared library link cleanly with no duplicate-definition.
#    The generated parser TUs are machine output (and #include the engine's
#    internal.h), so they are compiled at -O2 without -Wall -Wextra, exactly as
#    ci/pocketparse-cc-diff.sh compiles them; the hand-written driver embed.c is
#    held to -Wall -Wextra on its own.
"$CC" -std=c11 -O2 -Wall -Wextra -c -o "$HERE/embed.o" \
  "$HERE/embed.c" -I"$PP_INC" || exit 1
"$CC" -std=c11 -O2 -o "$HERE/embed" \
  "$HERE/embed.o" "$HERE/host_parser.c" "$HERE/json_parser.c" \
  -I"$PP_INC" -I"$SRC" \
  "$PP_DIST/libpocketparse.a"
