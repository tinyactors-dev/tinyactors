#!/usr/bin/env bash
# examples/nesting/build.sh -- the W17-3 NATIVE NESTING example.
#
# Generates THREE parser-lib translation units -- the std.fence host and two
# embedded languages (std.json, std.javascript) -- each with its OWN --name symbol
# prefix, then links all three with the nesting.c driver into ONE program. The
# driver registers the two embedded GENERATED parsers on the engine's language
# registry; the GENERATED host parser's baked inject sites resolve them by name at
# runtime and drive them over each fence's content. This is the proof that a
# generated host parser nests generated embedded parsers, coordinated by the
# engine's language registry.
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

# 1. Generate the three parsers, each with a DISTINCT --name prefix. The host
#    (std.fence) NAMES std.json and std.javascript in its inject forms but is not
#    linked against their parsers -- the driver wires them at runtime.
"$PP_CLI" c std.fence --parser-lib --name fence_host \
  --out "$HERE/fence_host.c" || exit 1
"$PP_CLI" c std.json --parser-lib --name json_lang \
  --out "$HERE/json_lang.c" || exit 1
"$PP_CLI" c std.javascript --parser-lib --name js_lang \
  --out "$HERE/js_lang.c" || exit 1

# 2. Compile the hand-written driver under -Wall -Wextra; the generated parser TUs
#    are machine output (and #include the engine's internal.h), so they build at
#    -O2 without -Wall -Wextra, exactly as the cc-diff / multiparser gates do.
"$CC" -std=c11 -O2 -Wall -Wextra -c -o "$HERE/nesting.o" \
  "$HERE/nesting.c" -I"$PP_INC" || exit 1
"$CC" -std=c11 -O2 -o "$HERE/nesting" \
  "$HERE/nesting.o" "$HERE/fence_host.c" "$HERE/json_lang.c" "$HERE/js_lang.c" \
  -I"$PP_INC" -I"$SRC" \
  "$PP_DIST/libpocketparse.a"
