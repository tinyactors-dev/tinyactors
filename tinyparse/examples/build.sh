#!/usr/bin/env bash
# examples/build.sh — the example dispatcher.
#
# build.sh::build_examples calls this. It builds each example that ships its own
# examples/<name>/build.sh, in isolation, and is TOLERANT: one example failing
# to build never aborts the others or the metric (the failing example's tests
# simply stay red/xfail).
#
# Each example/<name>/build.sh is invoked with these environment variables:
#   TP_ROOT   absolute path of the tinyparse project root
#   TP_DIST   absolute path of the host-platform dist dir (libtinyparse.a and
#             tinyparse.h live here)
#   TP_INC    absolute path of the tinyparse include directory
#   TP_CLI    absolute path of the built tinyparse CLI
# An example links the host dist archives and includes $TP_DIST/tinyparse.h.
set -uo pipefail

HERE="$(cd "$(dirname "$0")" && pwd)"
export TP_ROOT="$HERE/.."
export TP_INC="$TP_ROOT/include"

platform() {
  case "$(uname -s)/$(uname -m)" in
    Linux/x86_64) echo linux-x86_64 ;;
    Darwin/arm64|Darwin/aarch64) echo macos-arm64 ;;
    *) echo linux-x86_64 ;;
  esac
}
export TP_DIST="$TP_ROOT/dist/$(platform)"
export TP_CLI="$TP_DIST/tinyparse"

rc=0
for d in "$HERE"/*/; do
  [ -x "$d/build.sh" ] || continue
  name="$(basename "$d")"
  if ( cd "$d" && bash build.sh ); then
    printf '[tp-examples] built %s\n' "$name" >&2
  else
    printf '[tp-examples] WARN %s failed to build (its tests stay red)\n' "$name" >&2
    rc=1
  fi
done
exit "$rc"
