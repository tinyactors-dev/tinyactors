# Writing a binding

**Status:** Proposed (2026-10-11). The draft of `tinyactors-core/docs/bindings.md`, part of [One host, two queues](README.md).

Everything a binding author needs, in one place: the rules from today’s `bindings/README.md`, the run-loop lessons of the last two restructures, and the interface this proposal adds. It replaces `bindings/README.md` and is written before any binding is regenerated, so the regenerated bindings follow it rather than the code they replace. Items marked *(proposed)* exist only once this proposal lands.

## 1 · What a binding is

A binding lets one language drive `ta_host`, in every runtime that language has. It has three parts and an API, and under them one addon per runtime that exposes the host’s calls (Node-API, wasm exports, ctypes, JNI or the FFM API, a Swift C target). JavaScript has one package for Node, Bun and browsers; only the addon differs.

- **The queue loop:** feed inputs, run the host, read its effects, do the I/O they ask for, acknowledge them, arm one timer.
- **A datamodel, or none:** the language’s own values and expressions behind the datamodel vtable, or the quickjs datamodel, which needs nothing from the binding.
- **A CBOR module** *(proposed)*: one generic conversion between a CBOR token list and the language’s values, used for event data, requests and replies.
- **The API:** what the language’s developers call. It is thin: each item is a host input, a request, or the handling of an effect.

## 2 · What a binding never does

- **It exposes the core, nothing else.** Before designing the API, inventory the public headers it drives and write the mapping table: each API item next to the core functions, effects and flags behind it. An item with no core function behind it does not belong. A core function with no item is used internally (say where) or not exposed (say why).
- **It borrows shape, never semantics.** Another library (xstate for JavaScript) may suggest names and call shapes: `createActor`, `send`, `getSnapshot`, `subscribe`. What a call does is what the core does. Never emulate another library’s behaviour, rename the core’s events, or lower its concepts onto SCXML.
- **It invents nothing:** no event names, flags, fields, defaults or mechanisms of its own. A real gap is a core proposal, not part of a binding.
- **It keeps no twin of C.** SCXML semantics, queues, routing, timers, the invoke lifecycle, addresses, trace policy, encodings: if C does it, bind the C.
- **It reads the core before claiming a gap.** `src/core` and `src/system` implement all of SCXML, including I/O processors, external invokes, sends to `#_<invokeid>` and error events. Cite the lines of `docs/SCXML.md` and the core before saying “the core cannot”.
- **It decides no policy.** Defaults, limits, validation and their error messages live in core and come back in replies. Retry and backoff for the binding’s own I/O (an HTTP post, a reconnect) are the exception: that is I/O.

## 3 · The queue loop

```
on wake (timer fired, input arrived, I/O completed):
    ta_host_advance_time(host, { monotonic_ms, wall_ns })   # proposed: both clocks
    repeat:
        run = ta_host_run(host, budget)
        for each effect in ta_host_effects(host):
            handle(effect)                  # copy what you keep: views end at ack
        ta_host_effects_ack(host)
        TA_RUN_EFFECTS_FULL → the effect list was full: run again now
        TA_RUN_EFFECTS      → answer the sync sends (they are inputs), run again now
        TA_RUN_YIELDED      → give the event loop a turn, wake again at once
        TA_RUN_EXHAUSTED    → a pool could not grow (run.detail names it): report it
        TA_RUN_FAULT        → a segment could not run (out of memory, or a call
                              into it during a run): report it
        TA_RUN_IDLE         → arm one timer at run.next_deadline (UINT64_MAX: none)
    whatever a handler raised: ack anyway, finish the turn, then raise the first error
```

- **One timer,** re-armed after every loop; never one per actor or per deadline.
- **Bound each turn.** The budget caps work per run; across many runs, yield to the runtime’s event loop (a macrotask in a browser, a callback in Node, a Looper message on Android) so a busy system never blocks it.
- **A caller’s own clock** (simulation, tests) replaces the timer: time moves only when the caller advances it and runs the loop.
- **Not every program has an event loop.** Offer the same turn under two drivers where the language has both: a synchronous one (run until idle, sleeping to the next deadline or following the caller’s clock) and the language’s event loop (asyncio, a Looper, a dispatch queue).

## 4 · Effects: what to do with each

