# Writing a binding

**Status:** Current (2026-10-11), after [ADR 0009](../../docs/adr/0009-one-host-two-queues.md) steps 2 to 6. Drafted in the proposal [One host, two queues](../../docs/proposals/one-host-two-queues/README.md).

Everything a binding author needs, in one place: the rules, the run-loop lessons of the past restructures, and the interface a binding drives. Write a binding from this guide and the headers, not from an older binding.

## 1 · What a binding is

A binding lets one language drive `ta_host` in one runtime. It is that runtime’s extension, written in C against the runtime’s API (the CPython C API, Node-API): the extension defines the language’s types, runs the loop, converts values and implements the native datamodel, and the language holds nothing above it but loading it. There is no shared package per language; how a browser is served is decided later (`ROADMAP.md`). It has three parts and an API:

- **The queue loop:** feed inputs, run the host, read its effects, do the I/O they ask for, acknowledge them, arm one timer.
- **A datamodel:** the language’s own values and expressions behind the datamodel vtable, with the ECMAScript datamodel (quickjs-ng), which needs nothing from the binding, beside it (section 8).
- **A CBOR module:** one generic conversion between a CBOR token list (`tinyactors/cbor.h`) and the language’s values. It carries the host’s config, requests and replies, and the bytes that cross a process boundary: envelopes and event data.
- **The API:** what the language’s developers call. It is thin: each item is a host input, a request, or the handling of an effect.

## 2 · What a binding never does

- **It exposes the core, nothing else.** Before designing the API, inventory the public headers it drives and write the mapping table: each API item next to the core functions, effects, methods and flags behind it. An item with no core function behind it does not belong. A core function with no item is used internally (say where) or not exposed (say why).
- **It borrows shape, never semantics.** Another library (xstate for JavaScript) may suggest names and call shapes: `createActor`, `send`, `getSnapshot`, `subscribe`. What a call does is what the core does. Never emulate another library’s behaviour, rename the core’s events, or lower its concepts onto SCXML.
- **It invents nothing:** no event names, flags, fields, methods, defaults or mechanisms of its own. A real gap is a core proposal, not part of a binding.
- **It keeps no twin of C.** SCXML semantics, queues, routing, timers, the invoke lifecycle, addresses, trace policy, encodings: if C does it, bind the C.
- **It reads the core before claiming a gap.** `src/core` and `src/system` implement all of SCXML, including I/O processors, external invokes, sends to `#_<invokeid>` and error events. Cite the lines of `docs/SCXML.md` and the core before saying “the core cannot”.
- **It decides no policy.** Defaults, limits, validation and their error messages live in core and come back in replies. Retry and backoff for the binding’s own I/O (an HTTP post, a reconnect) are the exception: that is I/O.
- **It puts no serialization on the hot path.** The calls made per event take and give C structs; CBOR is for rarely called calls and for bytes that leave the process (the repository’s `AGENTS.md`, “No serialization on the hot path”).

## 3 · Creating a host, and the queue loop

```
config  = cbor_encode({ systemID: "app", system: {...}, telemetry: {...} })
error   = ta_buffer of your own
env     = { allocator, datamodels[], datamodel_count, trace_policy, error: &error }
ta_host_create(config, &env, &host)      # TA_ERROR_INVALID: error holds the reply
rate    = ta_host_request(host, {"id": 1, "method": "host.config"}).result.system.ticksPerSecond
                                         # once: the ticks per second of the monotonic clock
ta_host_add_image(host, image, &segment); ta_host_boot(host, segment)
```

- **The config is CBOR** ([host-config.md](host-config.md)): the system id, each segment’s capacities, the host’s I/O processors, the OTLP encoder and the host’s limits. Build it with the CBOR module from the language’s data. Core fills the defaults and validates it, and the request `host.config` ([requests.md](requests.md)) answers the effective config, every default filled in; a refused config answers in `environment->error` with the same `{"error": {"code", "message", "data": {"reason", "path"}}}` as a refused request. Raise it as the language’s exception, unchanged.
- **The environment** holds what cannot be data: the allocator, the datamodels (at most 8), the trace policy and the error buffer.

