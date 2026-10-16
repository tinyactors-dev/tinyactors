# AGENTS.md: tinyactors-napi

`@tinyactors/node`: one Node-API addon, written in C, that drives
`ta_host` from Node.js and Bun. It is being rewritten from scratch
(`70434bee` removed the first one; [ROADMAP.md](ROADMAP.md) says where
the work is: after the Python binding's step). This file is the rulebook
the rewrite starts from: it holds the binding's rules, true at every
step. The repository root's `AGENTS.md` applies too.

Read first, in this order:
[`tinyactors-core/docs/embedding.md`](../tinyactors-core/docs/embedding.md)
(how to use the core: the loop, lifetimes, memory, a complete loop in C),
[`tinyactors-core/docs/bindings.md`](../tinyactors-core/docs/bindings.md)
(the guide for a binding),
[`tinyactors-core/docs/boundary.md`](../tinyactors-core/docs/boundary.md)
(the contract of every host call), [ADR 0012](../docs/adr/0012-lifetimes-follow-the-loop.md),
and the public headers. Build from those, not from the removed addon (or
the 0.2.0 package in git history) or the proposals' sketches.

## Whom it serves

First the **application developer writing JavaScript** on Node.js or Bun:
chart logic in ECMAScript (the native `"ecmascript"` datamodel, in the
application's own engine), a `Host` on the runtime's event loop, values
that are JavaScript values. Then the **binding author**, held to the guide
by the conformance suite, and the **maintainers**, who run one suite
against every implementation. Every API item is a host input, a typed
method over a posted message, or the handling of an effect
(`docs/api-map.md`); what core does not have is not in the API.

The addon is the binding: it defines the classes, runs the loop,
dispatches effects, converts values and implements the native datamodel
in C. `index.mjs` loads the platform's `.node` file and re-exports it,
nothing else, until the direct layer has passed M1; then the convenience
layer joins it ("Layers and the resolver"). The plan of step 8.5, with
every decision below and its evidence, is [docs/plan.xml](docs/plan.xml).

## Layers and the resolver

Shared with the Python binding (decided 2026-10-13; bindings.md section
1, "Two layers"; `tinyactors-cpython/AGENTS.md`, "Layers" and "The
API"). Node's names and shapes (decided 2026-10-16, docs/plan.xml):

- **The addon is the direct layer, and mirrors the C API** more or less
  one to one: a compile call over `ta_compile_charts` that throws with
  every diagnostic and returns an object whose `image` is a `Buffer`
  copied once on first read (a first read after `close()`, which calls
  `ta_compile_free`, throws `TA_ERROR_STALE`; not a view: a detached
  external buffer is still read after the free, `boundary.md` section 7);
  `addImage(image)` returning the segment
  (`ta_host_add_image` copies the bytes); `chart(name)` over
  `ta_host_chart`; `spawn(ref, ...)` over `ta_host_spawn`, taking that
  reference.
- **A convenience layer in JavaScript above it comes after M1:** it
  calls only the addon's exports and adds shapes, not behaviour. First
  the typed methods as promises (`await host.actorState({session})`,
  resolving from the response handler, rejecting from the error handler;
  decided 2026-10-16: the direct layer keeps ADR 0012's response
  handlers, and a promise is what a JavaScript programmer would write over
  them anyway); then what the maintainer picks (images from a name→text
  mapping, spawning by chart name, a resolver reading files under a root,
  a chart builder).
- **A resolver is any callable** `(base, source) => text` (a function,
  an arrow, a bound method; decided 2026-10-16, as Python's API review
  did), given to the compile call only (a static `src`, at compile time;
  a refusal reported as a diagnostic at its line and column, the message
  in the compilation's scratch arena,
  `tinyactors-core/docs/boundary.md` section 12). **The resolver lives
  only at compile time:** the `Host` has none. A resolver that reads a directory belongs to
  the convenience layer, in JavaScript, confined to its root.
- **`TA_EFFECT_REQUEST` is a fetch at run time** (an `<invoke srcexpr>`
  whose chart is not in the image), and it stays
  (decided 2026-10-13; ADR 0012, decision 8;
  `tinyactors-core/docs/boundary.md` section 12). Core has already
  normalized the name and looked in the requesting segment's image
  (`tinyactors-core/src/system/route.c:439-497`), so until run-time
  fetching is built the addon answers it in C with `ta_host_resume` and
  `TA_ERROR_INVALID` (the parent gets `error.execution`); no JavaScript
  runs and the addon looks nothing up. How a binding will fetch
  is decided with the build-out (root `ROADMAP.md`, "Decided later");
  not now. It is not the control plane (`ta_host_post`,
  `REPLY`, `NOTIFICATION`).
- **A traceparent crosses as W3C text.** In: the string's UTF-8 for the
  call (`ta_spawn.traceparent`, `ta_event_in.traceparent`,
  `tinyactors-core/include/tinyactors/system.h:125-132`, `:171-173`),
  parsed by core (`ta_traceparent_parse`). Out: a send's traceparent is
  read through `TA_EFFECT_FIELD_TRACEPARENT` on first access (the effect
  holds the binary `ta_trace_context`), absent when untraced; an answer
  and a failure keep the send's trace unless the caller passes none
  (`traceparent` in an options object, `undefined` when untraced).

## Names

Our names carry the prefix `ta_napi_*`: every function this addon defines
or declares, static helpers included, and its types and globals. A
platform's name appears only where the platform requires it: the module's
entry points `napi_register_module_v1` and
`node_api_module_get_api_version_v1`, which `NAPI_MODULE_INIT()` defines
(Node.js's `include/node/node_api.h`). A callback given to Node-API (a
`napi_callback`, a `napi_finalize`) is our function, with our name. When
you write a name in prose, say whose it is. The lint rule
`functions-carry-the-prefix` checks functions. In JavaScript the API's
names follow the root's acronym rule (`sessionID`, `toOTLP`).

## The loop

The addon runs embedding.md's loop (sections 2 and 4), in C, as the
guide's `drain` and `turn` (bindings.md section 3) with our prefix: the
ack count that makes thin views stale is bumped in `drain`, right after
the ack.

- **One drain at a time:** take (`ta_host_effects`), handle every effect
  of the batch, ack (`ta_host_effects_ack`), and take again until a take
  is empty; then, at most, a run. A take never widens the open batch, and
  `ta_host_run` refuses while one is open. There
  is no nested drain: JavaScript a handler runs (an I/O processor's
  callback) may call the `Host` to add inputs, post, or read outputs; it
  never takes or acks.
- **The event loop drives every host, transparently** (decided
  2026-10-16: Python has a choice between an event loop and none; Node
  does not). There is no synchronous driver in the API. An input from
  outside a turn schedules one turn with `setImmediate` (one pending at a
  time; not a microtask, so I/O runs between turns); a turn that reaches
  its bound (64 runs, a drain of 64 batches) yields the same way; an idle
  turn arms one timer, the global `setTimeout`, at the deadline, ref'd
  while it is pending and replaced on the next idle turn (Bun has no
  `uv_timer_*`: oven-sh/bun#18546). So an open host with a pending
  deadline keeps the process alive until it or `close()`, which the README
  says. **`await host.idle()`** is a promise that settles at the next idle
  turn: it observes, it drives nothing.
- **Every run status** of bindings.md section 3; both clocks before every
  run and every input that depends on them, converted with
  `system.ticksPerSecond`, which the addon reads from `host.config` at
  creation, and after each run when an encoder is configured.
- **Time in the API is milliseconds** (decided 2026-10-16: what
  JavaScript programmers expect; Python's is seconds): `nextDeadline`,
  `migrate`'s `timeout`, `SimulatedClock.increment(ms)` and `.now`. Ticks
  stay inside the addon.
- **A simulated clock**, `SimulatedClock` with `increment(ms)` (xstate's
  shape, our meaning): it moves the clock and schedules a turn on each
  host made with it, like any input, and turns nothing itself (decided
  2026-10-16); a test writes `clock.increment(1000); await host.idle()`.
  A host made with one arms no timer.
- **Whatever a handler threw:** ack anyway, finish the turn, re-arm the
  timer, then report the first error: it rejects every waiting `idle()`,
  and with none waiting it is thrown from the turn's callback, an
  uncaught error the runtime reports (decided 2026-10-16). A handler's
  promise that rejects after its turn stays an unhandled rejection. The
  README says that either ends the process by default unless the
  application handles `uncaughtException` and `unhandledRejection`.
- **No turn inside another call:** `close()` from a handler is deferred
  to the end of the turn; a finalizer never turns or takes.

## Lifetimes and thin views

ADR 0012: **everything the host hands out about a batch lives until the
ack.** Effect fields (`TRACEPARENT` included), traces, logs,
metrics and hibernation blobs read while handling stay valid
across any later call into the host, until the ack. A datamodel's outputs
live until its next call. Snapshots and the refused config's error fill a
`ta_buffer` the addon lends.

The rules the Python binding follows hold here too (bindings.md, sections
3 and 6; `tinyactors-cpython/AGENTS.md`, "Lifetimes and thin views"):

- **Thin views, not eager conversion.** Wherever a C value is exposed to
  JavaScript, the addon hands out a view that holds the host, the ack
  count and the effect's index and converts a field when it is read: an
  I/O processor's send, a `<log>` line. As with the native datamodel's
  `_event.data`, a view's data from the native datamodel is the payload
  row's value itself, not a copy.
- **A view is stale after its ack.** The addon counts its acks (bumped
  right after `ta_host_effects_ack` in its drain; core keeps no batch
  counter); a read with another count throws with
  `ta_status_name(TA_ERROR_STALE)`.
- **Materialize what outlives the ack, before the ack, when the handler
  returns a promise** (decided 2026-10-16). Every handler gets a thin
  view. When it returns a thenable (an async function, or a plain function
  returning a promise, which a check at registration would miss), the
  addon fills that view with every field before the ack, and its
  `answer()` and `fail()` then work any time while the host lives. The
  view records whether it was answered or failed: a promise that rejects
  before an answer fails the send (`error.communication` for the sender)
  and stays an unhandled rejection; one that rejects after an answer
  fails nothing. A view kept and used from a callback the handler did not
  return (a bare `setTimeout`) is stale there, as in Python. Since ADR 0017
  the sender waits for the ack: an answer given in the handler is what it
  sees when it goes on; one given later arrives as an ordinary event.
- **Host outputs are copied once** into a `Buffer`
  (`napi_create_buffer_copy`) before the ack: telemetry data, `metrics()`,
  `hibernate()`, `snapshot()`; never a view of host memory (boundary.md
  section 7).
- **A reply's `result` is converted once**, at its dispatch, for its
  response handler.

## What reaches the application

| JavaScript sees | Through | Core |
|---|---|---|
| an actor's output | an I/O processor's handler the application configures per host, given the send as a thin view; it may answer or fail it | `TA_EFFECT_SEND`; `ta_host_send` (with `TA_EVENT_IN_ERROR_COMMUNICATION` for a failure) |
| services the charts invoke | a service registered by type | `TA_EFFECT_INVOKE`, `UNINVOKE`, sends with `IO_PROCESSOR` `TA_NIL` |
| an actor's state | the typed method over `actor.state` (subscriptions are removed, ADR 0012 decision 4) | `ta_host_post`, `TA_HOST_EFFECT_REPLY` |
| inspection and trace control | typed methods, each taking a response handler the drain calls when its `REPLY` is dispatched | `ta_host_post`, `TA_HOST_EFFECT_REPLY` |
| `<log>` | a log handler configured per host, given each line as a thin view; none: discarded; no console default | `TA_EFFECT_LOG` |
| telemetry | an exporter the application configures | `TRACES`, `LOGS`, `ta_host_metrics` |
| another system | the host's `transport` handler, given each send to another system's address as a thin view; without one, each such send is failed | `TA_EFFECT_SEND` with `io_processor` 0 |

- **Everything is per host:** handlers, the log handler and the clock are
  given to the `Host`; no global configuration.
- **`DONE` and `ERROR` are internal:** the addon releases its rows for the
  session; no application callback.
- **An I/O processor's handler that throws before answering** (or whose
  promise rejects before an answer) has its send failed, as ADR 0009 has
  a runtime that cannot do what an effect asks answer with a failed
  completion (`error.communication` for the sender), and the error is
  reported as "The loop" says.
- **A log entry is `(session, label, text)`**, no chart.
- **No synchronous request and no raw `post`** (ADR 0012, decision 3): one typed method per method of
  `tinyactors-core/docs/requests.md` but `actor.send` (which is
  `host.send`), shaped `method(params, onResult, onError)` (decided
  2026-10-16): `params` an object passed to core as given (`undefined`
  for none), `onResult(host, result)` at the `REPLY`'s dispatch,
  `onError(host, error)` optional and never a param for core; it returns
  nothing. Its promise form is the convenience layer's. The
  message id is the handle of a row in a pending `ta_table` holding the
  handlers; the drain finds it in O(1), the generation refusing a stale
  id, and pending handlers are dropped, never called, at `ta_host_free`.
- **No notification is defined;** the addon drops a
  `TA_HOST_EFFECT_NOTIFICATION` whose method it does not know.
- **Other systems** (ADR 0014; ADR 0017, decisions 4 and 5;
  bindings.md section 9): the `transport` handler gets every send to
  another system's address (`io_processor` 0; core decides local or not),
  and `send.message` is its outgoing half, an object of `ta_event_in`'s
  fields (`target`, `event`, `data`, `sendid`, `origin`, `traceparent`);
  the other side passes it unchanged to **`host.send(message)`**, which
  ignores keys it does not know, and a send the application believes
  failed is **`host.fail(target, sendid)`**
  (`TA_EVENT_IN_ERROR_COMMUNICATION`). The address fields are **`Address`**
  values, as Python's (decided 2026-10-16): immutable, `.text`, and
  `.system`, `.session` (a `bigint`) and `.name` read by core's parser on
  each access (`ta_address_parse`, `ta_address_name_decode`), `undefined`
  for a part the text lacks, `toString()` the text; `host.send` and
  `host.fail` take an `Address` or a string. No peers, envelopes or
  routes: the transport and the wire encoding are the application's.

## The host's allocator

The host allocates through the `ta_allocator` its environment gives it
(`tinyactors-core/include/tinyactors/allocator.h`), and this addon gives it
`malloc` and `free`, reporting each block to the engine with
`napi_adjust_external_memory`, so the garbage collector sees the host's
memory. That call takes a `node_api_basic_env` (Node.js 22's
`include/node/js_native_api.h:493`), so it may run where only basic-env
functions may, such as a finalizer that frees a host. The two functions
take core's parameter lists (`void *context, size_t size, size_t align`
and `void *context, void *ptr, size_t size`), which the lint exempts by
that signature; the environment they need comes through `context`.

**Alignment: `align` is at most 8, and `malloc` gives at least 8.** The
host passes 8 on every allocation it makes (`tinyactors-core/src/host/host.c`,
`host_alloc`), the largest alignment a region object needs is 8
(`src/base/config.h`, `TA_ALIGN_MAX`), and the allocator's contract says
"a power of two, at most 8 from this library"
(`include/tinyactors/allocator.h:9`). `malloc` returns memory aligned for
any object of fundamental alignment, `_Alignof(max_align_t)` (C11,
7.22.3), and the pinned clang 22 gives `max_align_t` an alignment of 16 on
x86_64 and aarch64 Linux and on x86_64 macOS, and 8 on arm64 macOS: at
least 8 on each. So the addon's `alloc` need not align by hand (an
`assert(align <= 8)` states the contract).

## The binding's memory

The addon allocates almost nothing (bindings.md, section 6):

- **Rows in core's `ta_table`** (`tinyactors/table.h`, ADR 0012 decision
  5): a session (its scope), a chart (its compiled functions), a
  payload (its `napi_ref` and reference count), a `<foreach>`, a pending
  typed method (its handlers). The tables grow
  through the host's allocator and are freed once with the host. Keep
  handles, never row pointers across a `ta_table_add`. The addon writes no
  table of its own.
- **Images are bytes:** `ta_host_add_image` copies them during the call,
  so a caller's `ArrayBuffer` (transferable, resizable, shared) is read
  once and never held; free a compile result at once. No image rows, no
  `SEGMENT_FREED` bookkeeping.
- **One `ta_growable_arena` per host, a mark per call** (ADR 0012
  decision 5). Made with the host, freed with it,
  its chunks kept. Each call takes a mark at entry and releases to it at
  exit, on every path. It holds the call's CBOR (a config, a typed
  method's params, a payload for quickjs, `encode`: a `ta_cbor_writer`
  over the arena's `ta_allocator` view), the call's text (the copy
  Node-API forces, below) and input structs. JavaScript running during a
  call (a getter, `toJSON`) may call the `Host` again: the nested call
  allocates above the outer one and releases to its own mark.
- **Views of what core and JavaScript hold.** Bytes come from a typed
  array's data (`napi_get_typedarray_info`, `napi_get_arraybuffer_info`),
  for the call. Text is the one copy Node-API forces: it is written as
  UTF-8 into the call's arena (`napi_get_value_string_utf8`), and read again
  as UTF-16 only when `EF BF BD` appears, to refuse a lone surrogate. What
  the host hands out is a view until the ack (above). Core's values are
  read with `ta_cbor_reader` as they stream.
- **The datamodel's last output.** `to_wire`, `serialize`,
  `payload_serialize` and the strings handed back to core (`eval_string`,
  `log`) are written with a writer the datamodel keeps, overwritten by its
  next call: the one buffer kept between calls, because Node-API gives no
  view of a string.
- **Payloads are copied, not encoded,** between the API and the native
  datamodel; CBOR only toward quickjs and for bytes that leave the
  process.
- **Statuses are named by core:** a bare status in a message is
  `ta_status_name(status)` (`"TA_ERROR_STATE"`), never a table of ours.
- **Where the allocator is called:** in the host's create and free, in
  the allocator (`src/module.c`, with `napi_adjust_external_memory`) and
  the environment's instance that holds it, and through core
  (`ta_table`, `ta_growable_arena` while it grows, the compiler's
  options); nowhere else, nothing
  per event or per evaluation. The lint rule `allocation-in-tables-and-host`
  checks it. The module's own calls (`encode`) have no host: they use an
  arena of the environment's instance, a mark per call, as a host's
  calls use the host's.

## C rules

- **At most three parameters** for every C function this addon defines or
  declares, static helpers and callback types included (root
  `AGENTS.md`, "At most three parameters"). A function whose signature a
  typedef fixes is exempt by that typedef, not by name:
  `napi_threadsafe_function_call_js` (four parameters), `ta_allocator`'s
  two, and, should a rule grow, `napi_callback` and `napi_finalize`.
  `tools/lint/utils/fixed-signature.yml` lists them by their parameter
  types; add a typedef there, with its header and line, before a function
  it fixes fails the rule.
- **Bytes are `ta_bytes` in and `ta_buffer *` out**, never a pointer and a
  length, in every function (`buffers-are-ta-buffer`).
- **No serialization on the hot path** (root `AGENTS.md`): spawn, send,
  send_many, terminate, deliver, advance_time, resume, run, the effects
  and their ack take and give C structs. CBOR is for `ta_host_post`, the
  config and bytes that leave the process.
- **Acronyms** are all capitals or all lowercase (root `AGENTS.md`,
  "Acronyms"): `ta_napi_otlp_writer`, `TA_NAPI_IO_...` in C;
  `sessionID`, `OTLPExporter`, `ioProcessors` in JavaScript and
  TypeScript.
- **Layers**, each an ast-grep rule:
  - `extension-calls-core-and-node-api`: `src/` calls core (`ta_*`,
    `TA_*`), Node-API (`napi_*`, `node_api_*`, `NAPI_*`) and a short list
    of C library functions (memory, strings, and `malloc`, `calloc`,
    `realloc`, `free` for the host's allocator), nothing else: no libuv
    (`uv_*`; Bun has none) and no V8. A C library function the addon needs
    joins the list with its reason, in the commit that first calls it.
  - `datamodel-calls-environment-and-node-api`: `src/datamodel/` calls of
    core only `ta_datamodel_environment_*`, `ta_image_*` (the chart's
    expressions, read at `chart_attach`), `ta_cbor_*` (`to_wire`,
    `from_wire`) and `ta_table_*`, txml's `txml_*` (XML content, "The
    native datamodel"), and our own `ta_napi_*`; never the host or the
    system (`datamodel.h`: "they must never call ta_system_*
    functions"). It reaches `node:vm` through `process.getBuiltinModule`,
    a JavaScript call made with Node-API, not V8's C++ API.
  - `one-timer-in-loop`: the global timers (`setTimeout`, `setInterval`,
    `setImmediate` and their clear functions) appear in `src/loop.c`
    only.
- **No mutable globals** (`no-mutable-globals`): no global and no static
  local the addon writes. The module is initialized once per environment
  (the main thread and each worker), and a `napi_value` or `napi_ref` of
  one environment means nothing in another, so state lives in the
  environment's instance data (`napi_set_instance_data`) or on the
  `Host`. A constant is a `const` object (`static const char *const
  names[]`, `static const napi_type_tag`; `static const char *p` is a
  mutable pointer). Arrays of `napi_property_descriptor`, the tables
  `napi_define_class` takes, are exempt by type.
- **One thread per host:** a host belongs to the environment (main thread
  or worker) that made it; record it and refuse calls from others. A
  `Host` posted to a worker arrives as a plain structured clone, which the
  type tag refuses (spike, 2026-10-16), so the recorded environment is an
  assertion.

## Values

- **The value table** is `tinyactors-core/docs/protocol.md`, "Values",
  with JavaScript's side in `docs/values.md`: numbers checked with
  `Number.isSafeInteger` and `BigInt` beyond (ids are always `BigInt`:
  `napi_get_value_int64` saturates silently), strings as valid UTF-8 (a
  lone surrogate refused, never replaced), `Uint8Array`, arrays, plain
  objects, `true`, `false`, `null`, `undefined`; **inside a value an
  actor reference is its address string** (decided 2026-10-12, commit
  `20189bdc`): tag 32 is read as its text and never written, as quickjs
  does, since neither of Node's datamodels has a reference type. The
  API's own address fields are `Address` values ("Other systems").
- **`encode(value) -> Uint8Array`** and **`decode(bytes) -> value`**
  expose the one conversion the API uses (`src/values.c`, streaming with
  `ta_cbor_writer` and `ta_cbor_reader`), each with its row in
  `docs/api-map.md` and typed in `index.d.ts`. What they cannot read or
  write is refused with an exception, never turned into a value. The
  adapter frames the driver's calls with them and imports nothing else;
  the binding's own value tests (against `values.txt`'s `ecmascript`
  lines) catch a value mapped wrong both ways.
- **Nesting is limited to 64**, which stops cycles but not a value that
  shares subtrees; the README says charts are trusted code.

## The native "ecmascript" datamodel

Decided 2026-10-16 (docs/plan.xml, decisions d-datamodel, d-declarations,
d-dom, d-separate):

- **Two ECMAScript datamodels, completely separate.** The native one uses
  the application's JavaScript objects in its own engine, as the
  `"python"` datamodel uses Python's; quickjs is the sandbox, for charts
  that are not trusted (`max_ops`, a bounded heap, its realms, passed
  through from the Host's option). They share no code or text and meet
  only in the suite they both pass. Both are named `"ecmascript"`, so a
  host holds one: `new Host(config, {datamodel: "native" | "quickjs"})`,
  and a host that names none gets `"native"`, the maintainer's exception
  to "no defaults of our own" ("it's what most people will expect").
- **Charts evaluate in `node:vm` contexts, never the application's
  realm.** One shared context per host's native datamodel holds the
  charts that declare nothing at the top of their scripts; each of their
  sessions is a scope object inside it. A chart whose scripts declare
  top-level names (`var`, `function`, `let`, `const`, `class`) gets a
  context per session, where a script runs as global code and closures
  work (about 143 KB a session on node, 60 KB on bun). Which charts
  declare is asked of the engine at `chart_attach` (the script after
  `throw 0;` in a scratch context: the global's new names, and a
  redeclaration error for lexical ones): the addon parses no JavaScript.
  Pre-declaring names on a scope and copying bindings out is refused: a
  script's functions close over its own `var`s.
- **A scope is shaped once per chart:** a per-chart constructor whose
  properties are, from the start, the `<data>` ids, the names a
  `<foreach>`'s item and index and a script's assignments bind, the
  session's link and `_event`, over a frozen prototype of null prototype
  holding the system variables as read-only accessors (V8 makes an
  `Object.create(null)` object a dictionary: 280 B against 56 to 64 B).
  `_event` is a non-writable data property redefined at each
  `bind_event`.
- **Expressions compile one at a time, lazily,** at their first
  evaluation, in the context's shape sloppy outer `with (scope)`, strict
  inner function, so an undeclared assignment throws and a syntax error
  is that evaluation's `error.execution`, not the chart's.
- **Values cross by copy, made with the receiving context's
  constructors** (an application's array is not `instanceof Array` in
  another realm); immutable values are shared.
- **XML content becomes a DOM through txml** (`tinyxml/include/txml.c`,
  compiled into the addon), built as JavaScript objects; Node and Bun have
  no `DOMParser`.
- **The system variables cost a session little until read.** A
  session's link holds its `_sessionid` and `_name` (the string of the
  session made before it when the text is the same) and its params until
  the first event; `_ioprocessors` is made at its first read and kept,
  from the context's template (the first session's processors, whose first
  location ends with its id) when a session's processors match it text
  for text, else from the session's own.
- **Measured cost per session** (`mise run measure`, 2026-10-16: x86_64
  Linux 6.18, Intel Xeon @ 2.10GHz, 4 CPUs, 16 GiB; node 22.23.3 with V8
  12.4, bun 1.4.1; 10,000 actors of a chart with three `<data>`, one of
  them a list, resident and idle; all four from one run, this machine's,
  not the Python binding's): bytes per actor

  | runtime, datamodel | resident | engine heap | region | after one event: resident | heap |
  |---|---|---|---|---|---|
  | node, native | 1240 | 189 | 268 | 1655 | 272 |
  | node, quickjs | 878 | 10 | 268 | 1337 | 11 |
  | bun, native | 1762 | 246 | 268 | 2679 | 336 |
  | bun, quickjs | 1172 | 24 | 268 | 1619 | 24 |

  The engine heap is node's `heapUsed + external` and bun's `heapSize +
  extraMemorySize` after a forced collection; quickjs's heap is its own
  malloc, in resident only. Resident varies between runs (bun's native
  1762 to 2049 B in three runs that day; heap and region do not). The Python binding measured 433 B resident
  on its `"python"` datamodel and 1104 B on quickjs on its own machine
  (`tinyactors-cpython/AGENTS.md`): not comparable with these, which are
  another machine's. A native session's heap is its scope (64 B on node),
  its link (60 B), its list and its `_sessionid`; after one event its
  `_event` adds about 85 B. Resident above heap and region is the
  engine's, much of it a fixed cost the population amortizes: V8's young
  generation grows during the spawns (about 300 B an actor of
  `new_space` at 10,000), so at 40,000 the native datamodel is 747 B
  resident on node and 971 B on bun (`--actors 40000`, the same day), and
  about 140 B an actor of malloc (glibc's `malloc_stats` under gdb), most
  of it the session's `napi_ref` (unverified). Before the system
  variables were made lazy, an earlier run on this machine gave 356 B of
  heap on node and 448 B on bun (1565 B and 2135 B resident).

## Rules the suite cannot see, each with a test of its own

- **Dropping a payload's last reference can run JavaScript** (a
  finalizer), and core calls `release` outside a run too (the ack,
  `ta_host_free`, posts, deliver), where it holds the run's rules. Defer
  the last drop to a list emptied after the outer call into core returns
  (bindings.md, section 8).
- **Handles are `ta_table` handles** with a generation: a stale one is
  refused, never a pointer.
- **Expressions are compiled once, lazily, as a strict function inside
  `with (scope)`**, so an undeclared assignment throws and a syntax error
  waits for its evaluation; `assign` checks the location as SCXML asks.
- **One handle scope per vtable entry:** a run is one native call, so
  handles opened by the entries would pile up until it returns
  (`napi_open_handle_scope` around each entry).
- **Reference counts live in the row,** one `napi_ref` per payload row,
  deleted at zero (`napi_delete_reference` takes a `node_api_basic_env`:
  Node.js 22's `include/node/js_native_api.h:339, 355`), so the host's
  finalizer and the environment's teardown free a host with basic-env
  calls only, and skip the last take.
- **Handler closures make cycles:** handlers are kept reachable from the
  `Host`'s own JavaScript object (a property under a private symbol), not
  by strong `napi_ref`s, so a dropped host whose handlers close over it is
  still collected.
- **`napi_type_tag_object` on every wrapped object,** so a method called
  on the wrong object is refused rather than misread.
- **One drain at a time and an ack before every run:** a test that a
  handler's typed method is answered in the next take, and that outputs
  read in a handler stay valid after later calls in the same batch.
- **A thin view read after its ack throws** `TA_ERROR_STALE`; marks nest
  (a nested call leaves the outer call's arena memory intact); pending
  handlers are dropped at `ta_host_free`.
- **The README says** chart code is trusted (a `vm` context is not a
  security boundary; untrusted charts belong on quickjs), that a pending
  deadline keeps the process alive while the host is open (bindings.md,
  section 11), that a handler's error ends the process by default, and
  that `using` needs a runtime that parses it (bun 1.4 does, node 22 does
  not).

## What not to build

- No synchronous request, no raw `post`, no request facade: typed methods
  take response handlers in the direct layer; no synchronous driver.
- No eager conversion of an effect: a thin view, and a record only where
  a value must outlive the ack. No global configuration.
- No re-reading `ta_host_effects` until stable, no consumed marks, no
  nested drain, no ack inside a handler.
- No copy of a host output before the next call (it lives until the ack).
- No handle table, status table, arena or image rows of our own
  (`ta_table`, `ta_status_name`, `ta_growable_arena`, `ta_host_add_image`
  copies).
- No application callback for `DONE` or `ERROR`; no subscription.
- No defaults, limits, methods, events or messages of our own (the one
  exception, the maintainer's: a host that names no datamodel gets the
  native one); nothing C
  does done again in JavaScript (bindings.md section 2); no JavaScript
  above the addon until the convenience layer is taken up ("Layers and
  the resolver").
- No serialization on the hot path, no struct layout read from
  JavaScript, no libuv, no V8's C++ API, no JavaScript parser of our own.

## Tasks

`mise.toml` pins node and bun, which run the package, and pnpm, which
manages its development dependencies; the C toolchain is the root's.

- `mise run build`: core's amalgamation into `build/core`
  (`mise run //tinyactors-core:amalgamate -- --out build/core`), the only
  core task this project's tasks run, never core's build; the addon
  compiles against it with `Toolchain::Target` (root
  `mise/lib/toolchain.rb`; never a compiler from `PATH`, never `$CC`).
  `Target#plugin` links a loadable module, and `Toolchain::ObjectFormat`
  already allows a Node-API addon's undefined `napi_*`.
- `mise run lint`: the rules' tests, the rules over `src/`, the adapters'
  lint (`mise run //tinyactors-test:lint -- conform.mjs`: the adapter
  imports only `@tinyactors/node` and `node:` built-ins),
  `docs/api-map.md` against core (`mise/lib/api_map.rb`), and the root's
  checks on this project's files: the build scripts' portability and
  acronyms.
- `mise run conform [-- runner options]`: the suite against
  `conform.mjs`, with `--kind binding`, under node and under bun, each on
  the native datamodel and on quickjs (both must pass); `-- --milestone
  <M>` runs one of the suite's sets (`tinyactors-test/suites/milestones.txt`).
- `mise run test`: lint, build, both runtimes, and the suite's set named
  by `MILESTONE` in `mise/napi.rb` once the adapter exists. Its `#MISE
  sources` list this project's files, core's and the suite's. An
  exhaustive test goes behind `--full` (root `AGENTS.md`).
- `mise run measure [--actors N]`: bytes per actor at a population of
  one chart (`tools/measure.mjs`), resident and idle and after one event
  each, on both datamodels under both runtimes, each in a process of its
  own, with the machine's signature; it writes `build/measure.json`. A
  measurement, not a test: `mise run test` does not run it.
- `mise run pack`: one prebuilt addon per platform package; it aborts
  until packaging is built.

The adapter imports the package by its name, `@tinyactors/node`, which
resolves to this directory through `package.json`'s `exports`; its lint
refuses a path. A spike (2026-10-12) showed node v22.23.2 and bun 1.4.1
(this project's pins) both resolve it.

## Commits and the changelog

Commit subjects are the changelog's input (root `AGENTS.md`, "Releases
and changelogs"): scope `napi` (`feat(napi): ...`), `!` for a breaking
change with a body that says what a caller does. The package continues
the release line of `@tinyactors/node` (tags `tinyactors-napi/v*`, last
0.2.0), so every `feat`, `fix` and `refactor` lands in its next
changelog.

## Code search, static checks and design claims

- **ast-grep for code search and rewrites** (`ast-grep run -p ... -r
  ...`), `rg` only for plain text. Invoke `ast-grep`, never `sg`. A new
  static check is an ast-grep rule in `tools/lint/rules/` with its test in
  `tools/lint/tests/`.
- **Design claims are checked** with the reasoned-analysis skill: a
  scenario in this design, evidence (`path:line`, a primary source or a
  spike) or the word unverified, and a refutation by a fresh agent before
  a finding changes the design, the guide or a request to core.

## Gaps in core

This project never patches `tinyactors-core` or the suite
(`tinyactors-test`), and works around nothing in them. A gap is a request
to core in [ROADMAP.md](ROADMAP.md): what is missing, why, and which
suite case or test shows it. The addon waits for the change.

## Layout

The Python binding has the same files on purpose, and the two are
compared file by file.

```
README.md, CHANGELOG.md, mise.toml, package.json, sgconfig.yml
ROADMAP.md           the work in progress: plan, progress, how it runs, requests to core
index.mjs, index.d.ts   load the platform's .node and re-export it; the types
docs/api-map.md      each API item → ta_* function, method or effect (checked)
docs/values.md       the JavaScript side of the value table
src/                 addon.h, module.c, host.c, loop.c, effects.c, methods.c, values.c,
                     datamodel/{datamodel.c, rows.c, expressions.c}
conform.mjs          the suite's adapter, over the public API only
test/                what the suite cannot see: liveness, workers, GC, refusals
tools/lint/          rules/, tests/, utils/ (ast-grep)
tools/measure.mjs    bytes per actor, node and bun, native and quickjs (mise run measure)
mise/napi.rb         what the tasks share
mise/tasks/          build, test, conform, lint, measure, pack
build/               core's amalgamation and the addon (not in git)
```
