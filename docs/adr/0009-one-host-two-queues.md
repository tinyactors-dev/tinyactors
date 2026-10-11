# ADR 0009: One host, two queues

**Status:** Accepted (2026-10-11). Supersedes [ADR 0008](0008-bindings-only-do-io.md).
Proposal: [one host, two queues](../proposals/one-host-two-queues/README.md),
which keeps the binding guide's draft and the Node, CPython and browser
sketches.
**Date:** 2026-10-11

## Whom it serves first, and who pays

Following [docs/roles.md](../roles.md):

- **First: the binding author and the maintainers.** After ADR 0008 the
  Node.js binding is 11,620 lines and its web twin 4,576, mostly wiring of
  core objects (the host, `src/link`, the OTLP batcher) and one
  hand-written converter per C struct. Each restructure so far moved code
  between layers without removing it. With `ta_host` as the only
  interface, a binding is a queue loop, a datamodel or none, and a CBOR
  module.
- **Then: the C embedder.** One object to drive, with the rarely called
  operations as requests in bytes, and the same defaults, validation and
  error messages a binding gets.
- **Then: the application developer, the operator and the observability
  backend.** The same API in every runtime of a language, and the same
  OTLP, encoded once into the binding's buffer.
- **Out of scope:**
  - durability: snapshots and hibernation take actors out of memory for a
    while and export moves them; none of them is persistence;
  - any transport protocol in core: nothing replaces the link protocol;
  - translating values between datamodels (decision 7);
  - a multi-threaded host;
  - the PostgreSQL binding, for now.
- **Who pays:**

  | Cost | What |
  |---|---|
  | per event | nothing: spawn, send, deliver, run and the effects stay structs |
  | per request | one CBOR encode and decode, for rarely called operations only |
  | per actor | nothing |
  | memory | the caller's token list and buffers; the CBOR functions allocate nothing |
  | code size | a CBOR module in core; about 1,800 lines of C leave core (link, JSON, framing, the batcher) |
  | API surface | `host.h` regrouped into structs, effect accessors, `ta_host_request` and its catalogue of operations |
  | every binding | deleted and regenerated from one guide |

## Context

