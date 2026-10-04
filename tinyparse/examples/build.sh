#!/usr/bin/env bash
# examples/build.sh — the example dispatcher.
#
# build.sh::build_examples calls this. It builds each example that ships its own
# examples/<name>/build.sh, in isolation, and is TOLERANT: one example failing
# to build never aborts the others or the metric (the failing example's tests
# simply stay red/xfail).
#
# Each example/<name>/build.sh is invoked with these environment variables:
#   PP_ROOT   absolute path of the pocketparse project root
#   PP_DIST   absolute path of the host-platform dist dir (libpocketparse.a and
#             pocketparse.h live here)
#   PP_INC    absolute path of the pocketparse include directory
#   PP_CLI    absolute path of the built pocketparse CLI
# An example links the host dist archives and includes $PP_DIST/pocketparse.h.
set -uo pipefail

HERE="$(cd "$(dirname "$0")" && pwd)"
export PP_ROOT="$HERE/.."
export PP_INC="$PP_ROOT/include"

platform() {
  case "$(uname -s)/$(uname -m)" in
    Linux/x86_64) echo linux-x86_64 ;;
    Darwin/arm64|Darwin/aarch64) echo macos-arm64 ;;
    *) echo linux-x86_64 ;;
  esac
}
export PP_DIST="$PP_ROOT/dist/$(platform)"
export PP_CLI="$PP_DIST/pocketparse"

rc=0
for d in "$HERE"/*/; do
  [ -x "$d/build.sh" ] || continue
  name="$(basename "$d")"
  if ( cd "$d" && bash build.sh ); then
    printf '[pp-examples] built %s\n' "$name" >&2
  else
    printf '[pp-examples] WARN %s failed to build (its tests stay red)\n' "$name" >&2
    rc=1
  fi
done
exit "$rc"
