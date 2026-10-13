---
name: boundary-design
description: Design or change the interface between tinyactors core and a language binding (host.h, datamodel.h, cbor.h, the effect records, requests and posted messages, how inputs, outputs, values and memory cross). Use before proposing or answering anything about how a binding calls core, how bytes or values cross, buffers, views, queues, rings, records, shared memory, ownership of payloads or images, reentrancy, or a binding's memory strategy; and before sending a binding session a change of direction. Works with the reasoned-analysis skill, which it assumes.
---

# Designing the core ↔ binding boundary

This boundary was redesigned several times in one day (2026-10-12): paired
`malloc`/`free`, then shared scratch, then rooms with retries, then
ADR 0010; then a ring, a step call and a posted queue were proposed and
withdrawn. Each turn cost the binding sessions work. The facts below are
what those turns established; start from them, and change one only with
evidence.

The reference for the current boundary, with every rejected alternative
and its reason, is `tinyactors-core/docs/boundary.md`; keep it current.

## The facts of the boundary

Check each against the code before relying on it; cite the line.

- **One owning thread per host.** Every call comes from the thread that
  made the host (guide section 5). Reentrancy happens only on that thread:
  a getter, `toJSON`, `__del__`, a cycle-collector finalizer, a datamodel
  expression. There is no other thread, no other process, no shared
  memory producer. An interrupt handler on a microcontroller is the one
  producer that cannot call the host: be ready for it with a ring beside
  core that drains into the existing calls, and do not design around it.
- **Inputs are read during their call.** `ta_host_send`, `_send_many`,
  `_spawn`, `_terminate`, `_resume`, `_deliver` take C structs whose views
  core copies before returning (`src/system/schedule.c` send path). A
  payload is a handle that moves to core on success and stays the
  caller's on refusal. Batching means deferring the run, not the reads:
  several calls, then one `ta_host_run`.
- **Values are handles, metadata are views.** Event data is a
  `ta_payload` (a row in the binding's table for its own datamodel);
  names, sendids, origins and traceparents are `ta_string_view`s read at
  the call.
- **Effects are host-owned records**, read through accessors by field id,
  valid until `ta_host_effects_ack`. Only C reads them.
- **Outputs are views** (ADR 0010): a datamodel's until its next call,
  the host's until the next call into the host. Core asks for each value
  once.
- **The datamodel vtable is the only call out of core**, guarded outside
  runs by the run's rules (ADR 0010, decision 3).
- **All binding code is C.** Nothing sits above the extension (ROADMAP
  step 8, decision 2); every consumer of a header is C, so layouts are
  hand-written public headers held by the ABI rules, never generated.
- **Two systems cross, never three:** fixed records read through
  accessors, and CBOR. JSON-RPC names the shape of control messages,
  replies and notifications; their bytes are CBOR. Nothing core gives or
  takes is JSON.
- **No serialization on the hot path** (`AGENTS.md`). CBOR is for the
  control plane, the config, and bytes that leave the process.
- **A binding's memory** is its handle tables, one CBOR writer per call,
  a datamodel's last output, and views (guide section 6). The allocator
  is called in those places only, checked by a lint rule.

## Ideas already rejected, and why

Do not re-propose one without new evidence that answers its reason.

| Idea | Rejected because | Shown by |
|---|---|---|
| Inputs as records in a queue or ring read at a later `step` | deferring the read needs held references or copies per record, refusals after the call returned, and a queue in core; nothing needs it, since batching comes from deferring the run | the file-reading walk-through; ADR 0004 (the ring protocol) |
| `ta_host_step(inputs[], budget)` | a wrapper over `send_many` plus `run` that adds nothing for C callers | `host.h` send_many contract |
| Records with tail offsets | needed only when a record outlives its call (another process, an interrupt) | the lifetime trace |
| Generated schemas or accessors | every consumer is C | the binding list |
| A language layer writing records or decoding values | it is the layer step 8 removed; a second codec beside `values.c` | ROADMAP step 8, decision 2 |
| Token lists for bindings | caller-sized scratch; a call per item costs ~3 ns even in wasm | ADR 0010 measurements |
| Caller-sized `ta_buffer` outputs with retries | core ran datamodel code two or three times per output | ADR 0010 context |
| Scratch shared between calls | not reentrant: a nested request corrupted the outer one | napi `8d600f4c` |
| Holding the caller's image bytes | transfer, resize or sharing made core read freed memory | napi `38fc05c9` |
| Cross-thread hazards in a binding | the host has one owning thread | guide section 5 |
| Struct layouts read from Python or JS | ADR 0009, "Only C touches host structs" | ADR 0009 |

## Steps

1. **Load reasoned-analysis** and write its frame (step 0) from the facts
   above.
2. **Name the existing call** that does the job, and the scenario in which
   it does not suffice. No scenario: no new mechanism.
3. **Trace lifetimes in a table** for every object that crosses: owner,
   created, read, freed, thread. Use the real calls.
4. **Cost per event, per call, per actor, in memory and API surface**,
   and who pays (docs/roles.md), as an ADR states it.
5. **Spike** what is measured or what a runtime does; refute with a fresh
   agent before recommending.
6. **Write it once**, as a proposal or ADR with "not X, because Y, as Z
   shows" for every alternative, before any code or any message to a
   binding session. Design in a document, not across replies.
7. **One change of direction per milestone to the sessions.** Collect
   changes, have the maintainer decide them, then send each session one
   message with the decided model and the call sites to migrate. Run the
   milestone audit (both bindings side by side) before the next.
