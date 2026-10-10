# Project layout, build and tooling

Legend: ✅ exists now · ◻ planned (directory/file appears with the
implementation of that layer). Layer dependency rules are in
[architecture.md §1](architecture.md#1-layering-challenged).

```
.
├── mise.toml                  ✅ the tools only this project pins (uv/python for W3C conversion, wasmtime);
│                                 the compiler and binutils are the repository root's
├── mise/
│   ├── core.rb                ✅ the layers (one static lib each, libtinyactors_<layer>.a), the test
│   │                             programs and what they link, the builds (dev: ASan/UBSan · release ·
│   │                             addon: PIC · wasi)
│   └── tasks/                 ✅ Ruby file tasks: build, test, w3c, node:*, web:*, pg:*, check:*, …
├── AGENTS.md                  ✅ rules for contributors/agents
├── sgconfig.yml               ✅ ast-grep project: the naming lint (`mise run lint`)
│
├── include/                   public ABI — the only headers bindings ever see (<tinyactors/system.h>)
│   ├── tinyactors.h           ✅ umbrella for the freestanding ABI
│   └── tinyactors/
│       ├── types.h            ✅ ta_string_view, ta_status, TA_NIL, ta_time, ta_expression, ta_payload, TA_API
│       ├── allocator.h        ✅ ta_allocator: the memory a caller lends the host and the compiler
│       ├── address.h          ✅ tinyactors:// addresses (parse/format)
│       ├── frame.h            ✅ length-prefixed link frames on byte streams
│       ├── image.h            ✅ open/verify image, chart/state/event/expression/data lookup
│       ├── datamodel.h        ✅ datamodel vtable, ta_datamodel_environment queries, system variables,
│       │                         event view, null datamodel
│       ├── system.h           ✅ the embedding ABI: layout/init/run/send/resume/effects/inspect/snapshot
│       ├── core.h             ✅ advanced/unstable bare kernel (step function)
│       ├── compile.h          ✅ compiler API (diagnostics, source resolver, manifests)
│       ├── runtime.h          ✅ OS conveniences (malloc, files, resolver, clock, auto-grow)
│       ├── host.h             ✅ host layer: segments sharing a base URI, routing, versions,
│       │                         migration jobs, sweep, growth, boot, snapshot/restore
│       ├── trace.h            ✅ trace values (IDs, step keys, span IDs, traceparent, generator,
│       │                         sampling) and the trace record schema (ADR 0006)
│       ├── otlp.h             ✅ the OTLP codec: trace records -> OTLP protobuf, and decoding
│       ├── datamodel_ecmascript.h ✅ the ECMAScript datamodel (link libtinyactors_datamodel_ecmascript)
│       └── basic_http.h       ✅ BasicHTTP Event I/O Processor codec
│
├── src/                       internal headers are included path-qualified ("base/pool.h", -Isrc)
│   ├── base/                  freestanding building blocks (§2)
│   │   ├── config.h           ✅ portability macros, TA_ASSERT, ta_pointer_add
│   │   ├── memory.h           ✅ memcpy/memset/memmove builtins, local memcmp (no libc symbols)
│   │   ├── format.h           ✅ decimal numbers and bounded text (ta_format_u64, ta_text)
│   │   ├── arena.[ch]         ✅ bump arena + measure mode
│   │   ├── pool.[ch]          ✅ fixed-stride pool, u32 indices, generation handles, lazy init, copy
│   │   ├── slab.[ch]          ✅ page-based half-step size classes (instances, dynamic strings)
│   │   ├── bitset.h           ✅ u32-word bitsets, range masks, next/previous scans
│   │   ├── index_queue.h      ✅ intrusive index FIFO
│   │   ├── heap.[ch]          ✅ 4-ary (due, sequence) heap with removal
│   │   ├── journal.[ch]       ✅ lossy ring of variable-size records (the trace journal)
│   │   ├── address.c          ✅ the address grammar (include/tinyactors/address.h)
│   │   └── frame.c            ✅ link frames (include/tinyactors/frame.h)
│   │
│   ├── image/                 shared by compiler (writer) and runtime (reader)
│   │   ├── records.h          ✅ on-disk records, bytecode opcodes, validated view
│   │   ├── image.c            ✅ open + full verification (fuzzed), public accessors
│   │   └── event_names.c      ✅ event trie lookup (dynamic names -> name id)
│   │
│   ├── trace/                 freestanding trace values and records (ADR 0006); no state
│   │   ├── trace.c            ✅ span IDs, traceparent, the xoshiro256** generator, ratio sampling
│   │   ├── record.[ch]        ✅ the record schema: reserve/commit, walking, bodies, lists, strings
│   │
│   ├── core/                  the kernel: pure resumable step function (§7)
│   │   ├── core_internal.h    ✅ event record, instance/workspace layouts, phases, step contract
│   │   ├── core.c             ✅ phases, selection, conflicts, exit/entry, history, bytecode VM, invoke
│   │   ├── event_pool.c       ✅ refcounted slab strings, event records, event views
│   │   ├── trace_step.c       ✅ the context pool, step records, a step's context for its events
│   │   └── datamodel_null.c   ✅ the SCXML null datamodel
│   │
│   ├── system/                actor VM (§10–§14)
│   │   ├── system_internal.h  ✅ region header, actor/timer/request/effect records
│   │   ├── system.c           ✅ layout/init (one carve function), spawn/reclaim, relocate,
│   │   │                         snapshot/restore, inspection
│   │   ├── schedule.c         ✅ run loop, budgets/quantum, mailbox delivery, inbound send
│   │   ├── route.c            ✅ SCXML I/O processor routing, timers/<cancel>, invoke/done.invoke,
│   │   │                         autoforward, requests, pending spawns
│   │   ├── hibernate.c        ✅ host-driven hibernation: blobs, wake queue → TA_EFFECT_WAKE, CLOCK sweep
│   │   ├── tracing.c          ✅ starting and sampling traces, lifecycle records, the drain API
│   │   └── effects.c          ✅ bounded outbox, lazily materialized effect views
│   │
│   ├── compile/               hosted; allocates through ta_allocator
│   │   ├── compile_internal.h ✅ the compiler's shared state (ta_compiler)
│   │   ├── compile_support.[ch] ✅ chunk arena, vectors, string map, diagnostics, default allocator
│   │   ├── xml.[ch]           ✅ minimal non-validating XML parser (namespaces, CDATA, entities)
│   │   ├── write.c            ✅ ta_xml_writer: XML from calls, for charts a binding builds
│   │   ├── scxml.c            ✅ SCXML -> records: states, transitions, domains, bytecode, sends,
│   │   │                         invokes, data, delays
│   │   ├── link.c             ✅ string interning, chart queue, event trie, image emission
│   │   └── compile.c          ✅ public API, manifests, self-verification
│   │
│   ├── runtime/
│   │   └── runtime.c          ✅ malloc allocator, files + resolver, clock, grow-by-relocation
│   │
│   ├── host/                  the policy every embedder would rebuild, once (freestanding; its caller's ta_allocator)
│   │   ├── host_internal.h    ✅ host state shared by host.c and router.c
│   │   ├── host.c             ✅ segments + session ranges, cross-segment routing, versions and
│   │   │                         retirement, quantum-bounded migration jobs, CLOCK sweep, growth,
│   │   │                         boot, whole-host snapshot/restore
│   │   └── router.c           ✅ envelopes, peers, routes, deduplication (docs/protocol.md)
│   │
│   ├── otlp/                  the OTLP codec (freestanding, one block, relocatable; uses trace + image)
│   │   ├── otlp_internal.h    ✅ the encoder's block, protobuf primitives
│   │   ├── otlp.c             ✅ records -> spans, span events and log records; staging; scopes
│   │   └── decode.c           ✅ OTLP protobuf -> span, attribute, value, event and link views
│   │
│   ├── datamodel_ecmascript/  SCXML ECMAScript datamodel on quickjs-ng; link it like a
│   │   └── datamodel_ecmascript.c ✅ tree-sitter language (include/tinyactors/datamodel_ecmascript.h); no I/O
│   │
│   └── basic_http/            BasicHTTP Event I/O Processor as a transport-agnostic codec
│       └── basic_http.c       ✅ (include/tinyactors/basic_http.h); freestanding
│
├── third_party/
│   └── quickjs-ng/            ✅ pinned engine sources (mise run vendor-quickjs; no quickjs-libc)
│
├── ext/                       optional, separately linked; never required by the core
│   └── datamodel_cgen/        ◻ "C switch" datamodel: expressions compiled to C
│
├── tools/
│   ├── w3c/convert.py         ✅ instantiates the IRP templates (XSLT 2.0, SaxonC via uv)
│   ├── lint/rules/            ✅ ast-grep rules (no-retired-abbreviations: the naming lint)
│   ├── torture/               ✅ torture-bench: scenario host (bench.c: systems, router, the
│   │   │                         test runtime's `system` I/O processor, scenario switching,
│   │   │                         the PostgreSQL service probe/starter), HTTP+WebSocket server
│   │   │                         (net.c), PostgreSQL wire client (postgres.c), scenario engine
│   │   │                         (prelude.js)
│   │   ├── scenarios/         ✅ throughput · tracing · crowd · heavy-data · federation · chaos · capacity ·
│   │   │                         campaign · fit · hibernate · reload · postgres · postgres-millions
│   │   │                         (charts, scripts, generators, mutations, data)
│   │   └── test/              ✅ JSONL validator + Node WebSocket/dashboard test (`mise run torture:test`)
│   ├── vendor_quickjs.py      ✅ fetch + verify the pinned quickjs-ng release
│   ├── amalgamate.py          ✅ dist/tinyactors.h + dist/tinyactors.c single-file distribution
│   ├── bench_js.mjs           ✅ JS bindings' call cost: Node addon vs wasm (events/s, spawn, hibernate/wake)
│   ├── bench_tracing.mjs      ✅ what tracing costs the JS bindings (untraced, summary, full, decoded)
│   ├── w3c/fetch.py           ✅ download the W3C SCXML IRP suite
│   ├── w3c/convert.py         ✅ .txml -> .scxml via confEcma.xsl (XSLT 2.0, SaxonC)
│   ├── tac/                   ◻ compiler CLI: scxml/manifest -> .tai, --emit-c, --dump
│   └── tarun/                 ◻ runner CLI: image + scripted events + simulated time -> trace
│
├── tests/
│   ├── harness/harness.h      ✅ dependency-free test macros
│   ├── support/               ✅ datamodel_test (ECMAScript-subset test datamodel), compile/system/file helpers
│   ├── unit/                  ✅ test_{base,image,compile,compile_scale,core,system,scenarios,runtime,e2e,
│   │                             datamodel_ecmascript,hibernate,migrate,host,router,address,frame,headers,
│   │                             trace,journal,tracing,otlp};
│   │                             test_torture_{loop,postgres}; test_durable:
│   │                             every persistence path vs. a run that never persisted (durability.md)
│   ├── w3c/run_w3c.c          ✅ IRP runner + BasicHTTP loopback host; with the ECMAScript datamodel
│   │                             all 191 tests pass (skip.txt is empty); irp/ is fetched, gitignored
│   ├── w3c/run_js.mjs         ✅ the same suite through the JS bindings (node + wasm): all 191,
│   │                             with a synchronous BasicHTTP loopback I/O processor
│   ├── conformance/           ✅ host conformance: JSON scenarios (spawn, send, sendMany, timers,
│   │                             hibernate, snapshot/restore, reload/migrate, requests) run by
│   │                             run_c.c (C host), run_js.mjs (node, web) — `mise run conformance`
│   └── fuzz/                  ◻ libFuzzer: image verifier, XML parser, compiler
│
├── bench/                     ◻ micro (selection, VM, pool); macro load lives in tools/torture
│
├── examples/                  hosts built on the public API (`mise run examples:test`)
│   ├── ui-elements/           ✅ frontend only: custom elements backed by actors (wasm), off-screen
│   │                             hibernation; machines tested in Node
│   ├── chess/                 ✅ multiplayer chess: rules (perft), lobby + game actors on the server,
│   │                             browser client over a link, join by code, tests over real links
│   ├── serve.mjs              ✅ static server for the browser examples
│   └── access-reconciler/     ✅ k8s-style controller per employee (charts/access.scxml) + a
│                                 deterministic discrete-event host (host/sim.c): HR feed, SaaS
│                                 apps, approvals, drift, outages, hibernation, invariants
│
├── web/torture/               ✅ index.html + home.js: scenario cards; chamber.html + app.js: live
│                                 dashboard (custom elements cloning <template>s, canvas charts,
│                                 WebSocket); service.js: live service status + start button;
│                                 tokens.css implements docs/design-system.md
│
├── bindings/                  thin hand-written bindings, each with its own mise.toml
│   ├── README.md              ✅ conventions
│   ├── surface.json           ✅ every public C function: "bind" (reachable from every binding)
│   │                             or skipped with a reason; checked with the typings by
│   │                             `mise run surface` (mise/tasks/surface)
│   ├── shared/core.mjs        ✅ the JS API, backend-independent (createRuntime): chart helpers
│   │                             (scxml.chart, c.expr), systems, actors, I/O processors and
│   │                             invokers, links, store links, tracing
│   ├── shared/binding.c       ✅ the native side: pure wrappers of the public headers, a System
│   │                             over ta_host with the native or quickjs ecmascript datamodel,
│   │                             effects, neutral values; Node-API value seam
│   ├── node/                  ✅ N-API (C API) addon (src/addon.c) + index.js, index.d.ts, ws.js
│   │                             (WebSocket server for links); node:test suite incl. hibernation
│   │                             and links, tsc type tests (`mise run node:test`)
│   ├── web/                   ✅ wasm/browser binding, same API: src/glue.c → tinyactors.wasm
│   │                             (zig cc), native.mjs (Node-API value adapter), index.mjs (browsers),
│   │                             node.mjs (Node); `mise run web:test` runs the node suite on it
│   └── postgres/              ✅ PostgreSQL extension: durable actors as rows, SQL API (define,
│                                 spawn, send, run, claim/ack/fail), inbox + outbox tables,
│                                 row-lock concurrency; pg_regress + concurrency tests, test/
│                                 scale.sh (millions of actors), scripts/start.sh (the instance
│                                 in this machine) — `mise run pg:test`, `pg:scale`
│
└── docs/
    ├── architecture.md        ✅ (§15: implementation status and deviations)
    ├── durability.md          ✅ a session's durable state, and what each persistence path keeps
    ├── postgres.md            ✅ the PostgreSQL binding's design: transactions, locking, outbox
    ├── concurrency.md         ✅ concurrency and parallelism, from one actor to PostgreSQL
    ├── scenarios.md           ✅ (executed by tests/unit/test_scenarios.c)
    └── layout.md              ✅ (this file)
```

## Why these tools

| Tool | Why |
|------|-----|
| **mise** | One pinned toolchain; tasks are Ruby file tasks, and the build is Ruby too (`mise/core.rb` over the repository's `mise/lib/toolchain/`). |
| **clang 22** | The pinned compiler for everything that runs here: the sanitizer build, the tools, the addon. |
| **zig cc** | Hermetic cross compiler for the portability matrix (wasm32, musl, macOS, ARM, RISC-V, Cortex-M) from any host, with no SDK installs. |
| **wasmtime** | Executes the wasm32-wasi test build (`mise run check:wasm`). |
| **Python** | Only `tools/w3c/convert.py`: the W3C stylesheets are XSLT 2.0, which SaxonC implements. Never a runtime dependency. |
| **ast-grep** | Structural search over tree-sitter ASTs. The naming lint is an ast-grep rule (`sgconfig.yml`, `tools/lint/`). Invoke it as `ast-grep` (in orbs `sg` is shadow-utils' group tool). |

## Tasks

| Task | What |
|------|------|
| `mise run build` | every layer, test program and tool (`build/dev`); `--release` for `build/release` |
| `mise run test` | every test program under ASan/UBSan, then the tasks below marked *test*; `--full` adds those marked *full* |
| `mise run lint` | *test* · the naming lint: the rule's tests, then a scan of every C source |
| `mise run check:freestanding` | *test* · prove `base/image/core/system/basic_http` compile and link with no libc for 4 targets |
| `mise run amalgamate` | *test* · `dist/<library>.{h,c}` and `dist/<library>_<platform>.c` for `tinyactors` and `tinyactors_runtime`, each compiled on its own |
| `mise run api [--check]` | *test* · `include/<library>.api.json`, the API descriptions the release and the changelog read |
| `mise run w3c` | *test* · W3C IRP with the test datamodel (153 mandatory passes at least) and with quickjs-ng in both realm modes: everything not in `tests/w3c/skip.txt` must pass |
| `mise run node:build` / `node:test` | *test* · the N-API addon; its tests on Node.js and on Bun, and the TypeScript type tests |
| `mise run web:build` / `web:test` | *test* · the wasm module; the node suite against it, plus its own tests |
| `mise run surface` | *test* · the binding surface (`bindings/surface.json`): every public C function classified, every bound one surviving the link of the Node addon and of the wasm module with dead-code elimination, every runtime export typed in `index.d.ts` |
| `mise run conformance:js` | *test* · host conformance through the Node addon and the wasm binding (the C host runs it in `test`) |
| `mise run w3c:js` | *test* · the IRP suite through the JS bindings; all 191 tests must pass |
| `mise run examples:test` | *test* · access reconciler (invariants), chess (rules + games over links), ui-elements |
| `mise run check:wasm` | *test* · the test programs built for wasm32-wasi, run under wasmtime |
| `mise run torture:test` | *full* · every torture scenario headless (postgres only when PostgreSQL answers), plus the dashboard server test |
| `mise run check:cross` | *full* · every layer, test program and tool cross-compiled for each native release target (macOS included) |
| `mise run torture` | the torture chamber with its live dashboard (release build) |
| `mise run pg:build` / `pg:test` | the PostgreSQL extension: build, pg_regress + concurrency tests on a throwaway cluster |
| `mise run pg:scale` | millions of actors on the `pg:start` instance: spawn rate, idle `run()`, hot/cold events, one hot actor, all timers due, backend memory (`TA_SCALE_N`) |
| `mise run pg:start` / `pg:restart` | the PostgreSQL instance of this machine with the extension (127.0.0.1:54320, database `tinyactors`) |
| `mise run vendor-quickjs` | the pinned quickjs-ng release into `third_party/quickjs-ng` |

## Naming

Names are whole words; the only abbreviation is the prefix.

- **Product name `tinyactors`**: `include/tinyactors/*.h` (umbrella
  `include/tinyactors.h`, included as `<tinyactors/system.h>`), libraries
  `libtinyactors_<layer>.a`, the amalgamation `tinyactors.{c,h}`, header
  guards `TINYACTORS_<PATH>_H`.
- **Symbols**: `ta_` for functions and types, `TA_` for macros and enum
  constants; everything after the prefix is whole words
  (`ta_datamodel_environment`, `TA_EFFECT_SEND`, `ta_shared_string_retain`).
  Kept short: unit and width suffixes (`_ns`, `_ms`, `u32`), `id`, `init`,
  `max`, `info`, `stats`, `config`, standard acronyms (SCXML, HTTP, JSON, XML,
  URI, ABI, UTF8, W3C), loop counters `i`/`j`/`k`.
- **Files** carry no prefix (the directory says where they belong) and are
  included path-qualified: `"base/pool.h"` with `-Isrc`.
- **Fields, parameters and locals** follow the same words: counts are
  `X_count`, `capacity`, `length`, `offset`, self-relative pointers
  `*_relative`; parameters and locals are named after their type
  (`ta_system *system`, `ta_core *core`, `ta_instance *instance`).
- `mise run lint` rejects any declared name (function, parameter, local,
  global, field, type, tag, enum constant, macro) that uses a retired
  abbreviation; the list is in `tools/lint/rules/no-retired-abbreviations.yml`.

## Distribution forms

What is released, and how, is in [distribution.md](distribution.md);
these are the forms the library takes.

1. **Static libraries** — `libtinyactors_<layer>.a`, one per layer.
2. **Amalgamation** — `tinyactors.c`/`tinyactors.h`, vendored by DB extensions (SQLite,
   PostgreSQL), embedded firmware, cgo, Rust `cc`, Python setuptools.
3. **WebAssembly module** — the freestanding layers compiled for
   wasm32 export the same ABI; the effect array is read straight from linear
   memory.
4. **Images** — `.tai` files or `tac --emit-c` C arrays for ROM/flash.
