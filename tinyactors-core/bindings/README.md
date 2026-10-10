# Language bindings

Each binding lives in `bindings/<lang>/` with its own `mise.toml` pinning that
language's toolchain (mise merges it over the root config), so core
contributors never download a JVM or .NET SDK.

| Directory | Toolchain (own mise.toml) | Links | Datamodel typically |
|-----------|---------------------------|-------|---------------------|
| `python/` | python, uv | amalgamation via setuptools/CFFI | Python objects (`PyObject *` behind `ta_payload`) |
| `ruby/` | ruby | amalgamation, native ext | `VALUE` |
| `node/` ✅ | node | N-API (C API) addon, the host's API + index.d.ts | napi values (variables and event data as napi refs; `c.expr` functions and expression text resolved to functions per chart), or the C ECMAScript datamodel; one `ta_host` per `System` |
| `rust/` | rust | `cc` crate on the amalgamation, `-sys` + safe crate | Rust closures / enum values |
| `go/` | go | cgo on the amalgamation | handle table (cgo forbids Go pointers in C) |
| `java/` | java (+ gradle) | JNI or Panama FFM | handle table |
| `dotnet/` | dotnet | P/Invoke | `GCHandle` |
| `swift/` | swift (macOS host) | SwiftPM C target | Swift values |
| `kotlin/` | java, android-ndk | JNI via NDK CMake | handle table |
| `wasm/` | node, wasmtime | wasm32 freestanding module | host JS/WASI |
| `sqlite/` | — | loadable extension, amalgamation | SQL values / row ids |
| `postgres/` | postgres dev headers | extension, amalgamation | `Datum` |

## Designing a binding's API: expose the core, nothing else

A binding exposes what the core already does. It is not a port of
another library, and it is not where features are designed. The
JavaScript binding grew to 3,500 lines by getting this backwards: it
started from xstate's API and emulated it on top of SCXML, reimplemented
C in JavaScript, and left out what the core offers (I/O processors,
external invokes). Do it in this order:

1. **Inventory the headers first.** List the public functions and effects
   of `include/tinyactors/*.h` that the binding will drive (usually
   `host.h`, `image.h`, `compile.h`, `address.h`, `frame.h`, `trace.h`,
   `otlp.h`).
2. **Write the mapping table before any code or prose design:** each
   public API item in the binding's language next to the core functions,
   effects and flags it calls. An API item with no core function behind
   it does not belong in the binding. A core function with no API item is
   either used internally by the binding (say where) or listed as not
   exposed (say why).
3. **Borrow shape, never semantics.** Another library (xstate for
   JavaScript) may suggest names and call shapes: `createActor`, `send`,
   `getSnapshot`, `subscribe`. What a call does is what the core function
   behind it does. Never emulate the other library's behaviour, rename
   the core's events, or lower its concepts (`after`, `always`, `reenter`)
   onto SCXML. Do not argue over which library's model is right: the
   mapping table decides.
4. **The core is a complete SCXML implementation; read it before
   claiming a gap.** `src/core` and `src/system` implement SCXML's
   semantics, including I/O processors (`<send type>`), external invokes
   (`<invoke type>`), sends to invoked services (`#_<invokeid>`), and
   error events. Before a design says "the core cannot do X", find where
   SCXML (`docs/SCXML.md`) and the core handle X, and cite the lines.
5. **A binding invents nothing:** no event names, flags, fields or
   mechanisms of its own, and no core change to make an API look like
   another library. A real gap in the core is a separate proposal for the
   core, not part of a binding's design.
6. **Narrow and deep.** The native side owns the host block, the run loop,
   the datamodel and effect delivery, and makes few, large calls. The
   language side keeps only what needs that language: its values, its
   promises and streams (stores, transports, I/O processors, invokers),
   its exporters. Bindings are written by hand; the mapping table, not a
   generator, keeps them complete.

Rules for every binding:

1. Never re-implement SCXML semantics, queues, routing, timers or invoke
   lifecycle. If a binding needs it, it belongs in `ta_system`. Never keep
   a twin in the binding's language of anything C already does
   (addresses, frames, wire formats, trace presets): bind the C.
2. Drive the system with `send_many` → `run(budget)` → `effects` → `effects_ack`.
   Integrate `next_deadline` with the host event loop.
3. Copy or retain anything taken from an effect before the next mutating call.
4. Datamodel vtables are the only callbacks; they must not call `ta_system_*`.

## The JavaScript bindings

`node/` and `web/` give JavaScript the same API (`core.mjs` in each, the
same `index.d.ts`) and share no code: each drives the core its own way.

**Node.js** (`node/`): `src/binding.c` is the native side of `core.mjs`:
hand-written wrappers of the pure functions of the public headers
(compile, images, addresses, frames, traceparents, OTLP), and a `System`
over `ta_host` with its datamodel (the native `ecmascript` vtable, values
as Node-API references, or the C one on quickjs-ng behind a thin
forwarding vtable), its run loop and effect delivery: one call per pump
hands JavaScript every effect, the sessions whose variables changed and
the OTLP bytes. `src/addon.c` registers it with Node.

**Browsers** (`web/`): no C of its own. `tinyactors.wasm` is the release's
three libraries (`tinyactors`, `tinyactors_runtime`,
`tinyactors_datamodel_ecmascript`) linked into one module as the release
links its modules, importing nothing, exporting the public API and a
function table (`mise run web:build`). `native.mjs` gives `core.mjs` the
same calls `binding.c` gives Node's, written in JavaScript over that API:

- structs are read and written at the wasm32 offsets of `api.mjs`, which
  web:build generates from the API descriptions; a struct passed by value
  is a pointer to a copy, one returned by value is written through a
  pointer given first (the wasm32 C ABI);
- what it hands the libraries lives in the module's memory
  (`ta_wasm_alloc`): scratch released after each call, a system's blocks
  until it is destroyed, an image's until nothing holds it;
- the JavaScript `ecmascript` datamodel's vtable entries are JavaScript
  functions, made wasm functions by a small module generated at load and
  placed in the function table: the core calls them directly, with
  integer handles for sessions and payloads;
- the quickjs datamodel is the engine's own vtable, with the few entries
  the binding needs to see (sessions created, destroyed, bound to an
  event) in front of the engine's.

Hibernation and export blobs of the native datamodels hold neutral values,
so they move between the two bindings. Snapshots remain build-specific,
and a snapshot of an earlier region layout is refused (ADR 0006 in the
repository root changed it). `node:test` runs the suites against the
addon, `web:test` the same suites against the web binding.
