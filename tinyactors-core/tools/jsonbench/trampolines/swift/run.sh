#!/usr/bin/env bash
# Build the Swift trampoline spike (release) and time each function:
# one line per function: language, function, ns/call (min of RUNS runs of
# N calls each, after a warm-up of N/10).
#
#   ./run.sh [N] [RUNS]          (defaults: 1000000 5)
#   SWIFT_TOOLCHAIN=/path/to/swift-6.x/usr/bin ./run.sh
#   JSONBENCH_ONLY=swift/addItem JSONBENCH_VERBOSE=1 ./run.sh 10000 1
set -euo pipefail
here="$(cd "$(dirname "$0")" && pwd)"
core="$(cd "$here/../../../.." && pwd)"
toolchain="${SWIFT_TOOLCHAIN:-/root/swift-toolchain/swift-6.1.2-RELEASE-ubuntu24.04/usr/bin}"
export PATH="$toolchain:$PATH"

if [ ! -f "$core/build/release/lib/libtinyactors_base.a" ]; then
    (cd "$core" && mise run build --release)
fi
cd "$here"
swift build -c release >&2
exec "$here/.build/release/JSONBench" "${1:-1000000}" "${2:-5}"
