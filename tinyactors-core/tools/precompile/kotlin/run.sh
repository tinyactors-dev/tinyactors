#!/usr/bin/env bash
# Build the Kotlin order example (generateCharts writes OrderSession from
# order.scxml), its JNI bridge, and run it.
set -euo pipefail
here="$(cd "$(dirname "$0")" && pwd)"
core="$(cd "$here/../../.." && pwd)"
if [ ! -x "$core/build/release/bin/ta-precompile-table" ]; then
    (cd "$core" && mise run build --release)
fi
java_home="${JAVA_HOME:-$(dirname "$(dirname "$(readlink -f "$(command -v javac)")")")}"
cc="$(cd "$core" && mise which clang 2>/dev/null || command -v cc)"
mkdir -p "$here/build"
# core's release libraries are not position-independent: the layers the
# bridge uses are compiled into the shared library (mise/core.rb lists them)
sources=()
for s in base/{address,cbor,output,format,arena,pool,heap,slab,journal,status,table,growable_arena} \
         image/{image,event_names} trace/{trace,record} core/{core,event_pool,trace_step,datamodel_null} \
         system/{system,schedule,route,hibernate,effects,tracing,policy} otlp/{otlp,decode} \
         host/{host,telemetry,request,definitions,session_map} compile/{compile_support,xml,scxml,link,compile,write} \
         runtime/{runtime,runtime_hosted} datamodel_precompiled/datamodel_precompiled; do
    sources+=("$core/src/$s.c")
done
"$cc" -std=c11 -D_POSIX_C_SOURCE=200809L -O2 -DNDEBUG -fPIC -shared -w \
    -I"$java_home/include" -I"$java_home/include/linux" -I"$core/include" -I"$core/src" \
    "$here/native/bridge.c" "${sources[@]}" -lm -o "$here/build/libtinyactors_kotlin.so"
(cd "$here" && gradle -q --console=plain installDist 2>&1 | { grep -v "^Picked up" >&2 || true; })
JAVA_OPTS="-Dtinyactors.lib=$here/build/libtinyactors_kotlin.so" \
    exec "$here/build/install/tinyactors-precompiled/bin/tinyactors-precompiled" "$here/src/main/scxml/order/order.scxml" \
    2> >(grep -v '^Picked up' >&2)