| Effect                 | The binding                                                                                                                                                                                   | Answers with                                                                                                             |
|------------------------|-----------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------|--------------------------------------------------------------------------------------------------------------------------|
| `CHANGED`              | *(proposed)* A subscribed session finished a macrostep: tell its subscribers. Only sessions the binding subscribed with a request produce it.                                                 | Nothing.                                                                                                                 |
| `SEND`                 | Hand it to the I/O processor named by `io_processor`. A `TA_IO_PROCESSOR_SYNC` processor answers before the next run.                                                                         | An event, if the processor replies; on failure an event with `TA_EVENT_IN_ERROR_COMMUNICATION` and the send’s `sendid`.  |
| `INVOKE`               | Start the service of that type, keyed by `(segment, token)`. Every send to an invoked service carries its token *(proposed)* (the host resolves `#_<invokeid>`): keep no table of invoke ids. | Its events with `TA_EVENT_IN_FROM_INVOKE` and the token; at its end `TA_EVENT_IN_INVOKE_DONE`, or a communication error. |
| `UNINVOKE`             | Stop that service; forget the key.                                                                                                                                                            | Nothing.                                                                                                                 |
| `DONE`, `ERROR`, `LOG` | Tell the application: the actor’s output or error code, a `<log>` line.                                                                                                                       | Nothing.                                                                                                                 |
| `WAKE`                 | Load the actor’s blob from the store. If the binding still holds that blob (its `put` has not settled yet), wake from the held copy and delete the store’s copy when the put settles.         | `ta_host_wake` with the blob.                                                                                            |
| `REQUEST`              | *(proposed)* Answered by the host itself (it has the images); today the binding looks up the chart and resumes.                                                                               | —                                                                                                                        |
| `JOB_DONE`             | Settle the migration the application started (its promise, future or callback) with migrated, failed and the pending sessions.                                                                | Nothing.                                                                                                                 |
| `SEGMENT_FREED`        | Release the image the binding kept for that segment.                                                                                                                                          | Nothing.                                                                                                                 |
| `PEER`, `PEER_ERROR`   | An envelope for another system, as CBOR *(proposed)*. Carrying it is the application’s; the binding hands it over.                                                                            | `ta_host_deliver` for envelopes that arrive.                                                                             |
| traces ready           | *(proposed)* Call `ta_host_traces` with a buffer of your own, then post it (`application/x-protobuf` to `/v1/traces`) or give it to a sink. Retries and a cap on what waits are yours.        | Nothing.                                                                                                                 |

## 5 · Inputs and reentrancy

- **Between runs, any call is fine,** including from an effect handler before the ack: effects already taken stay where they are, and effects the call makes come with the next `ta_host_effects`. This is today’s contract.
- **During a run** (a datamodel entry evaluating an expression), today every call returns `TA_ERROR_STATE`. *(proposed)* `ta_host_send`, `ta_host_send_many`, `ta_host_spawn` and `ta_host_terminate` become legal there: an event is appended to its target’s mailbox and an actor takes a slot from the pool, exactly as an internal `<send>` or `<invoke>` does mid-run. No queue of their own and no copy. Nothing grows during a run, so a full pool answers `TA_ERROR_NO_MEMORY` and the payload stays the caller’s.
- **Calls that change the host’s structure** (adding images, hibernating, waking, adopting, migrating, snapshots, freeing) and **every request**, inspection included, stay refused during a run (`TA_ERROR_STATE`): an expression cannot read another actor’s snapshot. Say so in the API.
- **One thread.** Parallelism is shared-nothing: several hosts, one per thread, exchanging envelopes. Do not reach for shared memory in a binding (in the browser it would require cross-origin isolation); a multi-threaded host would be a core decision first. A host is single-threaded: every call from the thread that made it. A threaded runtime confines it (one host per worker, an executor, a serial queue), and the binding checks it: record the creating thread and refuse calls from others. A global lock (Python’s GIL) is not a substitute: free-threaded builds have none. Keep the binding’s state per interpreter or isolate, never in C globals.
- **Freeing the host from an effect handler** is safe only after the loop returns: mark it, finish the loop, then free.
- **The datamodel vtable is the only call out of core.**

## 6 · Memory: who owns what, for how long