```
on wake (timer fired, input arrived, I/O completed):
    ta_host_advance_time(host, { monotonic, wall_ns })
    repeat:
        run = ta_host_run(host, budget)
        for i, effect in ta_host_effects(host):
            handle(effect)                  # copy what you keep: views end at ack
        ta_host_effects_ack(host)
        TA_RUN_EFFECTS_FULL → the effect list was full: run again now
        TA_RUN_EFFECTS      → answer the sync sends (they are inputs), run again now
        TA_RUN_YIELDED      → give the event loop a turn, wake again at once
        TA_RUN_EXHAUSTED    → the allocator refused (run.detail names the pool):
                              run again once it has memory
        TA_RUN_FAULT        → a segment could not run, or ta_host_run was called
                              during a run: report it
        TA_RUN_IDLE         → arm one timer at run.next_deadline (UINT64_MAX: none)
    whatever a handler raised: ack anyway, finish the turn, then raise the first error
```

- **One timer,** re-armed after every loop; never one per actor or per deadline. `next_deadline` already holds every deadline the host has: timers and delayed sends, a migration job’s timeout, the TTL of envelopes queued for a peer that is down, the end of a trace watch, and the encoder’s flush interval.
- **Both clocks in one call.** `monotonic` (`ta_time`, in ticks at the rate `host.config` answers as `system.ticksPerSecond`) drives timers, deadlines and TTLs; `wall_ns` (nanoseconds since the Unix epoch) stamps the trace records written since the last call. Call it before every run.
- **Bound each turn.** The budget caps work per run; across many runs, yield to the runtime’s event loop (a macrotask in a browser, a callback in Node, a Looper message on Android) so a busy system never blocks it.
- **A caller’s own clock** (simulation, tests) replaces the timer: time moves only when the caller advances it and runs the loop. A binding offers it as a simulated clock in its public API (the shape of xstate’s `SimulatedClock` and `increment(ms)`, the meaning ours): a host created with one arms no timer; `increment(ms)` calls `ta_host_advance_time`, with the simulated time as the wall clock, and runs the loop until idle; inputs run the turn at the current time (`ROADMAP.md`, step 8, M1).
- **Not every program has an event loop.** Offer the same turn under two drivers where the language has both: a synchronous one (run until idle, sleeping to the next deadline or following the caller’s clock) and the language’s event loop (asyncio, a Looper, a dispatch queue).

## 4 · Effects: what to do with each

