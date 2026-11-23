#!/usr/bin/env bash
# Build and run the Kotlin (JVM, JNI) trampoline spike.
#   run.sh [N] [warmup] [runs] [modes]
#   modes: a comma-separated list of array, buffer, bytes, bytes-direct, bytes+check
#   (default: all five). Output: language, function, mode, ns/call.
# JSONBENCH_VERBOSE=1 also prints each call's arguments and result and the bytes
# the JVM allocated per call.
set -euo pipefail
here="$(cd "$(dirname "$0")" && pwd)"
core="$(cd "$here/../../../.." && pwd)"
build="$here/build"
lib="$core/build/release/lib"
if [ ! -f "$lib/libtinyactors_base.a" ]; then
    (cd "$core" && mise run build --release)
fi
java_home="${JAVA_HOME:-$(dirname "$(dirname "$(readlink -f "$(command -v javac)")")")}"
cc="$(cd "$core" && mise which clang 2>/dev/null || command -v cc)"
mkdir -p "$build"
"$cc" -std=c11 -D_POSIX_C_SOURCE=200809L -O2 -fPIC -shared -Wall -Wextra \
    -I"$java_home/include" -I"$java_home/include/linux" -I"$core/include" \
    "$here/native/trampoline.c" "$lib/libtinyactors_base.a" \
    -o "$build/libjsonbench_kotlin.so"
(cd "$here" && gradle -q --console=plain installDist 2>&1 | { grep -v "^Picked up" >&2 || true; })
JAVA_OPTS="-Djsonbench.lib=$build/libjsonbench_kotlin.so -Xss8m" \
    exec "$build/install/jsonbench-kotlin/bin/jsonbench-kotlin" "$@" 2> >(grep -v '^Picked up' >&2)
