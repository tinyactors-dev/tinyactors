#!/usr/bin/env bash
# Build the Swift side and run the market tracker and the order, each with
# json charts and with staticjs charts.
#   SWIFT_TOOLCHAIN=/path/to/swift-6.x/usr/bin ./run.sh [market|order] [json|staticjs]
set -euo pipefail
here="$(cd "$(dirname "$0")" && pwd)"
core="$(cd "$here/../.." && pwd)"
toolchain="${SWIFT_TOOLCHAIN:-/root/swift-toolchain/swift-6.1.2-RELEASE-ubuntu24.04/usr/bin}"
export PATH="$toolchain:$PATH"
if [ ! -f "$core/build/release/lib/libtinyactors_datamodel_staticjs.a" ]; then
    (cd "$core" && mise run build --release)
fi
cd "$here/swift"
swift build -c release >&2
for program in ${1:-market order}; do
    for datamodel in ${2:-json staticjs}; do
        "$here/swift/.build/release/${program^}" "$datamodel"
        echo
    done
done
