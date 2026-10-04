#!/usr/bin/env bash
# examples/jit/build.sh — compile the JIT driver against the host dist archives.
#
# Invoked by examples/build.sh with TP_ROOT/TP_DIST/TP_INC/TP_CLI in the
# environment. It also works when run directly: the variables default from this
# script's own location.
set -uo pipefail

HERE="$(cd "$(dirname "$0")" && pwd)"
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

"$CC" -std=c11 -O2 -Wall -Wextra \
  -I "$TP_DIST" \
  -o "$HERE/jit" \
  "$HERE/jit.c" \
  "$TP_DIST/libtinyparse.a"
