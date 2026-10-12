# The core ↔ binding boundary

**Status:** Current (2026-10-13). Records [ADR 0009](../../docs/adr/0009-one-host-two-queues.md),
[ADR 0010](../../docs/adr/0010-outputs-are-views-values-stream.md) and
[ADR 0011](../../docs/adr/0011-the-control-plane-is-posted-messages.md).
Where ADR 0011 is not implemented yet, a section says so.

How a binding and core talk: what crosses, who owns it, for how long,
and every alternative that was considered and rejected, with the reason
and what showed it. [bindings.md](bindings.md) is the guide for writing
a binding; this page is the reference for changing the boundary. Change it
only with the skills `reasoned-analysis` and `boundary-design`
(`.agents/skills/`), and record the change here.

## The frame

Every decision below rests on these facts; a proposal that needs one of
them to be false has to show it is.

- **One host, one owning thread.** Every call comes from the thread that
  made the host (bindings.md section 5). Reentrancy happens only on that
  thread: a getter, `toJSON`, `__del__`, a finalizer, an expression.
- **Each binding is its extension's C**, with nothing above it in the
  language (ROADMAP.md step 8, decision 2). Every consumer of a header is
  C: the CPython extension, the Node-API addon, the same addon compiled to
  wasm with emnapi, `ta-conform`.
- **The datamodel vtable is the only call out of core.**
- **Core is the only queue.** Mailboxes, the run queue and timers are
  core's (ADR 0004).

## 1. Inputs

**Design.** `ta_host_send`, `ta_host_send_many`, `ta_host_spawn`,
`ta_host_terminate`, `ta_host_resume`, `ta_host_deliver`. Each takes a C
struct (`ta_event_in`, `ta_spawn`, ...) read during the call: core copies
the views (name, sendid, origin, origintype, raw, traceparent) into the
system's slab and takes the payload handle (`src/system/schedule.c`, send
path). A payload moves to core on success and is the caller's again on
refusal; `send_many` returns the accepted prefix. Batching defers the
run, not the reads: several calls, then one `ta_host_run`.

| Object | Owner | Created | Read | Ends |
|---|---|---|---|---|
| the input struct | caller (C stack) | before the call | during the call | the call returns |
| its views | the language's objects (a `str`'s UTF-8; Node: the call's text room) | before the call | copied during the call | the call returns; core keeps its copies |
| its payload | the binding's row | before the call | taken during the call | core's on success; the caller's on refusal |

**Rejected.**

- **Encoding each input as bytes (CBOR):** serialization on the hot path
  (`AGENTS.md`). Note: fixed-layout records are not encoded; the
  objection is to encoding, not to bytes.
- **Records in a queue or ring read at a later step:** deferring the read
  means held references (Python) or copies (Node) per queued record,
  refusals that arrive after the call returned, and a queue in core;
  nothing needs it, because batching comes from deferring the run. Shown
  by tracing a file-reading example; the ring protocol was tried and
  superseded (ADR 0004).
- **`ta_host_step(inputs[], budget)`:** `send_many` plus `run` with nothing
  added for C callers; one crossing per batch is worth ~3 ns per call even
  in wasm (ADR 0010 measurements).
- **Tail offsets instead of views:** needed only when a record outlives
  its call (another process, an interrupt handler); every view here
  outlives its read.
- **A generated schema and accessors:** every consumer is C; the public
  header, the ABI rules and `tests/unit/test_headers.c` fix the layout.
- **A language layer writing records:** the layer step 8 removed.
- **Producers that cannot call the host.** Other threads and processes do
  not occur. An interrupt handler on a microcontroller would write
  self-contained records into a ring the main loop drains into these
  calls: a layer beside core, which core need not change for.

## 2. Running and time

**Design.** The loop of bindings.md section 3: `ta_host_advance_time`,
`ta_host_run(budget)`, effects, ack, one timer at `next_deadline`. The
tick rate comes from the effective config (`host.config`). Each binding
offers `SimulatedClock.increment(ms)`. Node's timer is the global
`setTimeout`.

**Rejected.** A default tick rate in a binding (a default of its own; the
system's 1e9 is stated in host-config.md). libuv timers (Bun panics on
`uv_timer_init`, oven-sh/bun#18546). A fatal status for signals (a signal
is an event on the loop and a run is bounded: shutdown is between turns).

## 3. Effects

**Design.** Host-owned records (`ta_host_effect`), read through
accessors by field id (`ta_host_effect_number`, `_text`, `_bytes`),
published by `ta_host_effects` (everything appended since the last ack,
run or not) and valid until `ta_host_effects_ack`, the next run or
`ta_host_free`. A binding's drain re-reads until the count is stable and
acks only at its outermost level (ADR 0011, decision 6).

**Rejected.** Struct layouts read from Python or JavaScript (`memoryview`,
typed arrays): "Only C touches host structs" (ADR 0009), the ABI coupling
it was written against.

## 4. The datamodel

