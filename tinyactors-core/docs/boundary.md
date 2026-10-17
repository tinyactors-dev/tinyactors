# The core ↔ binding boundary

**Status:** Current (2026-10-13). Records [ADR 0009](../../docs/adr/0009-one-host-two-queues.md),
[ADR 0010](../../docs/adr/0010-outputs-are-views-values-stream.md),
[ADR 0011](../../docs/adr/0011-the-control-plane-is-posted-messages.md) and
[ADR 0012](../../docs/adr/0012-lifetimes-follow-the-loop.md), which step
8.2 of `ROADMAP.md` implemented.

How a binding and core talk: what crosses, who owns it, for how long,
and every alternative that was considered and rejected, with the reason
and what showed it. [embedding.md](embedding.md) is how to use the core;
[bindings.md](bindings.md) is the guide for writing a binding; this page is
the reference for changing the boundary. Change it only with the skills
`reasoned-analysis` and `boundary-design` (`.agents/skills/`), and record
the change here.

## The frame

Every decision below rests on these facts; a proposal that needs one of
them to be false has to show it is.

- **One host, one owning thread.** Every call comes from the thread that
  made the host (bindings.md section 5). Reentrancy happens only on that
  thread: a getter, `toJSON`, `__del__`, a finalizer, an expression.
- **Each binding is its extension's C** (ROADMAP.md step 8, decision 2):
  the direct layer, which mirrors the C API more or less one to one.
  A convenience layer in the language above it (decided 2026-10-13, later
  work, not built now) calls only the direct layer, never core
  (bindings.md section 1). Every consumer of a header is C: the CPython
  extension, the Node-API addon, the same addon compiled to wasm with
  emnapi, `ta-conform`.
- **The datamodel vtable is the only call out of core.**
- **Core is the only queue.** Mailboxes, the run queue and timers are
  core's (ADR 0004).
- **Lifetimes follow the loop** (ADR 0012): take, handle, ack, run.
  Everything the host hands out about a batch lives until
  `ta_host_effects_ack`.

## The contract, per call

### Phases

A call's answer depends on where the caller is in the loop:

| Phase | When | What may be called |
|---|---|---|
| **I**, idle | between runs, no batch open | everything |
| **D**, dispatch | between runs, a batch open: after a `ta_host_effects` that returned effects or an output call that handed out a view, until `ta_host_effects_ack` | everything but `ta_host_run` and snapshots; a take returns the open batch (none while it holds outputs alone) |
| **R**, run | inside `ta_host_run`, from a datamodel entry | the inputs on their in-run paths and the lookups; everything else refused (`host.h:151-170`) |
| **G**, guard | inside a datamodel entry core calls outside a run (a post, hibernate, wake, the telemetry takes, `release` in the ack, `ta_host_free`) | as R (`host.h:172-179`; ADR 0010 decision 3; `host_guard`, `src/host/host_internal.h:242-247`) |

R and G hold one rule: the guard sets the flag a run sets
(`host_internal.h:242-246`), so the checks below read "R/G".

### Host calls

"Ends" is when what the call produced stops being valid. "Pending" means
appended to the host's effects and handed out by the next take that opens
a batch. Test names are in `tinyactors-core/tests/unit/` unless a path is
given; `*.txt` vectors are in `tinyactors-test/suites/protocol/`; "needed"
marks a behaviour no test checks.