| What                                                                                                               | Owner                                        | Valid until                                                                                                                                                                                              |
|--------------------------------------------------------------------------------------------------------------------|----------------------------------------------|----------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------|
| Every block of the host and its segments                                                                           | the host, from `config.allocator`            | `ta_host_free`, which returns all of them. A refused block leaves the host as it was: the call returns `TA_ERROR_NO_MEMORY`, a run returns `TA_RUN_EXHAUSTED` and the next run goes on where it stopped. |
| Effect records, their strings and payload views                                                                    | the host (borrowed)                          | `ta_host_effects_ack`, the next `ta_host_run` or `ta_host_free`. Copy what you keep.                                                                                                                     |
| Views passed in (names, traceparents, envelope fields)                                                             | the caller                                   | the call; the host copies what it keeps.                                                                                                                                                                 |
| Payloads passed in                                                                                                 | moved to the host on success                 | the caller’s again on failure: release it. A payload from `from_wire` carries one reference.                                                                                                             |
| Image bytes                                                                                                        | the caller                                   | its segment is freed (`SEGMENT_FREED`) or the host goes.                                                                                                                                                 |
| A segment’s `ta_system` (inspection)                                                                               | the host (borrowed)                          | the next call that can grow or free it.                                                                                                                                                                  |
| Names in stats; a job’s pending sessions                                                                           | the host (borrowed)                          | the next mutating call; the next `ta_host_run`.                                                                                                                                                          |
| What the host writes for you: blobs, snapshots, state values; requests’ replies, envelopes and traces *(proposed)* | the caller’s `ta_buffer`                     | yours. On `TA_ERROR_NO_MEMORY`, `length` is what it needs and nothing changed: grow and call again.                                                                                                      |
| Hibernation blobs in flight                                                                                        | the binding, until the store’s `put` settles | the put settling. A wake can arrive first: wake from the held copy (section 4).                                                                                                                          |

- **One convention for written output** *(proposed)*: today it is `buffer, capacity, *length` (`ta_host_hibernate`, `ta_host_export_actor`, `ta_host_snapshot`, `ta_host_snapshot_pack`) or a returned length (`ta_host_state_value`). All of them take a `ta_buffer`, which groups those three fields with the same meaning.
- **base … system never allocate after init;** the host allocates only through its allocator; the CBOR functions allocate nothing.
- **Assemble records where crossing is costly.** Where each call into C is expensive (CPython, JNI), the extension reads an effect’s fields with the accessors and hands the language one record per effect.
- **Only C touches host structs.** Read effects with `ta_host_effect_number` and `ta_host_effect_text` *(proposed)* (by `TA_EFFECT_FIELD_*`), and let the addon, which is C in every runtime (Node-API, the same C compiled into a wasm module, cffi), fill input structs. Never copy a struct layout into another language.
- **Garbage collection.** Free the host deterministically when the application stops it, and drop every language reference the binding holds (handlers, scopes, payloads, functions) then; offer the language’s scoped form (a context manager, `using`, `defer`). A finalizer is only a fallback: handlers often close over the host. Where the language has a cycle collector, let it see those references (Python’s `tp_traverse`/`tp_clear`).

- **Snapshots are not durability.** Hibernation and snapshots take actors out of memory for a while, and a snapshot lets a host be inspected; export moves actors to another host. A snapshot is raw region memory, valid only for the same build of the library: never offer one as a backup, a save file or crash recovery, and never write it somewhere it outlives the build. Durability is the application’s (a database). The hibernation store is where idle actors wait while out of memory, not a record of them: a store that outlives the host (IndexedDB, a file) keys blobs per host and clears them when a host is created and when it stops, since no other host can wake them.

## 7 · Time

- One monotonic clock in milliseconds (`ticks_per_second = 1000`), advanced before every run and every input that depends on it (spawn, deliver, adopt).
- A host restored from a snapshot (same build, while its actors were out of memory) resumes at the snapshot’s time: start the clock there.
- A blob that leaves for another host is rebased to time 0 on export and to the receiver’s clock on adopt (`ta_blob_rebase`).
- Trace times are wall-clock nanoseconds, given with the monotonic time in the same call *(proposed)*.

## 8 · Choosing a datamodel

- **A datamodel is chosen by name.** Core matches the registered vtable’s `name` against the chart’s `datamodel` attribute. A binding may define a datamodel of its own language under its own name (`"python"`), with no core change: charts that ask for it evaluate that language. A host runs one datamodel; mixing datamodels in one host is not supported, so a binding that offers both its own and quickjs lets the application pick one per host.
- **quickjs:** nothing to implement and no calls into the language; values cross as CBOR. The default for a new binding.
- **Native:** expressions run in the language. Implement the datamodel vtable: compile a chart’s expressions once at `chart_attach` (by kind: condition, value, location, script), keep one scope per session, and a payload table of handles with reference counts (a slot and a generation, never a pointer). `to_wire`/`from_wire` and `payload_serialize`/`_deserialize` write CBOR *(proposed)*: they carry envelopes and blobs. Register an expression checker with the compiler under the same name, so a broken chart fails at compile time. An expression that throws returns `TA_ERROR_EXECUTION`, so core raises `error.execution` as SCXML requires; pass the exception to the logger and invent no effect for it.
- **Actors share no mutable value.** Inside a session a variable may hold any of the language’s objects. What crosses to another actor (captured `<send>` data, `<donedata>`, data sent from outside) is copied through the value table; immutable values may be shared. A mutable object outside the table fails the capture with `error.execution`; a session holding one is not hibernated (`serialize` answers `TA_ERROR_UNSUPPORTED`). Never serialize with a format that runs code when read (pickle, Java serialization).
- **Say what the language cannot bound.** A runtime that cannot be preempted mid-expression or limited in memory (CPython, unlike quickjs with `max_ops` and its allocator) makes its charts trusted code, and its values lie outside `config.allocator`. Charge fuel once per evaluation, and write both limits in the binding’s README.
- **Conformance:** translate the W3C suite’s expressions into the language (the charts unchanged) and run it on a host with the native datamodel as well as on one with quickjs.

