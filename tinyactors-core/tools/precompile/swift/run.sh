#!/usr/bin/env bash
# Build the Swift order example (its plugin generates OrderSession from
# order.scxml) and run it.
#   SWIFT_TOOLCHAIN=/path/to/swift-6.x/usr/bin ./run.sh
set -euo pipefail
here="$(cd "$(dirname "$0")" && pwd)"
core="$(cd "$here/../../.." && pwd)"
toolchain="${SWIFT_TOOLCHAIN:-/root/swift-toolchain/swift-6.1.2-RELEASE-ubuntu24.04/usr/bin}"
export PATH="$toolchain:$PATH"
if [ ! -x "$core/build/release/bin/ta-precompile-table" ]; then
    (cd "$core" && mise run build --release)
fi
cd "$here"
swift build -c release --product Order >&2
exec "$here/.build/release/Order"
