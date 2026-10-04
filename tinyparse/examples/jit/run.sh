#!/usr/bin/env bash
# examples/jit/run.sh '<C source>' <function> <int-arg>
#
# Builds the driver if needed, then JITs the named function from the C source,
# calls it with the integer argument, and prints the integer result.
#
# Designed to run with cwd = tinyparse/ (as the test harness does) but
# also works from anywhere: paths default from this script's own location.
set -uo pipefail

HERE="$(cd "$(dirname "$0")" && pwd)"
: "${PP_ROOT:=$(cd "$HERE/../.." && pwd)}"

platform() {
  case "$(uname -s)/$(uname -m)" in
    Linux/x86_64) echo linux-x86_64 ;;
    Darwin/arm64|Darwin/aarch64) echo macos-arm64 ;;
    *) echo linux-x86_64 ;;
  esac
}
: "${PP_DIST:=$PP_ROOT/dist/$(platform)}"

if [ ! -x "$HERE/jit" ] || [ "$HERE/jit.c" -nt "$HERE/jit" ]; then
  PP_ROOT="$PP_ROOT" PP_DIST="$PP_DIST" bash "$HERE/build.sh" || exit 2
fi

if [ "$#" -lt 3 ]; then
  echo "usage: run.sh '<C source>' <function> <int-arg>" >&2
  exit 2
fi

JIT_GRAMMAR="$HERE/jit.scm" exec "$HERE/jit" "$1" "$2" "$3"
