#!/usr/bin/env bash
# examples/uri/build.sh -- compile the URI parser driver.
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
fi

CC="${CC:-cc}"
"$CC" -std=c99 -O2 -Wall -Wextra -o "$HERE/uri" \
  "$HERE/uri.c" \
  -I"$PP_DIST" \
  "$PP_DIST/libpocketparse.a"
