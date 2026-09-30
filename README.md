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

Tinyactors is built in four layers. Each has one job, and bytes and
decisions do not pass through a layer that does not need them.

| Layer | Its job, in one line | What it holds | Where |
|---|---|---|---|
| **Core** | Decides what an input *means*, and guarantees that the same inputs at the same times give the same state. | Statechart semantics: configuration, macrosteps, delayed sends, invocations; replaying actors, prepare and publish; the interfaces a host implements (data model, router, I/O processors, invokers, directory, allocator). No I/O, no threads, no queues, no log, no storage. | `tinyactors-core/` |
| **Host kernel** | Decides how inputs are *queued, recorded and fed back*. | Everything a host that keeps actors in memory needs, free of any language's API: scheduling, mailboxes, the router, the directory, the turn logic, effects, the OTLP encoder, and the write-ahead log (message identity, records, batches, wakes, the write-ahead hold, replay and checkpoint encoding). | `tinyactors-host/` (being extracted, see the [roadmap](ROADMAP.md)) |
| **Language binding** | Decides how values *look* in its language and how they *become* bytes. | Value boxes; the data model and its durable codec (a data model encodes and decodes its own values); a pump driver on the language's event loop; the public API. | `tinyactors-napi/` (Node.js and Bun); the test host `tinyactors-test/` is a binding for testing |
| **Application** | Decides where bytes *live*, and everything that is policy. | Storage for logs and checkpoints; eviction; loaders; several bindings sharing one system; deployment. | outside this repository |

Two rules follow from the table:

- **A host adds what a language needs; the kernel adds what every host
  needs.** Code free of any language's API goes in the kernel, so a second
  binding does not copy it.
- **Core does not see bytes it has no use for.** The host sits at the
  input edge and already has every value, so it records inputs and encodes
  them itself; core only guarantees that replaying the recorded inputs
  rebuilds the state.

## Repository layout

- `tinyactors-core/` is the core: the freestanding-friendly C API and the
  production interpreter.
- `tinyactors-host/` is the host kernel (being extracted from napi; see
  the roadmap).
- `tinyactors-napi/` is the Node-API binding for Node.js 22 and Bun.
- `tinyactors-test/` is the instrumentable JSON-RPC test host and its Ruby
  driver: a binding that exercises core, and the kernel, from scenarios.
- `tinyactors-scxml/` is the SCXML codec, and `tinyxml/` the XML reader it
  is built on.
- `docs/proposals/` holds outlines of possible future work, such as new
  hosts. They are not plans or decisions; see its README.
- `docs/research/` holds background research that informed the designs,
  with sources, so it need not be repeated.

Each project records its decisions in its own `docs/adr/`.

Run `mise run build` to build every project and `mise run test` to test
what changed.
