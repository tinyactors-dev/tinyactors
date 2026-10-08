# Language bindings

Each binding lives in `bindings/<lang>/` with its own `mise.toml` pinning that
language's toolchain (mise merges it over the root config), so core
contributors never download a JVM or .NET SDK.

| Directory | Toolchain (own mise.toml) | Links | Datamodel typically |
|-----------|---------------------------|-------|---------------------|
| `python/` | python, uv | amalgamation via setuptools/CFFI | Python objects (`PyObject *` behind `ta_payload`) |
| `ruby/` | ruby | amalgamation, native ext | `VALUE` |
| `node/` ✅ | node | N-API (C API) addon, xstate-style API + index.d.ts | napi values (context/events as napi refs; readable expressions resolved to functions per chart); one native system per `System` |
| `rust/` | rust | `cc` crate on the amalgamation, `-sys` + safe crate | Rust closures / enum values |
| `go/` | go | cgo on the amalgamation | handle table (cgo forbids Go pointers in C) |
| `java/` | java (+ gradle) | JNI or Panama FFM | handle table |
| `dotnet/` | dotnet | P/Invoke | `GCHandle` |
| `swift/` | swift (macOS host) | SwiftPM C target | Swift values |
| `kotlin/` | java, android-ndk | JNI via NDK CMake | handle table |
| `wasm/` | node, wasmtime | wasm32 freestanding module | host JS/WASI |
| `sqlite/` | — | loadable extension, amalgamation | SQL values / row ids |
| `postgres/` | postgres dev headers | extension, amalgamation | `Datum` |

Rules for every binding:

1. Never re-implement SCXML semantics, queues, routing, timers or invoke
   lifecycle. If a binding needs it, it belongs in `ta_system`.
2. Drive the system with `send_many` → `run(budget)` → `effects` → `effects_ack`.
   Integrate `next_deadline` with the host event loop.
3. Copy or retain anything taken from an effect before the next mutating call.
4. Datamodel vtables are the only callbacks; they must not call `ta_system_*`.

## JavaScript binding seam

`shared/binding.c` implements the procedural backend used by `shared/core.mjs`:
both datamodel vtables, payload/instance ownership, compilation, effect collection,
router peers, hibernation, snapshots, migration and stats. Add host operations here
once. Both builds compile this exact C source; neither adapter contains actor logic.

The value seam is the synchronous subset of Node-API already used by the native
binding. `node/src/addon.c` registers the shared core against Node's real API.
`web/src/node_api.h` declares the subset as wasm imports; `web/native.mjs` supplies
objects, properties, callbacks, exceptions, strong references and byte buffers.
`web/src/glue.c` only exports registration, callback/finalizer trampolines and
allocation. This avoids another value vtable and leaves Node's native call path
unchanged. It is not a general Node-API polyfill (no async work, weak references,
classes, or arbitrary typed arrays).

Wasm values live in a per-call handle scope; strong references outlive scopes.
External handles have GC finalizers. Byte buffers are mirrored into wasm memory
for the call, synchronized at JS callbacks and copied back before return; output
buffers therefore survive memory growth and scope cleanup. Nested calls get their
own scopes. The cost is more wasm/JS crossings than the former actor-specific
imports, in exchange for having no second implementation of binding semantics.

Snapshots remain build/architecture-specific, and a snapshot of an earlier
region layout is refused (ADR 0006 in the repository root changed it). ES
instance and `<foreach>` handles carry a high-word tag over their slot in the
unified table. `node:test` and `web:test` run the same procedural
tests; `web:test` also runs the adapter unit tests in `tests/unit/test_js_adapter.mjs`.
