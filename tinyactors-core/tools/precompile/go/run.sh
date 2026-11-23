#!/usr/bin/env bash
# Generate the Go order example's code from its chart, build it and run it.
#   run.sh
set -euo pipefail
here="$(cd "$(dirname "$0")" && pwd)"
core="$(cd "$here/../../.." && pwd)"
if [ ! -x "$core/build/release/bin/ta-precompile-table" ]; then
  (cd "$core" && mise run build --release)
fi
export TINYACTORS_TABLE="$core/build/release/bin/ta-precompile-table"
export CC="${CC:-$(cd "$core" && mise which clang 2>/dev/null || echo cc)}" CGO_ENABLED=1
cd "$here/example/order"
go generate
# -a: bridge.c includes ../../demo/demo.c, which go's build cache does not see
go build -a -o "$here/build/order" .
exec "$here/build/order"
