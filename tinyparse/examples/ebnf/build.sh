#!/usr/bin/env bash
# examples/ebnf/build.sh -- compile the EBNF->scm converter.
#
# Invoked by examples/build.sh with TP_ROOT/TP_DIST/TP_INC/TP_CLI in the
# environment, or run directly: it falls back to deriving those from its own
# location and the host platform.
set -uo pipefail

HERE="$(cd "$(dirname "$0")" && pwd)"

if [ -z "${TP_DIST:-}" ]; then
  TP_ROOT="$(cd "$HERE/../.." && pwd)"
  case "$(uname -s)/$(uname -m)" in
    Linux/x86_64) PLAT=linux-x86_64 ;;
    Darwin/arm64|Darwin/aarch64) PLAT=macos-arm64 ;;
    *) PLAT=linux-x86_64 ;;
  esac
  TP_DIST="$TP_ROOT/dist/$PLAT"
fi

CC="${CC:-cc}"
"$CC" -std=c99 -O2 -Wall -Wextra -o "$HERE/ebnf2scm" \
  "$HERE/ebnf2scm.c" \
  -I"$TP_DIST" \
  "$TP_DIST/libtinyparse.a"
