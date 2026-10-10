# Tinyactors

Tinyactors is an implementation of actor system based on the [SCXML](https://w3.org/TR/scxml) standard.

It is tiny, because it doesn't perform I/O, use threads, or directly allocate memory: the core is libc-free.

This design is chosen to make it easy to embed Tinyactors into existing runtimes, which
have already made decisions about all of these things.

Tinyactors just bring a robust implementation of the semantics to the table, usable everywhere:

- in the browser through a wasm-freestanding target,
- in your favorite programming language through bindings of the native binary,
- in games, because the implementation is resource-efficient.

The actor model makes modeling distributed systems much more tractable than purely
imperative code, or sequential code written in an functional style with async/await.

There purposely is no "let's pretend code is sequential" API for Tinyactors: you'll have
to model states and transitions explicitly.

Thanks to the statecharts formalism, you can safely compose multiple state machines without
drowning in a combinatorial explosion of states and transitions.

If you ever had to manage the state of a live websocket connection, you have experienced
the uneasiness of that state management logic not having clear home in your system. Retries,
reconnects, timeouts, reconnect storms all become major headaches as a result.

Using the actor model, you can model just how a websocket connection behaves on your server,
*without* having to keep the full system in your head.

This is good for humans and for agents.

## Status

Under active development - breaking changes are made frequently to land on a good API.

## Layers and responsibilities

Tinyactors is built in layers. Each has one job, and bytes and decisions do
not pass through a layer that does not need them. The C layers are static
libraries in `tinyactors-core/`, and each links only against the layers
below it ([architecture](tinyactors-core/docs/architecture.md), section 1).

| Layer | Its job, in one line | What it holds | Where |
|---|---|---|---|
| **Core** (base, image, trace, core, system) | Decides what an input *means*, and runs actors in one block of memory sized at creation. | Statechart semantics over an immutable compiled image; mailboxes, the run queue and timers in a shared event pool; the SCXML I/O processor; hibernation and live migration; trace contexts carried by every event, and trace records of every step, drained like effects ([ADR 0006](docs/adr/0006-tracing-is-a-core-feature.md)). Freestanding: no C library, no malloc, no I/O, no threads, no clock. | `tinyactors-core/src/{base,image,trace,core,system}` |
| **OTLP codec** | Turns trace records into OpenTelemetry's OTLP protobuf, and reads OTLP back. | An encoder in one block sized at creation (open spans, staging), fed by a host after each run; never runs inside one. Freestanding like core. | `tinyactors-core/src/otlp` |
| **Compiler** | Turns SCXML documents into an image. | XML to SCXML to bytecode, validation and linking; an XML writer (`ta_xml_writer`) for charts a binding builds, so no binding writes or escapes XML. Allocates through an allocator the caller gives it. | `tinyactors-core/src/compile` |
| **Host** | Runs many systems as one. | Segments per image, routing between them, chart versions, migration jobs, the hibernation sweep, growth by relocation, one fuel clock and traceparents across segments and systems. Freestanding like core: it allocates only through the allocator its caller gives it ([ADR 0005](docs/adr/0005-freestanding-host.md)); the runtime conveniences put the C library's `malloc` behind one. | `tinyactors-core/src/{host,runtime}` |
| **Data models** | Evaluate expressions in a language. | The null data model in core; ECMAScript on quickjs-ng, linked like a tree-sitter language. | `tinyactors-core/src/datamodel_ecmascript` |
| **Language binding** | Decides how values *look* in its language and how they *become* bytes. | Node.js (N-API), browsers and Node.js (WebAssembly), PostgreSQL; one JavaScript API over the first two, which hands OTLP bytes to a sink or posts them to an OTLP/HTTP endpoint. | `tinyactors-core/bindings/` |
| **Application** | Decides where bytes *live*, and everything that is policy. | Storage for hibernated actors and snapshots; eviction; deployment. | outside this repository |

`libtinyactors` holds core, the OTLP codec and the host, and promises: **no C library;
base through system never allocate; the host allocates only through the
`ta_allocator` it was given.** The compiler and the runtime conveniences
(`libtinyactors_runtime`) use the C library.

Two rules follow from the table:

- **Core never does I/O and never calls back with an effect.** Effects are
  drained by the host (`ta_system_effects`); the only callbacks are the
  data model and the allocator.
- **Core does not see bytes it has no use for.** A payload is a handle the
  data model owns; core passes it along without reading it.

## Repository layout

- `tinyactors-core/` is the runtime: the C layers above, its bindings
  (`bindings/`), the torture chamber (`tools/torture/`), and examples.
- `tinyxml/` is a freestanding XML reader and writer.
- `tinyparse/` is a freestanding parser runtime and its CLI: grammar data
  in, a streaming parser and a syntax tree out, or a generated C parser.
- `staticjs/` is a pure, total subset of JavaScript: the language of a
  proposed data model (`docs/proposals/staticjs.md`).
- `docs/proposals/` holds outlines of possible future work, such as new
  hosts. They are not plans or decisions; see its README.
- `docs/research/` holds background research that informed the designs,
  with sources, so it need not be repeated.

Each project records its decisions in its own `docs/adr/`.

Run `mise run build` to build every project and `mise run test` to test
what changed.