## 9 · Values: the CBOR module

- Decode with one call into a token list in memory you own, then build the language’s values from it; encode the other way. One FFI call per message, never one per item. Build them where it is cheapest: in the language (JavaScript), or in the extension’s C where the language’s objects are reachable from C (CPython’s C API).
- Write down the language’s value table: integers, floats, bytes, text, arrays, maps, tag 32 (actor reference), `null` and `undefined`. Where the language has no second empty value (Python’s only `None`), add a singleton for `undefined`.
- Integers are exact to 64 bits: use the language’s big integers where its numbers are doubles. Byte strings are bytes, not base64. Tag 32 is an actor reference: make it the language’s reference object. `null` and `undefined` stay distinct.
- Map keys in requests and replies are text.

## 10 · Requests

- Everything rarely called (trace configuration, watches, stats, metrics, inspection, state values, snapshot contents, decoded spans) is `ta_host_request` *(proposed)*: a CBOR map with `"op"` in, a CBOR map out.
- Every operation, its fields, defaults, replies and error paths are in `tinyactors-core/docs/requests.md` *(proposed)*, next to the vectors that test them. An API item whose operation is not there does not exist.
- **Metrics are pulled.** Core keeps the counters; `ta_host_metrics` *(proposed)* writes them, as an OTLP metrics request, into your buffer whenever you call it. There is no metrics timer or effect: read them when the application asks, on `stop()`, or on a cadence the application chooses.
- An API method is one line: build the map from the caller’s arguments with the CBOR module, send it, return the reply. An `"error"` reply becomes the language’s exception, with its `path` and `message` unchanged.

## 11 · Project and build

- A project per runtime at the repository root, named for it (`tinyactors-napi`, `tinyactors-browser`, `tinyactors-cpython`), with its own `mise.toml` pinning the toolchain, so nobody else downloads it. Where several runtimes share a language, the shared package is a project of its own (`tinyactors-js`) and each runtime’s project holds only its addon.
- **Each runtime’s addon is its own C, with one function list.** The Node-API addon and the wasm addon cannot share a file (one is written against `napi_value`, the other against wasm numbers and pointers), but they export the same functions, and the shared package runs one test suite against each.
- Liveness and timer accuracy are the runtime’s: whether a pending timer keeps the process alive, and how late a throttled timer fires (a hidden browser tab), are I/O behaviour, documented per runtime (Node: alive while a deadline is pending), not a policy. A late timer only makes delayed sends late; the next turn runs everything due, in order.
- Link the release’s libraries (the amalgamation, or the release sources compiled into a wasm module for browsers). The binding has no C of its own beyond its addon.
- **Wasm:** memory growth detaches every view, so the loader refreshes its views after any call that can allocate, and nothing else keeps one. Memory never shrinks: hibernation makes room for other actors, not a smaller footprint, so size for the peak. The page’s Content Security Policy can forbid `new Function`: offer the quickjs datamodel in a separate module for a strict policy, so pages that do not need it do not download it.
- Tasks as Ruby file tasks under `mise/tasks/`; its `test` task lists core’s sources in `#MISE sources` so a core change reruns it.
- Repository rules apply: acronyms in capitals or lowercase, at most three parameters per C function, a `CHANGELOG.md` per released component, commits scoped by component.

## 12 · Testing

- **Request and reply vectors** *(proposed)* in `tests/conformance`: the binding must reproduce every pair.
- **The conformance scenarios and the W3C suite** run through the binding, with each datamodel it offers; all 191 W3C tests must pass.
- Tests run in parallel: listen on port 0, write to temporary directories, depend on no other test.
- A test that compares timings runs alone.

## 13 · Done means

- The mapping table exists, and every API item has a core function behind it.
- The queue loop handles every run status and every effect in section 4.
- The vectors, the conformance scenarios and W3C pass.
- No defaults, limits or messages of the binding’s own.
- Nothing C does is done again in the language.