**Design.** A native datamodel keeps handle tables (sessions, charts,
payloads, loops), since language objects cannot live in memory moved by
`memcpy` and the collector must see them. Its outputs (`to_wire`,
`serialize`, `payload_serialize`, `eval_string`, `log`) are views it owns
until its next call; core asks for each value once (ADR 0010, decision 1).
While core is inside a datamodel outside a run, calls back into the host
follow the run's rules (decision 3).

**Rejected.** Caller-sized outputs with retries: core ran datamodel code
two or three times per output (`router.c`, `hibernate.c`), and a 1 KB
envelope walked its value six times. Strings handed back into a buffer
core provides: user expressions cannot run twice. No guard: a `toJSON`
that terminated its actor left core calling a destroyed instance (an ASan
crash, `tests/unit/test_host_guard.c`).

## 5. Values and CBOR

**Design.** `ta_cbor_writer` writes items into a room that grows through
an allocator; `ta_cbor_reader` is a cursor (ADR 0010, decision 4). Token
lists remain only as the random-access index requests are parsed with.
Inside the process, values cross by copy through the value table between
the API and the binding's own datamodel; CBOR only toward quickjs and in
envelopes.

**Rejected.** Token lists in bindings (caller-sized scratch; streaming is
faster: 2.5 vs 3.7 µs encode, 6.7 vs 8.5 µs decode for 1 KB, no 294 KB of
tokens for 64 KB). `ta_cbor_encode` (deleted). A codec in the language.

## 6. The control plane

**Design (ADR 0011; not implemented yet).** `ta_host_post(h, message)`:
a JSON-RPC 2.0 message without `"jsonrpc"`, in CBOR, answered at the
call; its reply is the last effect the call appends,
`TA_HOST_EFFECT_REPLY`. Core's notifications are
`TA_HOST_EFFECT_NOTIFICATION` effects; the first is `actor.changed`
`{"session", "value"}` at the end of a run, replacing the host-level
CHANGED. A binding's synchronous `request` is a facade: post, take its
REPLY at the last index, drain the rest. A JSON-RPC transport is a pipe:
frames to `ta_host_post`, REPLY and NOTIFICATION bytes to the socket.

Until it lands: `ta_host_request(h, request, &reply)` answers with a view
of host memory valid until the next call into the host (ADR 0010).

**Rejected.** Keeping `ta_host_request` beside `ta_host_post` (two ways;
a view that ends at the next call). A queue of posted messages (answering
at the call orders the reply as well). Requests on an input queue (the
deferred read of section 1). Binary records for the control plane (the
catalogue grows; JSON-RPC faces outward). "Valid until released" for
replies as a separate rule (with replies as effects, the ack is that
release). Context in notifications (user code, unbounded size). Batches
in core.

## 7. Host outputs

**Design.** `ta_host_envelope`, `ta_host_traces`, `ta_host_logs`,
`ta_host_metrics`, `ta_host_hibernate`, `ta_host_export_actor`: views of
host memory valid until the next call into the host (ADR 0010, decision
2). `ta_host_snapshot`, `ta_host_snapshot_pack` and `environment->error`
keep `ta_buffer` (size known up front; no host yet). Hibernate and export
are written in one pass through the host's allocator, between runs only
(tinyactors-core/AGENTS.md, "Region memory is fixed at ta_system_init").

**Not yet decided.** Handing over ownership of large outputs (an OTLP
batch as a block the language wraps without a copy): unmeasured; it pays
only for large outputs.

## 8. Images

**Design (ADR 0011, decision 7; not implemented yet).**
`ta_host_add_image(h, ta_bytes image, &segment)`: the host copies the
bytes into a block it owns, opens them there, and frees the block when
the segment is freed. The binding keeps nothing.

Until it lands: the caller keeps the opened `ta_image` and its bytes,
unmoved, until `TA_HOST_EFFECT_SEGMENT_FREED`; bindings hold core's
`ta_compile_result` (Python) or a copy (Node).

**Rejected.** Holding a caller's language buffer (transfer, resize or a
shared buffer made core read freed memory: napi `38fc05c9`).
`ta_compile_image` returning the `ta_image` the result holds (kept the
lifetime the binding's). A borrow mode (zero-copy embedders use
`ta_system`, whose image stays borrowed).

## 9. A binding's memory

**Design.** Its handle tables, one CBOR writer per call (a C stack room
grown through the allocator, freed before the call returns), a
datamodel's last output, and views. The allocator is called only there
and in the host's create, free and allocator, enforced by each binding's
lint rule `allocation-in-tables-and-host`.

**Rejected.** Paired `malloc`/`free` at every call site (Node had 104,
with allocations per call and per evaluation). One arena for the host's
lifetime (grows without bound as actors come and go). Scratch shared
between calls (not reentrant: a nested request corrupted the outer one,
napi `8d600f4c`). Rooms with length hints and retries (workarounds for
caller-sized outputs, gone with ADR 0010). Cross-thread free and
collector-switch hazards (one owning thread).

## Changing this page

A change to the boundary is an ADR first, then this page, then the
guide, then the code; the bindings migrate in one message per change,
after the maintainer has read the ADR (`boundary-design`, step 7).