| Call (`host.h`) | When | Produces; owner; ends | Refused | Reentry | Test |
|---|---|---|---|---|---|
| `ta_host_create` (:105) | no host yet | the host, the caller's until `ta_host_free`; config read during the call | `TA_ERROR_INVALID` with the reply in `environment->error`, the caller's `ta_buffer`; `TA_ERROR_NO_MEMORY` (`host.c:454-461`) | none (no host) | `test_host_request.c` `test_create`, `test_config_effective`; `config.txt` |
| `ta_host_free` (:109) | I, D | releases the open batch and its outputs, every block (`host.c:506-512`) | R/G: does nothing, the host stays (`host.h`: not from a datamodel's code) | calls back from its own `destroy`/`release` refused (`closing`) | `test_host_guard.c` `test_free`; R/G: `test_host_inputs.c` `test_lookups_answer_and_free_does_nothing` |
| `ta_host_add_image` (:119) | I, D | a segment index; the host's copy of the bytes until the segment is freed | R/G `TA_ERROR_STATE` (`host.c:555`); `TA_ERROR_INVALID`; `TA_ERROR_NO_MEMORY`, nothing added | `chart_attach` runs in G (`host.c:588`) | `test_host.c` `test_image_copied` |
| `ta_host_chart` (:126) | all | a struct in the caller's memory | `TA_ERROR_INVALID`: no such name | a lookup: answers in R/G | `test_host.c` |
| `ta_host_segment_count` (:135) | all | a number | none | answers in R/G | `test_host.c` |
| `ta_host_segment_info_get` (:136) | all (not checked, `host.c:632-650`) | the caller's struct; `.image` borrowed while the segment is live | `TA_ERROR_INVALID` | answers in R/G, a lookup in `host.h`'s list | `test_host.c` `test_image_copied`; R/G: `test_host_inputs.c` `test_lookups_answer_and_free_does_nothing` |
| `ta_host_system` (:140) | all (not checked, `host.c:652-655`) | a `ta_system *`, borrowed until a call that can grow or free the segment | none | answers in R/G, a lookup in `host.h`'s list; a datamodel must not call `ta_system_*` on it (`datamodel.h:7-8`) | `test_host.c`; R: `test_host_inputs.c` `test_a_full_pool_is_no_memory` |
| `ta_host_boot` (:145) | I, D | the image's boot actors | R/G `TA_ERROR_STATE` (`host.c:2392`); `TA_ERROR_INVALID`: no such segment (`host.c:2394`); `TA_ERROR_NO_MEMORY` from the pool's growth: none spawned; a spawn failing after it (instance or name space, a datamodel's `create`): the earlier boot actors stay, and boot again spawns them again (`host.h`) | `create` in G (`host.c:2405`) | `test_host.c` `test_boot_manifest`: invalid segment, growth, named actors, entry actions after run; `test_boot_refused` (the growth refused); R/G: `test_host_inputs.c` `test_other_calls_are_refused` |
| `ta_host_actor_by_name` (:148) | all | a session id | 0: none | a lookup | `test_host_inputs.c` |
| `ta_host_spawn` (:183) | all | a session id; `spawn`'s views read during the call; its payload moves on success | `TA_ERROR_INVALID`: no such segment (`host.c:876`), or a name longer than `TA_SYSTEM_MAX_STRING` (`system.h`; nothing allocated, no growth); R/G: no growth, a full pool `TA_ERROR_NO_MEMORY` with nothing changed; a datamodel's `create` refusing: `TA_ERROR_NO_MEMORY`, nothing grows; inside free `TA_ERROR_STATE` (`host.c:874`) | `create` in G (`host.c:880`); in R the in-run path of `<invoke>`: at the back of the run queue, and an `<invoke>` whose child's `create` refuses fails with `error.execution` | `test_host_inputs.c` `test_spawn`; a datamodel's refusal: `test_host.c` `test_a_datamodel_refusal_grows_nothing`; a full pool in R: `test_host_inputs.c` `test_spawn_into_a_full_pool_is_no_memory`; the name's limit: `test_host_strings.c` `test_spawn_refuses_a_name_past_the_limit` |
| `ta_host_locate` (:189) | all | the caller's struct | `TA_ERROR_STALE` (`host.c:663-671`) | a lookup | `test_host.c` |
| `ta_host_send` (:191) | all | nothing kept of the caller's: views copied, payload moved on success | as spawn: a name, sendid, origin, origintype or raw longer than `TA_SYSTEM_MAX_STRING` is `TA_ERROR_INVALID` (a traceparent has no limit); the payload stays the caller's on refusal | in R the in-run path of `<send>`: to itself at the end of its macrostep, to another actor at the back of the queue (`host.h:156-161`); outside a run `release` in G for an `error.communication` (`host.c:900`) | `test_host_inputs.c` `test_send_to_self_waits_for_the_macrostep`, `test_send_and_send_many_to_another_actor`; the limit: `test_host_strings.c` `test_send_refuses_a_string_past_the_limit` |
| `ta_host_resolve` | all | the session a local address names, into the caller's `uint64_t` | `TA_ERROR_INVALID` (not an address, another system's), `TA_ERROR_STALE` (names nothing); answers in R/G, a lookup in `host.h`'s list | none | `test_remote.c` |
| `ta_host_send_many` (:195) | all | the accepted prefix's length | stops at the first failure (a text past the limit too); that event's payload stays the caller's | as send (`release` in G at `host.c:935`) | as send |
| `ta_host_terminate` (:196) | all | nothing | as spawn | exits at its next microstep boundary (`host.h:162-165`) | `test_host_inputs.c` `test_terminate_self_ends_after_the_microstep`, `test_terminate_another_actor` |
| `ta_host_resume` (:198) | I, D | nothing | R/G `TA_ERROR_STATE` (`host.c:958`); `TA_ERROR_INVALID`: no such segment (`host.c:960`) | `release` in G (`host.c:962`) | `test_host_inputs.c` `test_other_calls_are_refused` (R) |
| `ta_host_advance_time` (:214) | I, D | nothing | R/G: nothing done (`host.c:1671`) | none | `test_host_telemetry.c`; `test_host.c` |
| `ta_host_run` (:229) | I | a `ta_run` by value; its effects pending, then its notifications | D: `TA_RUN_FAULT`, detail `TA_ERROR_STATE`, nothing runs or is released; `used` 0, `next_deadline` `UINT64_MAX`; R/G: the same (`host.c:1828-1833`) | the datamodel vtable, in R | R: `test_host_inputs.c` `test_other_calls_are_refused`; D: `test_host_effects.c` `test_run_refuses_open_batch` |
| `ta_host_effects` (:288) | I, D | I: opens a batch of everything pending; D: the same pointer and count, never widened; an empty take opens no batch. Records, their strings, `WIRE` bytes and payloads are the host's until the ack | R/G: 0 (`host.c:1196-1199`) | none | `test_host_memory.c` `test_published_effects_stay_valid`, `test_pool_is_reclaimed_with_effects_pending`; `test_host_effects.c` `test_take_freezes_batch_across_growth` |
| `ta_host_effects_ack` (:289) | I, D | closes the batch: its effects, their payloads (`release` in G, `host.c:1044-1048`) and the batch's outputs end | R/G: nothing (`host.c:1275`) | `release` may run language code: a binding defers the last drop (bindings.md section 8) | `test_host_guard.c` `test_send_and_ack`; `test_host_memory.c` |
| `ta_host_effect_number`, `_text`, `_bytes` (:334-336) | D | a field of the open batch, valid until the ack; `TRACEPARENT`'s and `ORIGIN`'s text written on first ask into a slot per effect, valid until the ack (`effect_traceparent`, `effect_origin`, `host.c`) | an index past the batch, a field the kind does not set, or the wrong accessor: 0 or empty; not checked for R/G (`host.c:1218-1271`): publication closes before ack/free release callbacks, so they answer 0 or empty | none | `test_host_effects.c` `test_fields_match_the_struct` (number, text); `_bytes`: `test_host_request.c`; closed publication: `test_host_guard.c` `test_closed_batch_during_release`; two `TRACEPARENT`s read together: `test_host_effects.c` `test_traceparents_last_until_the_ack` |
| `ta_host_traces`, `ta_host_logs` (:348-349) | I, D | an OTLP request, a view of host memory until the ack (`outputs`, `host_output`); length 0: none staged | R/G and without an encoder `TA_ERROR_STATE` (`telemetry.c:289`, `:301`); `TA_ERROR_NO_MEMORY`: the spans stay staged | `to_wire` for traced values in G (`telemetry.c:90`) | `test_host_telemetry.c` `test_drain_and_take`, `test_take_refused`; `test_host_guard.c` `test_traces`; the view after a later call: `test_host_guard.c` `test_outputs_last_until_the_ack` (metrics, through the same output path) |
| `ta_host_migrate` (:587) | I, D | a migration id; `MIGRATED`, `MIGRATION_DONE` pending in later runs | R/G `TA_ERROR_STATE` (`host.c:1403`); `TA_ERROR_NO_MEMORY` | none | `test_host.c` `test_versions_and_migration` |
| `ta_host_migration_pending` (:589) | all (not checked) | the caller's array: the sessions a running migration waits for, in no order; a finished migration's list is readable until the next run, which comes after the ack | none | answers in R/G, a lookup in `host.h`'s list | `test_host.c` `test_migration_wake_and_timeout` |
| `ta_host_sweep` (:595) | I, D | sessions in the caller's array | R/G: 0 (`host.c:1962`) | none | `test_host.c`, `test_host_memory.c` |
| `ta_host_hibernate` (:600) | I, D | the blob, a view of host memory until the ack | R/G `TA_ERROR_STATE` (`host.c:2014`); `TA_ERROR_NO_MEMORY`: the actor stays | `serialize`, `destroy` in G (`host.c:2022`) | `test_host.c` `test_hibernate_and_wake`; `test_host_guard.c` `test_hibernate`; blob after a later call: through the same output path as `test_outputs_last_until_the_ack` |
| `ta_host_wake` (:601) | I, D | the actor back, and back on the retry list of a migration that waits for it; the blob read during the call | R/G `TA_ERROR_STATE` (`host.c:2029`); `TA_ERROR_NO_MEMORY` (a migration's list could not grow, or the segment); `TA_ERROR_INVALID` for a malformed blob: the actor stays hibernated | `deserialize` in G (`host.c:2035`) | `test_host.c` `test_hibernate_and_wake` |
| `ta_host_snapshot` (:642) | I with nothing pending | into the caller's `ta_buffer` | effects published or pending (`host.c:2470`), or a migration active: `TA_ERROR_STATE`; a batch opened only by an output view too; R/G too (`host.c:2469`); `TA_ERROR_NO_MEMORY`: `length` is what it needs | none | `test_host.c` `test_snapshot_restore` |
| `ta_host_snapshot_segments` (:646) | no host | hashes in the caller's array | 0: not a snapshot | none | `test_host.c` |
| `ta_host_restore` (:700) | no host | a new host; images copied as by `add_image`; its first `ta_host_run` (after the datamodels' state is restored) announces each external invocation that was running again, before any actor runs: a `TA_EFFECT_INVOKE` with `TA_EFFECT_FLAG_RESTORED`, its token, type, src, invoke id and params | `TA_ERROR_INVALID`, `NO_MEMORY` | `chart_attach` in G (`host.c:2669`) | `test_host.c` `test_snapshot_restore`, `test_host_effects.c` `test_restore_announces_external_invocations`, `test_restore_after_images_freed` |
| `ta_host_snapshot_pack` (:671) | I with nothing pending | into the caller's `ta_buffer` | as `ta_host_snapshot` (it calls it, `host.c:2764`) | none | `test_host.c` `test_snapshot_pack` |
| `ta_host_snapshot_unpack`, `_image` (:684-686) | no host | views into the pack | `TA_ERROR_INVALID`; empty | none | `test_host.c` `test_snapshot_pack` |
| `ta_host_post` (:711) | I, D | applies the method; its effects, then its `REPLY` (with an id), pending: in D they come after the ack | R/G `TA_ERROR_STATE` (`request.c:996`); `TA_ERROR_NO_MEMORY`: nothing applied, no `REPLY` | `actor.context`: `variable`, `to_wire`, `release` in G (`request.c:701`, `:738`, `:747`) | `test_host_request.c` `test_post`; `test_host_guard.c` `test_request`; `requests.txt` |
| `ta_host_metrics` (:718) | I, D | an OTLP metrics request, a view until the ack; with no batch open it opens one of its own | R/G and without an encoder `TA_ERROR_STATE` (`telemetry.c:295`); `TA_ERROR_NO_MEMORY` | none | `test_host_request.c` `test_spans_and_metrics`; `test_host_guard.c` `test_outputs_last_until_the_ack` |

### The vtable, as core calls it

Every entry runs in R or G, so a datamodel's code that calls back into the
host gets the R/G column above. Its outputs are views it owns until the
next call on the same datamodel (`datamodel.h:9-13`); core copies what it
keeps first (ADR 0010 decision 1).

| Entries (`datamodel.h`) | Called in | From | Test |
|---|---|---|---|
| `init_data`, `bind_event`, `detach_event`, `eval_condition`, `eval_string`, `assign`, `assign_string`, `script`, `log`, `foreach_*`, `capture`, `custom` | R | the system's run | the W3C suite through `ta-conform` |
| `create` | R; G | `<invoke>` in a run; `ta_host_spawn` (`host.c:880`), `ta_host_boot` (`:2405`) | `test_host_inputs.c` |
| `chart_attach` | G | `ta_host_add_image` (`host.c:588`), `ta_host_restore` (`:2669`) | `test_host.c` `test_image_copied` |
| `destroy`, `chart_detach` | R; G | an actor exits in a run; retirement (`host.c:1807`), hibernate, migration (`host.c:1508`), `ta_host_free` | `test_host_guard.c` `test_free`, `test_outputs_grow_between_runs` |
| `retain`, `release` | R; G | effect copies (`host.c:1190`), the ack (`:1047`), the drain (`:1359`), resume (`:962`), sends that fail outside a run (`:900`, `:935`), sends between segments in a run's drain (`local_send`, `host.c`), post (`request.c:747`), free | `test_host_guard.c` `test_send_and_ack` |
| `serialize`, `deserialize` | G | hibernate (`host.c:2022`), wake (`:2035`), migration (`:1470`, `:1508`, `:1529`) | `test_host_guard.c` `test_hibernate`, `test_each_output_once` |
| `payload_serialize`, `payload_deserialize`, `migrate` | G | migration (as above) | `test_durable.c`; `test_migrate.c` |
| `from_wire` | G | `actor.send` at the target (`request_run_actor_send`, `request.c`) | `test_host_request.c` `test_actor_send` |
| `to_wire` | G | `actor.context` (`request.c:701`), traced values when records are read (`telemetry.c:90`, at a run's start and at a take) | `test_host_guard.c` `test_each_output_once`, `test_traces` |
| `variable` | G | `actor.context` (`request.c:738`) | `test_host_request.c` `test_actor_context` |

### Which distinctions callers need

| Distinction | Callers need it, or bookkeeping | After ADR 0012 |
|---|---|---|
| run (R) or between runs | callers: it decides what a datamodel's code may call | kept |
| guard (G) or run (R) | bookkeeping: one rule since ADR 0010 decision 3 | kept as one rule, "R/G" |
| idle (I) or dispatch (D) | callers: a run and a snapshot wait for the ack | made explicit: the run refuses in D |
| effect appended, published, consumed, published again by a widened take | bookkeeping: the drain re-read and the facade's marks | **removed**: pending or in the open batch, nothing else |
| effects end at the ack, the next run or `ta_host_free` | bookkeeping: the run's release lost effects | **removed**: the ack (or `ta_host_free`) |
| `TRACEPARENT`'s text until the next `ta_host_effect_text` | bookkeeping: one buffer | **removed**: until the ack |
| a host output until the next call into the host | bookkeeping: one block reused | **removed**: until the ack |
| a datamodel's output until its next call | callers (datamodel authors): core consumes it inside its own call | kept |
| the caller's `ta_buffer` (snapshots, `environment->error`) | callers: the size is known up front, or no host exists | kept |
| structural borrows (a segment's image, `ta_host_system`, a migration's pending list) | bookkeeping a binding rarely reads | kept, as `host.h` states each |
| `REPLY` or `NOTIFICATION` | callers: an answer or news | kept |
| `actor.changed` with `value`, `null`, or no `value` | the third was a limit's leftover | **removed** with `actor.subscribe` (ADR 0012, decision 4); notifications stay possible |

### What no test checks yet

From the table: a boot that fails after some boot actors spawned (a
spawn refused past the pool's growth), documented in `host.h` but not
tested.

The findings of step 8.2, each settled by a decision and a test:

1. **`ta_host_free` from a datamodel's code** — settled: it does nothing
   in R/G (`test_host_inputs.c` `test_lookups_answer_and_free_does_nothing`).
2. **Lookups during a run beyond `host.h`'s list** — settled: `host.h`
   lists `ta_host_segment_info_get`, `ta_host_system`, `ta_host_resolve`,
   `ta_host_migration_pending` and the effect accessors as lookups (same
   test).
3. **A test calls `ta_system_*` from datamodel code** — settled: the
   rule stands (`datamodel.h:7-8`); `test_host_inputs.c`'s hooks show that
   nothing grew by counting the host's allocator, not by reading
   `ta_system_stats`.
4. **The width of ids in `types.h`** — settled: `types.h` names three
   kinds: indices (segment, chart, I/O processor, datamodel) are
   `uint32_t` from 0 with `TA_NIL` for none; handles (actor, token,
   migration, watch) are `uint32_t`, never 0, with 0 for none (an actor's and a
   token's carry a generation, `ta_pool_handle`, `src/base/pool.h:70`);
   sessions, payloads and `ta_table` handles are `uint64_t` with 0 for
   none.
5. **`detach_event`** — settled: the datamodel keeps `_event` after it,
   until the next `bind_event` or `destroy`; core frees only its own copy.
   SCXML keeps `_event` bound to the last event until another is
   processed (`docs/SCXML.md`, 5.10), and a cancel runs `exitInterpreter`
   without rebinding it (Appendix D): the `<onexit>` handlers of an actor
   terminated at rest (`PHASE_STABLE` with `INSTANCE_FLAG_CANCEL`,
   `core.c:1876-1878`) read it, and `serialize` writes it
   (`datamodel.h`).

## 1. Inputs

**Design.** `ta_host_send`, `ta_host_send_many`, `ta_host_spawn`,
`ta_host_terminate`, `ta_host_resume`. Each takes a C
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

**Design.** The loop of embedding.md: drain (take, handle, ack until a
take is empty), `ta_host_advance_time`, `ta_host_run(budget)`, drain, one
timer at `next_deadline`. A run refuses while a batch is open: fault with
detail `TA_ERROR_STATE`, `used` 0 and `next_deadline` `UINT64_MAX`, without
work or release. The tick rate comes from the effective config
(`host.config`). Each binding offers `SimulatedClock.increment(ms)`.
Node's timer is the global `setTimeout`.

**Rejected.** A default tick rate in a binding (a default of its own; the
system's 1e9 is stated in host-config.md). libuv timers (Bun panics on
`uv_timer_init`, oven-sh/bun#18546). A fatal status for signals (a signal
is an event on the loop and a run is bounded: shutdown is between turns).
A run that releases an unacknowledged batch (effects lost in silence;
ADR 0012).

## 3. Effects

**Design.** Host-owned records (`ta_host_effect`), read through accessors
by field id (`ta_host_effect_number`, `_text`, `_bytes`). A take
(`ta_host_effects`) with no batch open opens one with everything pending;
while it is open, a take returns the same batch and effects appended
meanwhile wait for the next take after the ack. An empty take opens no
batch. A private pointer fixes the published array across growth; replaced
arrays and pools stay held until the ack. Publication closes before ack/free
release callbacks, so accessors cannot read records being freed.
Records, strings, wire bytes and payloads live until `ta_host_effects_ack`
or `ta_host_free`, `TRACEPARENT`'s text included.

| Object | Owner | Created | Read | Ends |
|---|---|---|---|---|
| an effect record and its strings | the host's effect array and pool | appended by a run or a call | through the accessors, in D | the ack |
| `TRACEPARENT`'s text | the batch's memory | first ask for that effect | in D | the ack |
| an effect's payload | the datamodel's row, retained by core (`host.c:1187-1191`) | the effect's copy | `ta_host_effect_number(PAYLOAD)` | released by core at the ack (`host.c:1044-1048`) |
| a replaced effect array or pool block | the host (held) | growth while a batch is open (`host.c:972-981`) | through the open batch's pointers | the ack |

**Rejected.** Struct layouts read from Python or JavaScript (`memoryview`,
typed arrays): "Only C touches host structs" (ADR 0009), the ABI coupling
it was written against. A take that widens the open batch, with a drain
that re-reads until the count is stable and acks at its outermost level
(ADR 0011 decision 6): it existed for the synchronous request, and a
facade that acked inside a handler lost effects (ADR 0012).

## 4. The datamodel

**Design.** A native datamodel keeps handle tables (sessions, charts,
payloads, loops), since language objects cannot live in memory moved by
`memcpy` and the collector must see them; `ta_table` (base,
`tinyactors/table.h`, ADR 0012 decision 5) is that table. Its outputs
(`to_wire`, `serialize`, `payload_serialize`, `eval_string`, `log`) are
views it owns until its next call; core asks for each value once
(ADR 0010, decision 1). While core is inside a datamodel outside a run,
calls back into the host follow the run's rules (decision 3).
At `detach_event` core frees its own copy of the event; the datamodel
keeps its `_event` until the next `bind_event` or `destroy` (finding 6
above).

**Rejected.** Caller-sized outputs with retries: core ran datamodel code
two or three times per output (`hibernate.c`, and the envelopes of the
router since removed), and a 1 KB value was walked six times. Strings handed back into a buffer
core provides: user expressions cannot run twice. No guard: a `toJSON`
that terminated its actor left core calling a destroyed instance (an ASan
crash, `tests/unit/test_host_guard.c`). A handle table in each binding
(the same code twice; ADR 0012).

## 5. Values and CBOR

**Design.** `ta_cbor_writer` writes items into a room that grows through
an allocator; `ta_cbor_reader` is a cursor (ADR 0010, decision 4). Token
lists remain only as the random-access index requests are parsed with.
Inside the process, values cross by copy through the value table between
the API and the binding's own datamodel; CBOR only toward quickjs.

**Rejected.** Token lists in bindings (caller-sized scratch; streaming is
faster: 2.5 vs 3.7 µs encode, 6.7 vs 8.5 µs decode for 1 KB, no 294 KB of
tokens for 64 KB). `ta_cbor_encode` (deleted). A codec in the language.

## 6. The control plane

**Design (ADR 0011, decisions 1, 2 and 5; ADR 0012, decisions 2 to 4).**
`ta_host_post(h, message)`: a JSON-RPC 2.0 message without `"jsonrpc"`,
in CBOR, read during the call and answered at it (`src/host/request.c`).
Its reply is the last effect the call appends, `TA_HOST_EFFECT_REPLY`
(WIRE `{"id", "result"}` or `{"id", "error"}`), after the effects the
method appends itself (`trace.end`'s TRACES and LOGS); all of them are
pending, so a post made while a batch is open is answered in the next
take. The reply is written into the host's output, then copied once into
the effect pool (its last string slot, WIRE). A method that changes the host reserves room by capacity for the
reply and three records before it applies (`request_fits`), so
`TA_ERROR_NO_MEMORY` means nothing was applied and no REPLY appended; a
method that only reads, and a refusal after a method tried, reserve after
writing. A message without an id is applied with no REPLY; one that is
not a request is answered with `"id": null`; batches are refused
(-32600). `TA_ERROR_STATE` in R/G.

Every byte is CBOR; JSON-RPC names only the shape of the maps, so no
shape is invented. Core never reads or writes JSON: a binding deals with
the effect records and CBOR, and a transport that speaks JSON converts at
its own edge.

Core's notifications are `TA_HOST_EFFECT_NOTIFICATION` effects (WIRE
`{"method", "params"}`, EVENT the method, SESSION the session). None is
defined (ADR 0012, decision 4 removed `actor.subscribe`,
`actor.unsubscribe` and `actor.changed`, the host's subscription table
and the config key `maxSubscriptions`). `actor.state` answers a
session's state, refusing with `limit` past 1024 active states. The
system's CHANGED and `ta_actor_watch` stay below the host, as
`ta_system`'s API; the host sets no watch. A binding drops a notification whose method it does not
know.

**A language API has no synchronous request and no raw post** (ADR 0012,
decision 3): typed methods over post, each taking a response handler that
the drain handling its REPLY calls (bindings.md section 10). A JSON-RPC transport, when one exists, is a
pipe: frames to `ta_host_post`, REPLY and NOTIFICATION bytes to the
socket, in the order the takes give them.

| Object | Owner | Created | Read | Ends |
|---|---|---|---|---|
| the posted message | caller | before the call | parsed into tokens on the C stack during `ta_host_post` | the call returns |
| the reply's first writing | the host's output arena (scratch, never handed out) | during the call | copied into the effect pool at the end of the call | reused by the next output while nothing is lent; else the ack |
| a REPLY or NOTIFICATION's WIRE | the effect pool | the post (REPLY), the end of a run (NOTIFICATION) | `ta_host_effect_bytes` in D | `ta_host_effects_ack` or `ta_host_free` |

**Rejected.** Keeping `ta_host_request` beside `ta_host_post` (two ways;
a view that ends at the next call). A queue of posted messages (answering
at the call orders the reply as well). Requests on an input queue (the
deferred read of section 1). Binary records for the control plane (the
catalogue grows; JSON-RPC faces outward). "Valid until released" for
replies as a separate rule (with replies as effects, the ack is that
release). The REPLY before the method's own effects (the reservation is
by capacity before applying; the reply is written last). Context in
notifications (user code, unbounded size). Batches in core. A
synchronous `request` facade (it needs a widened batch and runs inside
dispatch; ADR 0012). Subscriptions (`actor.subscribe`, `actor.changed`):
a stress test of the protocol, removed by ADR 0012 (decision 4),
since an actor's output reaches the application through an I/O processor
and its state through `actor.state`; the shapes weighed before that
(ADR 0012, "History") follow. The whole state value in `actor.changed`: past the stack's room for 1024 states it
needed new state-value machinery, a walk in place or scratch from the
host's allocator, both dropped by the maintainer; and,
before it, a truncated value with `"limit": true`, `null` past the limit,
or no `value` (three shapes for one reader). For the end of a session:
a list of every state as `exited` and a `null` member (a value, where the notification
carries none).

## 7. Host outputs

**Design.** `ta_host_traces`, `ta_host_logs`,
`ta_host_metrics`, `ta_host_hibernate`: views of
host memory valid until `ta_host_effects_ack` (ADR 0012, decision 1). An
output taken with no batch open opens one. The host keeps a batch's
outputs in memory it frees or reuses at the ack, and growing it never
moves an output already handed out. `ta_host_snapshot`,
`ta_host_snapshot_pack` and `environment->error` keep `ta_buffer` (size
known up front; no host yet). A hibernation is written in one
pass through the host's allocator, between runs only
(tinyactors-core/AGENTS.md, "Region memory is fixed at ta_system_init").

| Object | Owner | Created | Read | Ends |
|---|---|---|---|---|
| an output view | the batch's memory (the host's allocator) | the output call, in I or D | by the caller, across any later calls | the ack |

**Rejected.** A view until the next call into the host (ADR 0010
decision 2): a handler that posts, sends or takes a second output
ends the view it is reading, and both bindings copied every output
before any call (ADR 0012).

**Decided (2026-10-14): one copy, in the binding.** An output reaches
the application as memory the language owns: the binding copies the
host's view once (`napi_create_buffer_copy`, `PyBytes_FromStringAndSize`)
before the ack, and the application gets a `Buffer` or `bytes` it may keep,
with no further copy. It works on every runtime, Electron included, and
core adds nothing for it. Not a view scoped to a handler: proposed and
refuted (2026-10-14, by spikes in CPython 3.13, Node 22
and 26, and Bun 1.4):

- **Node and Bun:** an external `ArrayBuffer` must stay valid until its
  finalizer runs (Node-API docs, `napi_create_external_arraybuffer`), and
  detaching after the handler does not end every reader:
  `ArrayBuffer.prototype.transfer()` in the handler moves the bytes to a
  buffer the binding cannot detach, which then reads memory the host
  reused after the ack; Bun's `fs.promises.writeFile` reads the old
  pointer after the detach; a `socket.write` or `http` write queued
  under backpressure, or before a connection, loses its bytes and the
  connection stalls. Electron refuses external buffers
  (`napi_no_external_buffers_allowed`), so a copy is needed there anyway.
- **Python:** a buffer exporter counts its exports reliably, but a
  `memoryview` cannot be pointed elsewhere, so a view held past the
  handler reads reused memory after the ack, or the ack must wait for it.
  Waiting stops the host (`ta_host_run` refuses while a batch is open),
  and ordinary code holds views: a handler that raised (its traceback),
  a view in a reference cycle, an asyncio transport under backpressure
  (`_SelectorSocketTransport.write` keeps `memoryview(data)[n:]`).
- This repeats, in the other direction, the reason a caller's image
  bytes are not held (napi `38fc05c9`).

Not the host handing a block's ownership to the binding, freed when the
language's object is collected: a new core call, host memory outside the
loop's lifetimes and the allocator called from finalizers, to save one
copy of a few kilobytes (unmeasured; to be measured only if large
hibernation blobs make the copy matter).

**A batch of outputs alone.** An output taken with no effects published
opens a batch (`outputs_lent`, `src/host/host_internal.h`): `ta_host_run`
and snapshots refuse, and a take returns 0 with pending effects waiting,
until the ack (ADR 0012, decision 2: a take never widens the open batch).
A caller that pulls metrics before a drain acks first, or the drain
waits. Each output is a block of its own in an arena the ack empties,
kept for reuse; internal scratch (a post's reply before it reaches the
pool) also comes from it and is reused at once while nothing is lent.

## 8. Images

**Design (ADR 0011, decision 7).** `ta_host_add_image(h, ta_bytes image,
&segment)`: the host reads the bytes during the call only, copies them
into one block from its allocator, `[ta_image | bytes]`, 8-aligned (a
block of its own, since `host->segments` grows by copying), opens them
there before any datamodel code runs, and frees the block after the
segment's `ta_system_deinit`: when it retires (which clears the segment's
image), with `ta_host_free`, and for every segment a failed restore left.
`ta_host_snapshot_input.images` are `ta_bytes`, copied the same way.
`ta_host_segment_info.image` is the host's copy, NULL once freed.
`TA_HOST_EFFECT_SEGMENT_FREED` stays as a retirement signal. The
ECMAScript datamodel's `chart_by_hash` compares the hash stored at attach,
among attached charts, and never reads an idle chart's image, which the
host may have freed. `host.stats`' `regionBytes` counts regions, not image
copies. The binding keeps nothing.

| Object | Owner | Created | Read | Ends |
|---|---|---|---|---|
| the caller's image bytes | caller | before the call | copied during `ta_host_add_image` / `ta_host_restore` | the call returns |
| the image block | the host (environment's allocator) | `ta_host_add_image`, `ta_host_restore` | the segment's system, the datamodels' charts, requests, definitions, snapshot packs | after the segment's `ta_system_deinit` |
| `ta_host_segment_info.image` | the host (borrowed) | `ta_host_segment_info_get` | while the segment lives | the segment is freed (then NULL) |
| a datamodel's idle chart | the datamodel | `chart_attach` | its stored hash only | eviction, or adoption by an image with that hash |

**Rejected.** Holding a caller's language buffer (transfer, resize or a
shared buffer made core read freed memory: napi `38fc05c9`).
`ta_compile_image` returning the `ta_image` the result holds (kept the
lifetime the binding's). A borrow mode (zero-copy embedders use
`ta_system`, whose image stays borrowed).

## 9. A binding's memory

**Design.** Four things, and the allocator is called only by them and by
the host's create, free and allocator (each binding's lint rule):

- **Its handle tables** (`ta_table`): charts, sessions, payloads, loops,
  and the pending typed methods; a handle is a row's, with a generation,
  so a stale one is refused.
- **One `ta_growable_arena` per host**, made with the host and
  freed with it. Each API call takes a mark at its entry and releases to
  it at its exit, on every path. It holds what the call converts: the
  config's and a request's CBOR (a `ta_cbor_writer` growing through the
  arena's `ta_allocator`), text the language gives no view of, and input
  structs (`ta_event_in` arrays for `send_many`). A call made during
  another (language code a conversion runs) allocates above the outer
  call's memory and releases to its own mark; nothing moves. Released
  chunks are kept, so after warm-up no call reaches the allocator.
- **A datamodel's last output**, overwritten by its next call.
- **Views**, both ways: what the language holds goes to core as a view
  for the call (a `str`'s UTF-8); what core hands out is exposed to the
  language as a **thin view**, an object that holds where the value is
  (the host, the batch, the effect's index) and converts a field when it
  is read, not before. A thin view is valid while its batch is open: the
  binding counts its acks (core has no batch generation, and needs none:
  only the binding calls `ta_host_effects_ack`), a view records the count
  when it is made, and a read with another count is refused. What must
  outlive the ack is materialized at a point the binding names
  (bindings.md section 6 has the table of each object).

Nothing it reads from the host needs copying to survive its own calls
while it handles a batch (ADR 0012).

| Object | Owner | Created | Read | Ends |
|---|---|---|---|---|
| the host's arena | the binding (chunks from the host's allocator) | with the host | by every call | `ta_growable_arena_free`, with the host |
| a call's allocations | the arena, above the call's mark | during the call | during the call, and by core during the core calls it makes | the release to the call's mark, at its exit |
| a thin view of an effect | the language's object, holding the host, the binding's ack count and an index | when the binding hands the effect to a handler | by the handler, field by field | usable until the ack; a read after it is refused |
| a materialized record | the language's object | before the ack, where a value must outlive it | any time | the language's collector |

**Rejected.** Paired `malloc`/`free` at every call site (Node had 104,
with allocations per call and per evaluation). One arena for the host's
lifetime with no marks (grows without bound as actors come and go; the
arena here is released to a mark at every call's exit, so it holds the
largest single call's need, not the sum of all). Scratch shared between
calls without marks (not reentrant: a nested request corrupted the outer
one, napi `8d600f4c`; with marks a nested call allocates above the outer
one and gives back only its own). One writer per call on the C stack,
grown through the allocator and freed at the call's end (a call with
several conversions pays an allocator pair for each, every call; ADR
0012, rejected alternatives). Converting every field of an effect into the
language when it is dispatched (a handler pays for fields it never
reads). A batch generation in core (only the binding acks, so it can
count). Rooms with length hints and retries (workarounds for
caller-sized outputs, gone with ADR 0010). Cross-thread free and
collector-switch hazards (one owning thread).

## 10. What reaches the application

**Design (ADR 0012, decision 6).** Output: `<send>` to
an I/O processor the application configured (`TA_EFFECT_SEND`), whose
handler may answer it. State: `actor.state`, a typed method. Inspection:
typed methods over post with response handlers. `<log>`: a log handler
configured per host, no global configuration. `DONE` and `ERROR`: the
binding's own lifecycle (release its rows), not callbacks. Other systems:
the host's transport, a handler of the application's.

## 11. Base APIs for embedders and bindings

Base supplies three utilities for embedders and bindings. They are
freestanding, have no dependency on a language's API, and allocate only
through the caller's `ta_allocator`:

| API | What | Lifetimes |
|---|---|---|
| `ta_table` (`table.h`) | rows of a caller's size, named by 64-bit handles `generation << 32 \| (slot + 1)`, 31 bits of generation (odd while live); grows by doubling | a handle until `ta_table_remove` (stale for good after); a row pointer until the next `ta_table_add`; the rows until `ta_table_free`; `ta_table_snapshot` writes the handle namespace into the caller's `ta_buffer`, `ta_table_restore` puts it back into an empty table (the handles live again, the rows zeroed) |
| `ta_growable_arena` (`arena.h`) | linked chunks, a request larger than the chunk size in a chunk of its own, marks, a release that keeps its chunks, a `ta_allocator` view. Both compiler arenas use it | an allocation until a release to a mark taken before it (it never moves); chunks until `ta_growable_arena_free`; the `ta_allocator` view while the arena lives |
| `ta_status_name` (`types.h`) | `"TA_ERROR_STATE"` for `TA_ERROR_STATE`; NULL for a status `types.h` does not define | static text |

The internal `ta_arena` (`src/base/arena.h`: a caller's region carved by
offset, with a measure mode, for `ta_system_layout` and `ta_system_init`)
keeps its name and its job; it never allocates.

Bindings use these shared utilities for tables, status names and scratch
storage. Arena release retains all chunks for reuse; only free returns
them to the allocator.
The compiler uses one `ta_growable_arena` for its temporary objects and
another for diagnostic strings kept by the result. Its helpers zero
objects and latch allocation refusal outside the shared arena.

## 12. Compiling, and the resolver

A binding compiles with `ta_compile_charts`, reads the image
with `ta_compile_image` and the diagnostics with
`ta_compile_diagnostics`, and frees the result with `ta_compile_free`; the
host copies the image's bytes (section 8). A static `src` (`<script>`,
`<data>`, `<invoke>`) is resolved during the call through
`ta_compile_options.resolve`. The callback takes the resolver context,
a request and a result:

```c
typedef struct ta_resolve_request { ta_string_view base; ta_string_view source; ta_growable_arena *scratch; } ta_resolve_request;
typedef struct ta_resolve_error { uint32_t line, column; ta_string_view message; } ta_resolve_error;
typedef struct ta_resolve_result { ta_source source; ta_resolve_error error; } ta_resolve_result;
typedef ta_status (*ta_resolve_function)(void *context, const ta_resolve_request *request, ta_resolve_result *out);
```

The compiler zeroes the result before each callback. On `TA_OK`,
`out->source` supplies the text and an optional canonical name. On a
refusal, `out->source.name` identifies the diagnostic file when supplied;
otherwise the file is the requested `src`. `out->error` supplies the line,
column and message. Unspecified positions are 0, and an absent message
produces `cannot resolve src`. Diagnostic messages are limited to 511
bytes. Names and messages need not be NUL-terminated.

`scratch` is the compiler's per-compilation `ta_growable_arena`, freed
before either compile entry point returns. A resolver may allocate names,
text and messages there; it must not free the arena or release storage
from earlier callbacks. Resolver-owned views must also remain valid until
the compile call returns. The compiler copies queued text into its
temporary storage, expressions into the image, and diagnostic names and
messages into the result's arena. All access occurs on the compiling thread.

| Object | Owner | Created | Read | Ends |
|---|---|---|---|---|
| the scratch arena | the compiler (chunks from `ta_compile_options.alloc`) | at the start of the compile call | by the resolver (writes) and the compiler | when the compile call returns |
| a resolved name, text or error message | the scratch, or the resolver's own memory | in the resolver's call | by the compiler, which copies what it keeps | must remain valid until the compile call returns |
| a diagnostic (`file`, `message`) | the compile result's arena | when the compiler reports it | by the caller | `ta_compile_free` |
| the image (`ta_compile_image`) | the compile result | at the compilation's end | by the caller; `ta_host_add_image` copies it | `ta_compile_free` |

**The resolver lives only at compile time.** A static `<invoke src>` is fetched through it
while compiling, and the invoked chart is compiled into the same image
(`scxml.c:893-896`, `resolve_chart` at `:855-870`). A host has no
resolver.

**`TA_EFFECT_REQUEST` stays (2026-10-13; ADR 0012,
decision 8).** It is how a chart fetches at run time: an `<invoke srcexpr>`
(or a `src` the compiler did not match, one with a `typeexpr`,
`scxml.c:895`) whose chart is not in the image. Core normalizes the
name itself (`file://`, `file:`, `./`, an inline document) and looks it
up in the requesting segment's image (`route.c:439-489`); it gives the
REQUEST, the name in `AUXILIARY`, only when that lookup fails
(`:491-497`). The fetching is built out later. Until then a binding
answers every REQUEST with `ta_host_resume` and `TA_ERROR_INVALID`, and
the parent gets `error.execution` (`route.c:544-550`): a resume names
only a chart of the requesting segment's image (`:544-553`), and core
has already looked there. What the build-out needs from core is in
`ROADMAP.md`, "Decided later". A REQUEST is not the control plane:
`ta_host_post`, `REPLY` and `NOTIFICATION` (section 6;
ADR 0011) are a different mechanism.

**Rejected.** A fourth parameter for the error (the rule of three
parameters; root `AGENTS.md`). A resolver keeping every text alive until
the compilation ends by itself (`ta_runtime_files` owns its texts,
`runtime.h:32-37`): the compiler already has an arena with exactly that
lifetime. The compile call's resolver kept on the host to answer a
REQUEST with a text (an earlier design, ADR 0012, "History"): a resume
names only a chart of the segment's image (`route.c:544-553`), so the
text had nowhere to go. How run-time fetching is answered (a runtime
resolver configured on the host, among others) is decided later.

## Changing this page

A change to the boundary is an ADR first, then this page, then the
guide, then the code; the bindings migrate in one message per change,
after the maintainer has read the ADR (`boundary-design`, step 7).