`ta_host` already takes inputs and gives effects, and holds the effects
until they are acknowledged. The bindings grew because we put more objects
beside the host for them to wire (ADR 0008 added `src/link`, a JSON reader
and the exporter's batcher), and because rich data crossed as one C struct
per topic, which each binding converted by hand. The proposal's sketches
of a Node addon, a CPython extension and a browser build, each designed
from a guide alone, found what core lacks for the host to be the only
interface; decision 9 lists it.

## Decisions

### 1. `ta_host` is the only interface

Every runtime talks to the same object: there is no second engine, no
wrapper layer and no other core object for a binding to wire. A binding
works the two queues: it feeds inputs, runs the host, takes the effects,
does the I/O they ask for, acknowledges them and arms one timer from the
next deadline.

### 2. The hot path stays structs; everything else is a request

- **Structs:** spawn, send, send_many, deliver, advance_time, resume, run
  and the effects.
- **Requests and replies (CBOR maps):** every rarely called operation:
  trace configuration, watches, stats, metrics, inspection, the state
  value, snapshot contents and decoded spans. A request names its
  operation in `"op"`; a reply is the result or
  `{"error": {"code", "path", "message"}}`. Keys are text, so requests
  stay readable in diagnostic notation and logs.
- **Core owns the schemas**, and with them the defaults (a watch's 15
  minutes, the sampling scale), the validation and the error messages.

```c
TA_API ta_status ta_host_request(ta_host *host, ta_bytes request, ta_buffer *reply);
```

### 3. CBOR as token lists, in caller memory

`src/base/cbor` and `include/tinyactors/cbor.h` (RFC 8949):

- the decoder turns a message into a flat list of tokens in a buffer the
  caller provides; text and byte tokens are views into the message;
- the encoder writes a token list in core deterministic form
  (RFC 8949 §4.2.1);
- writer helpers fill a token list from C, so a C embedder builds a
  request without a second API;
- freestanding, no allocation; indefinite lengths are unsupported and
  nesting is limited.

A binding walks the token list in its own language. It needs one generic
CBOR module and no CBOR library, and pays one call across per message, not
one per item. The C API is in the proposal.

### 4. Neutral values are CBOR

In one change, so there is one value encoding: the datamodel vtable's
`to_wire` and `from_wire`, the quickjs datamodel's value functions,
hibernation, export and snapshot blobs, and the Values section of
`docs/protocol.md`. An actor reference (`{"$ref"}` today) is tag 32 on its
address.

### 5. What leaves core

- **The link protocol**, `src/link` and `link.h`. Nothing replaces it.
  Between systems, the router keeps peers, routes, deduplication and
  error envelopes; an envelope leaves as an effect, written as CBOR into
  the caller's buffer by `ta_host_envelope`, and arrives as an input. How
  it travels is the application's.
- **Framing**, `src/base/frame.c` and `frame.h`: it is transport.
- **JSON**, `src/base/json` and the JSON forms of neutral values, from
  core, the bindings and the examples.
- **The batcher**, `ta_otlp_batch` (decision 6).

### 6. The host owns the OTLP encoder

Journals drain into `ta_otlp` inside the host's run. When the encoder's
staging fills (`TA_OTLP_FLUSH`) or a flush deadline passes, an effect says
traces are ready, and `ta_host_traces` writes the OTLP request into a
buffer the binding supplies: encoded once, written once. The spans leave
the staging only on `TA_OK`. Retries, caps and failure logging around the
HTTP post are I/O policy, so they are the binding's.

### 7. Datamodels: indices stay, nothing is translated

A binding with its own datamodel implements the datamodel vtable, the one
call out of core; a binding that uses the quickjs datamodel implements
nothing core calls. Core picks a chart's datamodel by the vtable's `name`,
so a binding registers a datamodel of its own language (CPython's
`"python"`) without a core change.

`ta_host_config` keeps its array of datamodels, and effects and envelopes
keep their datamodel index. Core does not translate a value between
charts that use different datamodels: a payload is handed only to the
datamodel whose index it carries.

### 8. Calls and buffers

- **Inputs during a run are appended, not refused.** send, send_many,
  spawn and terminate made during a run append to the mailbox, the path
  an internal `<send>` or `<invoke>` takes: no new queue, no copy, no
  growth mid-run (a full pool is `TA_ERROR_NO_MEMORY`). This replaces the
  bindings' queues of calls made during a run. Structural calls and
  requests stay refused during a run (`TA_ERROR_STATE`).
- **One buffer convention.** Every call that writes for its caller takes
  a `ta_buffer` (`pointer`, `capacity`, `length`), replacing the five
  forms of `buffer, capacity, *length` today.
- **Requests are atomic.** `TA_ERROR_NO_MEMORY` means nothing changed and
  `length` is what the call needs: a request whose reply does not fit is
  not applied, and envelopes and traces stay with the host until they are
  written.
- **At most three parameters** per function in the public headers; the
  rest are grouped in structs. The rule is in `AGENTS.md`.

### 9. Core changes the sketches found

Designing the Node addon from the guide alone needed:

- structs for the `host.h` functions over three parameters;
- a `CHANGED` effect for subscribed sessions;
- the invocation token on every send to an invoked service;
- both clocks in `ta_host_advance_time`;
- `ta_host_metrics` writing the counters on demand, with no metrics timer
  or effect;
- `ta_host_create` taking CBOR configuration plus a struct of pointers;
- effect accessors (`ta_host_effect_number`, `_text`), so no binding
  decodes a host struct.

### 10. The bindings and examples are deleted and regenerated

`bindings/node`, `bindings/web`, `bindings/postgres`, the `bindings/`
directory with its README and `surface.json`, the examples, the JavaScript
runners of the conformance and W3C suites, the benchmark scripts that
drive the bindings, the torture chamber's PostgreSQL scenario, and their
mise tasks are deleted. They are regenerated from
`tinyactors-core/docs/bindings.md`, written from the proposal's draft
before the first binding is. Each regenerated binding is a project at the
repository root with its own `mise.toml`; we confirm this with the first.

### 11. Conformance is request and reply vectors

Pairs of CBOR, shown in diagnostic notation, in `tests/conformance`, run
by a C runner, that every regenerated binding must reproduce. They
replace the surface check; the C conformance scenarios and the W3C suite
(all 191) stay.

## What stays

- the kernel, `ta_system`, the compiler and images;
- `ta_host`: segments, router, migration, hibernation, snapshots;
- the OTLP encoder, now owned by the host;
- the quickjs datamodel, its value functions on CBOR;
- the C test programs, the C conformance scenarios and W3C;
- the torture chamber, without its PostgreSQL scenario.

## Order of work

Each step is a commit, or a short series, that passes `mise run test`:

1. this ADR, and the three-parameter rule in `AGENTS.md`;
2. delete the bindings, the `bindings/` directory, the examples, the link
   layer, JSON, framing, `ta_otlp_batch` and their tasks; core's C tests
   still pass;
3. `src/base/cbor`, with unit tests and the RFC 8949 test vectors;
4. neutral values as CBOR (decision 4);
5. the host: the encoder, inputs during a run, requests (decisions 2, 6,
   8 and 9);
6. request and reply vectors (decision 11);
7. the guide, `tinyactors-core/docs/bindings.md`;
8. the bindings: one JavaScript package at the repository root over two
   addons (Node-API and wasm), then the examples; then CPython with its
   `python` datamodel; PostgreSQL and other languages later.

## Consequences

- A binding is a queue loop, a datamodel or none, and a CBOR module.
  Nothing C does is done again in a language.
- Until step 8, the repository has no binding and no JavaScript
  conformance run; the C suites are what a commit passes.
- A runtime that cannot do what an effect asks answers with a failed
  completion, which becomes `error.communication`.
- Connecting systems needs code from the application: core gives
  envelopes as effects and takes them as inputs, and nothing more.
