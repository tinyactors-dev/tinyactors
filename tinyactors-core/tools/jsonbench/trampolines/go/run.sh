#!/usr/bin/env bash
# Build and run the Go trampoline spike: one line per function
# (language, function, ns/call; Go rows add allocations per call).
#   run.sh [-n 1000000] [-runs 5]
set -euo pipefail
here="$(cd "$(dirname "$0")" && pwd)"
core="$(cd "$here/../../../.." && pwd)"
if [ ! -f "$core/build/release/lib/libtinyactors_base.a" ]; then
  (cd "$core" && mise run build --release)
fi
cd "$here"
CC="${CC:-$(mise which clang 2>/dev/null || echo cc)}"
export CC CGO_ENABLED=1
go build -trimpath -o "$here/build/jsonbench-go" .
exec "$here/build/jsonbench-go" "$@"