Read an effect’s fields with `ta_host_effect_number`, `ta_host_effect_text` and `ta_host_effect_bytes` (the relayed value's `WIRE`) (`TA_EFFECT_FIELD_*`, in `host.h`), by its index in the list `ta_host_effects` returned.

| Effect                  | The binding                                                                                                                                                                                                                       | Answers with                                                                                                                         |
|-------------------------|-----------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------|--------------------------------------------------------------------------------------------------------------------------------------|
| `TA_EFFECT_SEND`        | Hand it to the I/O processor at index `io_processor` of the config’s `ioProcessors`. A `sync` processor answers before the next run. With `io_processor` `TA_NIL` it is a send to an invoked service: route it by `token`.          | An event, if the processor replies; on failure an event with `TA_EVENT_IN_ERROR_COMMUNICATION` and the send’s `sendid`.              |
| `TA_EFFECT_INVOKE`      | Start the service of type `event` with `target` (its src), `sendid` (its invokeid) and `payload` (its params), keyed by `(segment, token)`. Sends to it carry the same token: keep no table of invoke ids.                         | Its events with `TA_EVENT_IN_FROM_INVOKE` and the token; at its end `TA_EVENT_IN_INVOKE_DONE`, or a communication error.            |
| `TA_EFFECT_UNINVOKE`    | Stop that service; forget the key.                                                                                                                                                                                                | Nothing.                                                                                                                             |
| `TA_EFFECT_REQUEST`     | An `<invoke>` whose `src` (`auxiliary`) names no chart in the image. Resolving it may be I/O (fetching a chart).                                                                                                                  | `ta_host_resume` with the token and the chart index + 1, or an error status.                                                         |
| `TA_EFFECT_DONE`, `TA_EFFECT_ERROR`, `TA_EFFECT_LOG` | Tell the application: the actor’s output or error code, a `<log>` line.                                                                                                                              | Nothing.                                                                                                                             |
| `TA_EFFECT_WAKE`        | Load the actor’s blob from the store. If the binding still holds that blob (its `put` has not settled yet), wake from the held copy and delete the store’s copy when the put settles.                                             | `ta_host_wake` with the blob.                                                                                                        |
| `TA_EFFECT_CHANGED`     | A session subscribed with the `actor.subscribe` method finished a macrostep: tell its subscribers. At most one per session per run, and none for sessions nobody subscribed. Read its `actor.state` or `actor.context` after the run.    | Nothing.                                                                                                                             |
| `TA_HOST_EFFECT_MIGRATED`, `TA_HOST_EFFECT_JOB_DONE` | A session moved to a new chart version; a migration job ended. Settle what the application started (its promise, future or callback) with `migrated`, `failed` and the pending sessions (`ta_host_job_pending`). | Nothing.                                                                                                                             |
| `TA_HOST_EFFECT_SEGMENT_FREED` | Release the image the binding kept for that segment.                                                                                                                                                                       | Nothing.                                                                                                                             |
| `TA_HOST_EFFECT_PEER`, `TA_HOST_EFFECT_PEER_ERROR` | An envelope, or an error envelope, for the system behind `peer`. Carrying it is the application’s (section 9).                                                                                          | `ta_host_deliver` or `ta_host_peer_error` for what arrives.                                                                          |
| `TA_HOST_EFFECT_TRACES`, `TA_HOST_EFFECT_LOGS` | The encoder has a request ready. Call `ta_host_traces` (`ta_host_logs`) with a buffer of your own, then post it (`application/x-protobuf` to `/v1/traces`, `/v1/logs`) or give it to a sink. Retries and a cap on what waits are yours. At most one of each is outstanding: the next comes after a take. | Nothing.                                                                                                                             |

- **A host without telemetry** has a null encoder: it never gives `TRACES` or `LOGS`, and `ta_host_traces`, `ta_host_logs` and `ta_host_metrics` answer `TA_ERROR_STATE`.
- **Stopping ends the open spans.** A macrostep cut short has its span still open. `stop()` sends the `trace.end` request, takes traces with `ta_host_traces` until a take writes nothing, posts them, and then calls `ta_host_free`. Ended spans carry `scxml.incomplete` and end at the last `wall_ns`; `ta_host_free` drops what was not taken.
- **Metrics are pulled.** `ta_host_metrics` writes the counters, as an OTLP metrics request, into your buffer whenever you call it. There is no metrics timer or effect: read them when the application asks, on `stop()`, or on a cadence the application chooses.

## 5 · Inputs and reentrancy

- **Between runs, any call is fine,** including from an effect handler before the ack: effects already taken stay where they are, and effects the call makes come with the next `ta_host_effects`.
- **During a run** (a datamodel entry evaluating an expression), `ta_host_send`, `ta_host_send_many`, `ta_host_spawn` and `ta_host_terminate` take the paths of an internal `<send>`, `<invoke>` and cancel: no queue of their own, no copy, and nothing grows, so a full pool answers `TA_ERROR_NO_MEMORY` and the payload stays the caller’s. When they take effect:
  - an event for the actor whose expression sent it: at the end of the macrostep it is in, never inside it;
  - an event for another actor, or a spawned actor: at the back of its segment’s run queue, later in this run if the budget allows, else in the next;
  - a terminated actor exits at its next microstep boundary; the actor whose expression terminated itself does not run the rest of its macrostep.
- **Lookups answer during a run** (`ta_host_chart`, `ta_host_locate`, `ta_host_actor_by_name`, `ta_host_segment_count`). **Everything else is refused:** calls that change the host’s structure (adding images, hibernating, waking, adopting, migrating, snapshots), every request, the envelope and telemetry calls (`TA_ERROR_STATE`); `ta_host_effects` returns 0, `ta_host_effects_ack` and `ta_host_advance_time` do nothing, and `ta_host_run` returns `TA_RUN_FAULT`. An expression cannot read another actor’s state. Say so in the API.
- **One thread.** Parallelism is shared-nothing: several hosts, one per thread, exchanging envelopes. Do not reach for shared memory in a binding (in the browser it would require cross-origin isolation); a multi-threaded host would be a core decision first. A host is single-threaded: every call from the thread that made it. A threaded runtime confines it (one host per worker, an executor, a serial queue), and the binding checks it: record the creating thread and refuse calls from others. A global lock (Python’s GIL) is not a substitute: free-threaded builds have none. Keep the binding’s state per interpreter or isolate, never in C globals.
- **Freeing the host from an effect handler** is safe only after the loop returns: mark it, finish the loop, then free.
- **The datamodel vtable is the only call out of core.**

## 6 · Memory: who owns what, for how long

| What                                                                                       | Owner                                        | Valid until                                                                                                                                                                                              |
|--------------------------------------------------------------------------------------------|----------------------------------------------|----------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------|
| Every block of the host and its segments                                                   | the host, from `environment->allocator`      | `ta_host_free`, which returns all of them. A refused block leaves the host as it was: the call returns `TA_ERROR_NO_MEMORY`, a run returns `TA_RUN_EXHAUSTED` and the next run goes on where it stopped. Nothing grows during a run. |
| Effect records, their strings and payload views                                            | the host (borrowed)                          | `ta_host_effects_ack`, the next `ta_host_run` or `ta_host_free`. Copy what you keep.                                                                                                                     |
| The config bytes                                                                           | the caller                                   | the call to `ta_host_create`; nothing points into them afterwards.                                                                                                                                       |
| Views passed in (names, traceparents, envelope fields, an envelope’s `wire`)               | the caller                                   | the call; the host copies what it keeps.                                                                                                                                                                 |
| Payloads passed in                                                                         | moved to the host on success                 | the caller’s again on failure: release it. A payload from `from_wire` carries one reference.                                                                                                             |
| Image bytes                                                                                | the caller                                   | its segment is freed (`TA_HOST_EFFECT_SEGMENT_FREED`) or the host goes.                                                                                                                                  |
| A segment’s `ta_system` (`ta_host_system`)                                                 | the host (borrowed)                          | the next call that can grow or free it.                                                                                                                                                                  |
| A job’s pending sessions                                                                   | your `ta_uint64_array`                       | yours.                                                                                                                                                                                                   |
| What the host writes for you: blobs, snapshots, replies, envelopes, traces, logs, metrics  | the caller’s `ta_buffer`                     | yours. On `TA_ERROR_NO_MEMORY`, `length` is what it needs and nothing changed: grow and call again.                                                                                                      |
| Hibernation blobs in flight                                                                | the binding, until the store’s `put` settles | the put settling. A wake can arrive first: wake from the held copy (section 4).                                                                                                                          |

- **One convention for bytes.** Bytes go in as `ta_bytes` and come out through a `ta_buffer *` (`tinyactors/types.h`); no public function takes a pointer and a length (the lint rule `buffers-are-ta-buffer`). A reply, blob or request that does not fit is not applied.
- **Core never allocates after init below the host;** the host allocates only through its allocator, and only between runs; the CBOR functions allocate nothing.
- **The binding allocates almost nothing.** Allocations and frees must pair, so keep them few and in one place:
  - **Borrow what core or the language already holds.** A string or bytes passed into a call is a view for the call's duration; core copies what it keeps. Take the language's own bytes where it gives a view (CPython's `PyUnicode_AsUTF8AndSize`, the buffer protocol), and keep a string the datamodel hands back to core in a language object the session holds. Read what core supplies (system variables, a chart's states through `ta_image_*`) when it supplies it; do not copy it.
  - **Hold a language object instead of copying bytes.** The image bytes the compiler writes stay in a language object (a `bytes`, an `ArrayBuffer`) the binding references until `TA_HOST_EFFECT_SEGMENT_FREED`; dropping the reference frees them.
  - **What core cannot hold lives in the handle tables.** Language objects (a session's scope, a chart's compiled expressions, payload values, references to the runtime) cannot be in core's memory (no pointers, moved by `memcpy`, hibernated) and must be visible to the garbage collector. A session, chart or payload is a row of its table, not an allocation of its own; the tables grow by doubling, reuse freed slots, and are freed once, with the host.
  - **Where the runtime cannot give a view**, as Node-API cannot give UTF-8 without copying, use one buffer per host, reused by every call and grown rarely; never an allocation per call.
  - So the allocator is called only by the tables, the host's buffer, and the host's create and free; nothing per event, nothing per evaluation. A lint rule keeps allocation calls in those places.
- **Assemble records where crossing is costly.** Where each call into C is expensive (CPython, JNI), the extension reads an effect’s fields with the accessors and hands the language one record per effect.
- **Only C touches host structs.** Read effects with the accessors, and let the addon, which is C in every runtime (Node-API, the same C compiled into a wasm module, cffi), fill input structs. Never copy a struct layout into another language.
- **Garbage collection.** Free the host deterministically when the application stops it, and drop every language reference the binding holds (handlers, scopes, payloads, functions) then; offer the language’s scoped form (a context manager, `using`, `defer`). A finalizer is only a fallback: handlers often close over the host. Where the language has a cycle collector, let it see those references (the CPython slots `Py_tp_traverse` and `Py_tp_clear`).
- **Snapshots are not durability.** Hibernation and snapshots take actors out of memory for a while, and a snapshot lets a host be inspected; export moves actors to another host. A snapshot is raw region memory, valid only for the same build of the library: never offer one as a backup, a save file or crash recovery, and never write it somewhere it outlives the build. Durability is the application’s (a database). The hibernation store is where idle actors wait while out of memory, not a record of them: a store that outlives the host (IndexedDB, a file) keys blobs per host and clears them when a host is created and when it stops, since no other host can wake them.

## 7 · Time

- One monotonic clock in ticks, at the rate `host.config` answers as `system.ticksPerSecond` (a config without the key gets the system’s default, 1000000000: nanoseconds). Read it once, after creating the host, and convert the language’s clock with it, both ways: a time for `ta_host_advance_time`, and `next_deadline` back to the clock of the timer you arm. Never assume a rate of your own; a caller who wants milliseconds sets `ticksPerSecond` 1000 in the config. Advance the clock with the wall clock before every run and every input that depends on it (spawn, deliver, adopt).
- A host restored from a snapshot (same build, while its actors were out of memory) resumes at the snapshot’s time: start the clock there.
- A blob that leaves for another host is rebased to time 0 on export and to the receiver’s clock on adopt (`ta_blob_rebase`).
- Span times are wall-clock nanoseconds, interpolated between the `wall_ns` of two calls to `ta_host_advance_time`; 0 means unknown.

## 8 · Choosing a datamodel

- **A datamodel is chosen by name.** Core matches each registered vtable’s `name` against the chart’s `datamodel` attribute. A binding may define a datamodel of its own language under its own name (`"python"`), with no core change: charts that ask for it evaluate that language. A host takes up to eight datamodels (`environment->datamodels`), and each chart uses the one it names. Core never translates a value between datamodels: a payload goes only to the datamodel it came from, and event data that arrives from another system is read by the datamodel of the chart it is delivered to.
- **ECMAScript** (`ta_datamodel_ecmascript_vtable()`, name `"ecmascript"`, on quickjs-ng): nothing to implement and no calls into the language; values cross as CBOR. Every binding registers it beside its native datamodel, which comes first (its developers write chart logic in their language): both pass the same suite (section 12).
- **Native:** expressions run in the language. Implement the datamodel vtable (`tinyactors/datamodel.h`; every per-instance entry takes a `ta_datamodel_call`, `create` and `deserialize` a `ta_datamodel_start`, `<foreach>` a `ta_datamodel_loop`, and blobs go through `ta_buffer`/`ta_bytes`): compile a chart’s expressions once at `chart_attach` (by kind: condition, value, location, script), keep one scope per session, and two tables of handles, for payloads (with reference counts) and for sessions, each a slot and a generation, never a pointer, so every language reference the binding holds can be found and a stale handle is refused. `to_wire` and `from_wire` convert a payload to and from a neutral value (section 9). `serialize`/`deserialize` and `payload_serialize`/`payload_deserialize` write the datamodel’s own blob format for hibernation and export. `variable` answers the `actor.context` method. An expression that throws returns `TA_ERROR_EXECUTION`, so core raises `error.execution` as SCXML requires; pass the exception to the logger and invent no effect for it. An exception that is the runtime’s rather than the chart’s (Python’s `KeyboardInterrupt`) is kept and raised again after the run.
- **The scope keeps SCXML’s rules.** Assigning to an undeclared location, or to a system variable (`_event`, `_sessionid`, `_name`, `_ioprocessors`, `_x`), is `error.execution`. Where the language’s scope cannot refuse it, check after the evaluation and restore (Python’s globals dict), or compile so that it throws (JavaScript in strict mode inside `with (scope)`, as the ECMAScript datamodel does).
- **`release` can run the language’s code** (a finalizer, a weakref callback), and core calls it outside a run as well: at the start of `ta_host_run`, in the ack, in `ta_host_free`, in requests and in deliver, where reentry is not refused. Defer dropping the last language reference until the outer call into core returns.
- **Actors share no mutable value.** Inside a session a variable may hold any of the language’s objects. What crosses to another actor (captured `<send>` data, `<donedata>`, data sent from outside) is copied through the value table; immutable values may be shared. A mutable object outside the table fails the capture with `error.execution`; a session holding one is not hibernated (`serialize` answers `TA_ERROR_UNSUPPORTED`). Never serialize with a format that runs code when read (pickle, Java serialization).
- **Say what the language cannot bound.** A runtime that cannot be preempted mid-expression or limited in memory (CPython, unlike quickjs with `max_ops` and its allocator) makes its charts trusted code, and its values lie outside the host’s allocator. Charge fuel once per evaluation, and write both limits in the binding’s README.
- **Conformance:** translate the W3C suite’s expressions into the language (the charts unchanged) and run it on a host with the native datamodel as well as on one with ECMAScript.

## 9 · Values and envelopes: the CBOR module

- Decode with one call (`ta_cbor_decode`) into a token list in memory you own, then build the language’s values from it; encode the other way (`ta_cbor_encode`). One FFI call per message, never one per item. Build them in the extension’s C, where the language’s objects are reachable (the CPython C API, Node-API).
- **The value table** is [protocol.md, Values](protocol.md#values): integers, floats, text, bytes, arrays, maps with text keys, `false`, `true`, `null`, `undefined`, and tag 32 for an actor reference. Write down the language’s side of it. Where the language has no second empty value (Python’s only `None`), add a singleton for `undefined`.
- Integers are exact to 64 bits: use the language’s big integers where its numbers are doubles, and check a double before converting it (Node-API’s `napi_get_value_int64` saturates silently). Text is valid UTF-8: a lone surrogate is refused (`TA_ERROR_INVALID`), never replaced (Node-API replaces it with U+FFFD silently). Where a boolean is an integer (Python’s `bool`), test for it first. Byte strings are bytes, not base64. Tag 32 is an actor reference: where the language’s datamodel has a reference type, make it that object, which writes tag 32 again (Python’s `Address`; `values.txt`, “not ECMAScript” cases); where it has none, as in ECMAScript (Node’s datamodels both), read it as its address text and never write it ([protocol.md, Values](protocol.md#values); `values.txt`, “ECMAScript: an actor reference is its address”). `null` and `undefined` stay distinct. Writers use the shortest form of every integer, length and float; map keys stay in the order written.
- **Envelopes between processes** ([protocol.md, Envelope](protocol.md#envelope)):
  - out: `ta_host_envelope(host, effect, &buffer)` writes a `PEER` or `PEER_ERROR` effect as CBOR;
  - in: `ta_host_envelope_read(host, bytes, &envelope)` then `ta_host_deliver(host, peer, &envelope)`, which returns a `TA_ROUTE_*` outcome or a negative error. Deliver also when the read failed with `envelope.size` set (data that is not one well-formed item): `ta_host_deliver` refuses it as malformed and answers the sender with a `PEER_ERROR`; with `size` 0 there is nobody to answer, drop it. An error envelope: `ta_host_peer_error_read(bytes, &error)` (set its `peer`), then `ta_host_peer_error`;
  - the event data stays CBOR (`ta_envelope.wire`) until the host delivers it: the target chart’s datamodel reads it, and a relay forwards the bytes without reading them. Bytes that are not an envelope are `TA_ERROR_INVALID`; data the target cannot read is refused and answered with an error envelope to its origin.
- **Envelopes within one process** (two hosts in one process, one per worker) never touch CBOR: fill a `ta_envelope` from the `PEER` effect’s fields (copying `wire` when it is set) and call `ta_host_deliver`.
- How envelopes travel between processes (a socket, a queue, a database) is the application’s. Core speaks no transport protocol.
- **The CBOR module may be exposed** as the language's own `encode` and `decode` (Python's `tinyactors.encode(value) -> bytes` and `tinyactors.decode(data) -> object`, Node's `encode` and `decode` exports): the same value table and the same calls (`ta_cbor_encode`, `ta_cbor_decode`, one per message), not a second codec, each with its row in the mapping table. The binding's suite adapter frames the driver's calls with it and imports no other codec (section 12). The vectors then cross the binding's codec both ways, so the binding's own tests of its value table catch a value mapped wrong the same way in both directions.

## 10 · Requests

- **Everything rarely called is a method of `ta_host_request`:** trace configuration, watches, stats, the effective config, inspection, state values, a session’s variables, subscriptions and decoded spans. The catalogue is [requests.md](requests.md): every method, its params, defaults, replies and errors. An API item whose method is not there does not exist; what is not a method is listed there too.
- **The shape is JSON-RPC 2.0’s, without its `"jsonrpc"` member, in CBOR:** `{"method", "params", "id"}` in; `{"id", "result"}` or `{"id", "error": {"code", "message", "data": {"reason", "path"}}}` out. A request without `"id"` is a notification: applied, with no reply and no error reported. Pass an id whenever the caller wants the answer.
- An API method is one line: build the map from the caller’s arguments with the CBOR module, send it, return the `result`. An `error` becomes the language’s exception, with its `code`, `reason`, `path` and `message` unchanged. Match on `code` and `reason`, never on `message`.
- `TA_ERROR_NO_MEMORY` means nothing was applied: grow the reply buffer and send the same request again.

## 11 · Project and build

- A project per runtime at the repository root, named for it (`tinyactors-napi`, `tinyactors-cpython`), with its own `mise.toml` pinning the toolchain, so nobody else downloads it.
- **The conformance suite is shared, not code.** `tinyactors-test` holds the tests that are data (protocol vectors, self-checking scenarios, W3C) and runs them against one command per binding (`ROADMAP.md`, step 8). The binding’s own tests cover what the suite cannot see: memory failures, refusal during a run, threads, garbage collection, liveness.
- **Our names carry the binding’s prefix** (`ta_python_*`, `ta_napi_*`); the platform’s names appear only where it requires them.
- Liveness and timer accuracy are the runtime’s: whether a pending timer keeps the process alive, and how late a throttled timer fires (a hidden browser tab), are I/O behaviour, documented per runtime (Node: alive while a deadline is pending), not a policy. A late timer only makes delayed sends late; the next turn runs everything due, in order.
- Link the release’s libraries (the amalgamation, or the release sources compiled into a wasm module for browsers). The binding has no C of its own beyond its addon.
- **Wasm:** memory growth detaches every view, so the loader refreshes its views after any call that can allocate, and nothing else keeps one. Memory never shrinks: hibernation makes room for other actors, not a smaller footprint, so size for the peak. The page’s Content Security Policy can forbid `new Function`: offer the ECMAScript datamodel in a separate module for a strict policy, so pages that do not need it do not download it.
- Tasks as Ruby file tasks under `mise/tasks/`; its `test` task lists core’s sources in `#MISE sources` so a core change reruns it.
- Repository rules apply: acronyms in capitals or lowercase, at most three parameters for every C function the binding defines, static helpers included (a function whose signature a platform typedef fixes is exempt by that typedef), no serialization on the hot path, a `CHANGELOG.md` per released component, commits scoped by component.

## 12 · Testing

A binding is tested by the conformance suite, [tinyactors-test](../../tinyactors-test/README.md), run against its adapter: one command over the binding’s public API that speaks the [driver protocol](../../tinyactors-test/docs/driver.md) on its standard input and output (`mise run //tinyactors-test:run -- <command…>`). The adapter implements the methods; a method it does not offer answers -32601 and the cases that need it are skipped, not passed.

- **The protocol vectors** ([suites/protocol](../../tinyactors-test/suites/protocol/README.md)): `requests.txt`, `config.txt`, `envelopes.txt` and `values.txt`, byte-exact. The adapter takes each case’s bytes, sends them through the binding’s own calls into core (a `request(method, params)` that takes the language’s values decodes them first and encodes the reply again, as its API does), and the runner compares the answer with the expected bytes, where tag 39999 is a wildcard for one item of the named type. The `roundtrip` and `malformed` cases of `values.txt` go through the public path: the value is delivered in an envelope to the echo setup's chart (`envelope.deliver`), which sends `_event.data` back to `_event.origin`, and the data of that envelope is compared, so a binding's datamodel passes them with no method of its own; a datamodel in another language passes all but those named “ECMAScript: …”, an ECMAScript one all but those named “not ECMAScript: …”.
- **The scenarios** ([suites/scenarios](../../tinyactors-test/suites/scenarios/README.md)): core’s conformance scenarios as charts that check themselves, in the W3C IRP’s notation, and end in `pass` or `fail`. A `steps.txt` lists the host operations between runs (spawn, send, the clock, hibernation, snapshots, a new image, migration). The adapter runs them with the binding’s real loop and effect handlers in the environment the driver protocol describes (“The scenario environment”), and answers the first `Outcome` log. No effect is compared.
- **The W3C suite** runs through the adapter (`w3c.run`), with each datamodel the binding offers; every automatic test must pass except those on its language's skip list (`skip-<language>.txt`, each with a reason). Its environment has BasicHTTP as a synchronous I/O processor, which 12 tests use (201, 509, 510, 518, 519, 520, 522, 531, 532, 534, 567, 577): the binding's loop answers `TA_RUN_EFFECTS` (section 3) by the time its W3C gate runs (`ROADMAP.md`, step 8, M2).
- **The differential run**: the same scenarios and W3C suite run on the binding’s native datamodel and on quickjs, and both pass. No outcome or effect is compared beyond that.
- What a black box cannot show stays in the binding’s own tests: memory failures, refusal during a run, threads, garbage collection, liveness.
- Tests run in parallel: listen on port 0, write to temporary directories, depend on no other test.
- A test that compares timings runs alone.

## 13 · Done means

- The mapping table exists, and every API item has a core function or method behind it.
- The queue loop handles every run status and every effect in section 4.
- The suite passes against the binding's command: protocol vectors, scenarios and W3C, on every datamodel it offers.
- No defaults, limits, methods or messages of the binding’s own.
- Nothing C does is done again in the language.
