# Migration: what goes, what core gains, which ADRs change

**Status:** Sketch (2026-10-01), part of [the ring protocol proposal](../README.md)

This is not a plan. It lists what adopting the proposal would change, so
the cost can be weighed before anyone decides. AGENTS.md's rules apply
throughout:

- the libraries are private, so APIs change in place and every caller
  moves in the same change;
- code that nothing uses any more is deleted in the same change.

Line counts are from `wc -l` on 2026-10-01.

## `tinyactors-host` is deleted

The package goes: `include/tinyactors_host/*.h`, `src/*.c` (8,137
lines), `tests/` (5,360 lines) and its ADRs, which stay as history. Each
module either has nothing left to do, or its work moves somewhere named
below.

### Deleted: core or the protocol does it

| Module | Lines (src) | What did it, what does it now |
|---|---|---|
| `scheduler.h`, `scheduler.c` | 1,465 | Mailboxes, runnable actors, turn budgets, dead letters. Replaced by core's pool and its run queue ([envelope.md](envelope.md)), run to completion ([macrostep.md](macrostep.md)) and `ta_run`'s budget. |
| `router.h`, `router.c` | 232 | `ta_external_event_router` for mail between sessions. Mail between local sessions is an envelope in core. Anything else is a [`c2h.send`](../protocol/generated/MESSAGES.md#c2hsend). |
| `effects.h`, `effects.c` | 98 | The per-turn effect queue that kept effects until JavaScript could run. Replaced by c2h records executable on receipt, including before the macrostep end. |
| `adapter_io.h`, `adapter_io.c` | 260 | The I/O processor adapter that copied sends into effects. Replaced by `c2h.send`. |
| `adapter_invoker.h`, `adapter_invoker.c` | 210 | The invoker adapter. Replaced by [`c2h.invoke`](../protocol/generated/MESSAGES.md#c2hinvoke), [`c2h.invoke_cancel`](../protocol/generated/MESSAGES.md#c2hinvoke_cancel), [`c2h.invoke_forward`](../protocol/generated/MESSAGES.md#c2hinvoke_forward) and [`h2c.invoke_report`](../protocol/generated/MESSAGES.md#h2cinvoke_report). |
| `adapter_log.h`, `adapter_log.c` | 34 | The buffered log sink. Replaced by [`c2h.log`](../protocol/generated/MESSAGES.md#c2hlog). |
| `recorder.h`, `recorder.c` | 509 | Collecting the events recorded families took, once per turn. Replaced by [`c2h.input`](../protocol/generated/MESSAGES.md#c2hinput) and the `recorded` bit ([recording-and-replay.md](recording-and-replay.md)). |
| `replay.h`, `replay.c` | 247 | `tah_replay_event`, `tah_replay_events_done`. Replaced by `h2c.spawn` (replaying), `h2c.event` and `h2c.publish`. Core's replay semantics (ADR 0089, 0094) stay. |
| `loads.h`, `loads.c` | 529 | Load records, waiting mailboxes, wakeups. Replaced by waiting lists in the pool and four records ([unloading.md](unloading.md)). |
| `offloads.h`, `offloads.c` | 99 | Images of offloaded actors, kept per address. The host keeps the `blob` from [`c2h.unloaded`](../protocol/generated/MESSAGES.md#c2hunloaded). |
| `directory.h`, `directory.c` | 118 | The in-memory directory with unloaded entries. Core keeps the resident ones. Unloaded addresses are in the host's index. |
| `events.h`, `events.c` | 30 | The cache of interned event names (napi ADR 0004). Run-time names are bytes now ([names.md](names.md)). |
| `hooks.h`, `hooks.c` | 57 | Post-call hooks that told the plumbing about new and removed actors. There is no plumbing left to tell. [`c2h.spawned`](../protocol/generated/MESSAGES.md#c2hspawned) and [`c2h.destroyed`](../protocol/generated/MESSAGES.md#c2hdestroyed) carry the same facts. |
| `params.h`, `params.c` | 22 | Kernel parameters. Removed or replaced ([BEHAVIORS.md](../BEHAVIORS.md#removed-parameters)). |
| `system.h`, `system.c` | 687 | The native system record, the pump driver, entering and leaving core. What is left is napi's: one `ta_system`, its rings, and the pump. |
| `version.c` | 6 | |
| **Total** | **about 4,600** | |

### Moved: still needed, but not in a kernel

| Module | Lines (src) | Goes to |
|---|---|---|
| `otlp.h`, `otlp.c`, `adapter_trace.h`, `adapter_trace.c` | 2,349 | A reader of `tel`, in its own small C library or in each binding (**open**, [telemetry.md](telemetry.md)). |
| `watch.h`, `watch.c` | 321 | Mostly deleted. Watched sessions become a per-actor bit in core, set by address selector like the `recorded` bit. The operator-facing parts move to napi. |
| `allocator.h`, `allocator.c`, `memory.c`, `memory.h`, `containers.h`, `containers.c`, `platform.h` | about 650 | Back to napi (napi ADRs 0009 and 0014: the counting allocator and containers). With the scheduler gone, napi needs far fewer containers. Delete whatever is left unused. |
| `host_metrics.h`, `host_metrics.c`, `definitions.h`, `definitions.c`, `enumerate.h`, `enumerate.c` | 214 | napi: pump timings, how late timers fire, definition churn, cursor paging. All of it is binding work over core's API. |

### Tests

`tinyactors-host/tests/` has tests for the deleted modules, and they go
with them. The properties they check do not go: each is either a core
property now, tested in core, or a protocol property, tested on rings.

| Test | What its property becomes |
|---|---|
| scheduler, router, loads, directory tests | core tests over h2c and c2h: order between each sender and receiver, admission, recycling, load requests |
| `event_replay_test.c`, replay tests | core tests: replay with `h2c.spawn` (replaying), then `h2c.publish` |
| `replay_test` with `TAH_FULL=1` (the out-of-memory sweep over replay) | **no longer needed for macrosteps**, because a macrostep cannot fail to allocate. Keep a sweep over spawn, load and definition registration, which still allocate. |
| allocator, containers tests | napi, if the code stays there |

## napi

napi becomes a binding over core and its rings. What it keeps:

- the JavaScript data model (`napi_datamodel.c`, about 2,400 lines), unchanged;
- value boxes as vrefs (`values.c`, napi ADR 0011);
- codecs;
- the public definitions API, implemented by complete-spec requests and
  correlated results rather than a separate native management interface
  ([definitions.md](definitions.md));
- the loader and its timeouts, as JavaScript that answers [`c2h.load_needed`](../protocol/generated/MESSAGES.md#c2hload_needed);
- the pump, reduced to a short loop:
  1. post h2c;
  2. call `ta_run`;
  3. read each c2h record and hand it to I/O processors, invokers or the recorder on receipt; macrostep ends report completion;
  4. drain `tel` into the OTLP encoder;
  5. set one timer from [`c2h.wake`](../protocol/generated/MESSAGES.md#c2hwake).

| File | Change |
|---|---|
| `src/pump.c` (832) | rewritten around the loop above, much shorter |
| `src/napi_system.c` (2,115) | loses everything that wrapped `tah_system_*`. Gains ring setup. |
| `src/napi_services.c` (782) | reads `c2h.send`, `c2h.invoke`, `c2h.log` instead of kernel effects |
| `src/napi_loads.c` (555) | the loader answers `c2h.load_needed`. Waiting mail and dead letters go. |
| `src/napi_recorder.c` (167), `src/napi_replay.c` (165) | read `c2h.input`, post replay records. Most of the code goes. |
| `src/napi_trace.c` (722) | drains `tel` |
| `src/napi_children.c`, `src/import.c` | invoked children are local sessions. Their `done.invoke` is an envelope in core. |
| `lib/system.js`, `lib/loader.js`, `lib/tracing.js`, `lib/params.js` | follow the native changes. `send()` settles from the correlated input disposition: bounced, refused, or input_completed after stabilization and invocation-startup follow-up, independently of recording. |
| `index.d.ts` | the API it declares mostly stays. Mark what changes meaning with `// status:` (AGENTS.md, "Desired API files"). `setRecorder`'s `families` become recording selectors. Dead letter reasons change. |

Suites to rewrite: `route.js`, `loads.js`, `session-loads.js`,
`recorder.js`, `replay-events.js`, `persist.js`, `session-inputs.js`,
`scale.js` and `allocations.js`, since the zero-allocation property moves
from "after warm-up" to "always, in a macrostep".

## The test host

`tinyactors-test/src/hosted.c` (760 lines) wraps the kernel for the
`hosted.*` RPC methods. It becomes a **ring driver**:

- `hosted.post` writes h2c;
- `hosted.turn` calls `ta_run`;
- `hosted.batches` returns available c2h records, including records before
  a pending macrostep end, decoded by
  the catalog's JSON form ([`protocol/generated/messages.json`](../protocol/generated/messages.json)).

A scenario then reads as an h2c script, with expected c2h records. That
is how the [scenarios](../scenarios/) are written. The `ta.*` methods,
which the RPC layer generates one per core function, follow core's API
changes automatically.

## What core gains

| Gain | Reference |
|---|---|
| The three rings, their records, and a reader/writer API for each | [ring.md](ring.md), [MESSAGES.md](../protocol/generated/MESSAGES.md) |
| The envelope pool: one run queue, waiting lists per address, admission watermark, fairness, delayed sends as envelopes with a deadline index, recycling | [envelope.md](envelope.md) |
| The name area, and matching of run-time names that looks names up and never inserts | [names.md](names.md) |
| `ta_run(budget)`: one call that admits, runs to completion and writes records | [macrostep.md](macrostep.md) |
| `error.execution.capacity` and compiler-sized mandatory internal storage | [macrostep.md](macrostep.md) |
| The `recorded` and `watched` per-actor bits, set by address selector | [recording-and-replay.md](recording-and-replay.md), [telemetry.md](telemetry.md) |

What core loses:

- the router, I/O processor, invoker, log and trace sink vtables, which
  become records;
- `ta_actor_deliver`'s `TA_ERR_BUSY` as an admission answer;
- event blocks and their cache;
- interning of run-time names;
- per-actor sparse state for delayed sends;
- ADR 0070's machinery for stopping where it stands inside a macrostep;
- no input log byte layout returns: kernel ADR 0006 already removed ADR
  0090's format; `c2h.input` observes inputs for application-owned journals;
- `TA_OP_NOMEM` in `ta_op_status`. A data model or invoker that runs out of
  its own memory answers `TA_OP_ERROR`, and the chart sees
  `error.execution` ([macrostep.md](macrostep.md)). napi's data model and
  the test host's jq and quickjs data models change where they answer
  `TA_OP_NOMEM` today.

The data model vtable stays ([data-model.md](data-model.md)).

## ADRs

### Core ADRs superseded or refined

| ADR | Today | With this proposal |
|---|---|---|
| 0007 Structured interned event names | every name interned into a trie that only grows | **refined**: authored names only; run-time names are bytes, matched by lookup |
| 0011 The host owns external scheduling | the host chooses which actor receives which event | **superseded**: core schedules everything in its pool; the host chooses the order of h2c |
| 0012 Logical time and engine-owned delayed sends | | **kept**; time enters only by `h2c.time` and each input's `at` |
| 0015 Synchronous I/O callbacks | I/O processor called inside the macrostep | **superseded**: `c2h.send` is a record executable on receipt, including before the macrostep end |
| 0016 Invocation is a pluggable service boundary | invoker vtable | **refined**: the boundary is records |
| 0026, 0054 Delayed sends: actor-owned, globally indexed | sparse state per owner; global index of owners | **refined**: delayed sends are envelopes; the index is over the pool; they still travel with their owner's image |
| 0029 No per-actor external mailbox is required by core | no queue in core | **superseded**: core has a pool, with a list head per actor |
| 0037 Core-owned actor protocol, host-backed directory | directory vtable | **refined**: resident sessions are core's; unloaded ones are the host's index (**open**, [unloading.md](unloading.md)) |
| 0040 Built-in SCXML event processor, host-routed external events | core submits to the host's router | **superseded**: mail between local sessions stays in core |
| 0045 Log records go to a host sink | log sink vtable | **refined**: `c2h.log` |
| 0047 An interrupted macrostep is retryable | a macrostep interrupted by running out of memory can run again | **superseded**: core cannot run out of memory in a macrostep, and a data model or invoker that does answers `TA_OP_ERROR`, so nothing interrupts a macrostep for memory |
| 0055 Deliver is the admission test | `TA_ERR_BUSY` | **superseded**: admission is moving from h2c into the pool |
| 0064 An internal queue is lent to a running actor | lent, can grow | **superseded**: ordinary quota plus compiler-sized mandatory storage, allocated outside macrosteps; the whole-execution accumulation bound must be established |
| 0067 Core traces to a host sink | sink vtable | **refined**: the `tel` ring |
| 0069 An idle actor carries no event | event blocks | **refined**: the taken event stays in its envelope; no blocks |
| 0070 A run that runs out of memory stops where it stands | stopping inside a macrostep | **superseded** for core's memory; capacity becomes `error.execution.capacity` |
| 0074 An unloaded actor is offloaded to bytes | | **kept**; `c2h.unloaded` carries them |
| 0089, 0094 Replay before publish; every input carries its time | | **refined**: h2c inputs seed new execution before publish; exact restoration is not promised |
| 0090 Core defines the input log's byte layout | already removed by kernel ADR 0006 | remains removed; `c2h.input` is an observation, not a durable journal format |
| 0091, 0095 Checkpoints; refused sends restored | sends the router refused are restored with the checkpoint | 0091 **kept** (`c2h.state`); 0095 **superseded**: nothing is refused after the fact, a send fails in its macrostep |
| 0092 Parameters bound every buffer | | **kept**; new parameters ([BEHAVIORS.md](../BEHAVIORS.md#parameters)) |
| 0093 The event-block cache fills on demand | | **superseded** by the pool, which may fill on demand in the same way |

### Kernel ADRs 0001 to 0006

All six are **superseded**, because the package goes. They stay as
history, with a status line pointing at the ADR that removes the kernel.

| Kernel ADR | Decision | Replaced by |
|---|---|---|
| 0001 The kernel holds what every host needs | a C kernel between core and bindings | core holds it, as rings and a pool |
| 0002 The kernel keeps the write-ahead log | already superseded by 0006 | no WAL returns; `c2h.input` observes inputs |
| 0003 A full mailbox overflows instead of refusing mail between actors | overflow | the opposite: a full pool **refuses**, as `error.execution.capacity` in the sender |
| 0004 A macrostep's first record commits with what it sent | already superseded by 0006 | `c2h.macrostep_ended` reports completion without committing storage or holding effects |
| 0005 An actor's log is an inbox with acknowledgements | already superseded by 0006 | no write-ahead hold or durability acknowledgement returns |
| 0006 Actors record their events through the application | the recorder | the `recorded` bit + `c2h.input`; journaling remains the application's, and replay seeds new execution |

### napi ADRs that depend on the kernel

| napi ADR | Change |
|---|---|
| 0004 Event names are interned once | superseded: names are bytes |
| 0005 Per-target mailboxes and a ready queue | superseded: core's pool |
| 0006 The addon routes actor-to-actor events | superseded: core |
| 0007 The event loop drives a bounded pump | refined: the pump loop above |
| 0009, 0014 Native toolkit, memory pooled by lifetime | refined: smaller, back in napi |
| 0015 to 0018 Tracing hook, OTLP, encoder | refined: drains `tel` |
| 0020 Mail for an unloaded actor waits while a loader brings it back | refined: waits in core's pool; the loader answers `c2h.load_needed` |
| 0022 Mail a chart sends nowhere raises error.communication | kept, now done by recycling the envelope |
| 0032 to 0034 Persistence by replay; the input log | already superseded by kernel ADR 0006; input observations become `c2h.input` |
| 0035, 0036 Parameters | refined: kernel parameters go |
| 0037 napi is a binding over the host kernel | superseded: a binding over core's rings |
| 0040 Effects are not held | kept: effects may execute on receipt for every live session, independently of macrostep ends and journaling |

### ADRs to write

Each would be one ADR in `tinyactors-core/docs/adr/`, unless noted.

1. **Core talks to its host through three rings.** h2c, c2h and tel, and
   their record format. The vtables for routing, I/O, invocation, logs
   and traces go. The data model vtable stays.
2. **A macrostep never allocates core memory.** Capacity becomes
   `error.execution.capacity`. Supersedes 0070 for core's memory. Records
   the figures: per-actor bytes before and after.
3. **Pending inputs are envelopes in one pool per system.** One run
   queue in arrival order, admission watermark, fairness, recycling. Supersedes 0011, 0029,
   0040, 0055, 0064, 0069 and 0093. Records the bytes per envelope and the
   pool's default.
4. **Delayed sends are envelopes.** Exact `<cancel>`, one wake, timers
   travel in images. Refines 0026 and 0054.
5. **Only authored names are interned.** Run-time names are bytes,
   matched by lookup. Refines 0007. Fixes the memory that grows today.
6. **Recorded sessions expose input observations on c2h.** Journaling is
   application policy. Replay seeds new execution before publish; effects
   execute on receipt in live execution, without a write-ahead hold.
7. **Telemetry is a lossy ring.** Values as text. Refines 0067.
8. In `docs/adr/` (the repository's own): **remove `tinyactors-host`**.
   Supersedes kernel ADRs 0001 to 0006.
9. In `tinyactors-napi/docs/adr/`: **napi is a binding over core's
   rings.** Supersedes napi 0037 and the others above.

## A rough order of work

Each step builds and passes `mise run test`, and is committed on its own
(AGENTS.md).

1. **Names first.** Intern authored names only, and match run-time names
   by lookup ([names.md](names.md)). It stands alone, fixes memory that
   grows today, and touches only core and the kernel's events cache.
2. **The ring module in core**, with tests, used by nothing yet.
3. **`tel`.** Move trace records onto it. The kernel's OTLP encoder reads
   it. This is the smallest real user, and it is already shaped like a
   stream.
4. **The pool and `ta_run`.** Mail between local sessions and delayed
   sends become envelopes. The kernel's scheduler and router are bypassed
   for local mail.
5. **c2h.** Sends, invocations and logs become records. The kernel's
   adapters and effects queue go.
6. **h2c.** Inputs, complete definitions, spawn, time and load go through it. The kernel's
   loads, offloads and directory go.
7. **Recording and replay** on `c2h.input`. The kernel's recorder and
   replay go.
8. **Delete `tinyactors-host`.** Move what is left (OTLP, the allocator,
   metrics) to its new homes.
9. **The test host** as a ring driver. The scenarios become its tests.

Steps 4 to 6 are the expensive ones. Each removes a vtable from core and
changes every binding in the same change.

## Decided along the way

- **Debugging temporarily replaces the production driver.** Attachment
  suspends the pump and transfers exclusive ownership of the whole system.
  Debugger commands stay outside the rings; native primitives support
  inspection, abandonment at legal boundaries and operator transitions
  through the normal engine. Detachment returns ownership to the pump.
  Actor-only holds and interleaved stepping are outside the initial design
  ([debugging.md](debugging.md)).
- **Definition management uses requests and results.** Hosts submit a
  complete declarative spec; core compiles and registers it between
  macrosteps. Structured compile errors travel through bounded buffers.
  Native data model and value ownership integration stays desired.
  Exact spec and diagnostic encodings remain sketches
  ([definitions.md](definitions.md)).
- **The data model stays a synchronous vtable.** Data model requests on the
  ring ("option (b)") are rejected: in-process data models gain nothing,
  and every action would have to become resumable in the middle
  ([data-model.md](data-model.md#decided-the-data-model-stays-a-synchronous-vtable)).
- **A `ta_system` is not thread-safe.** The rings are used on the system's
  own thread only. A host that works on other threads puts its own queue or
  channel in front of the system ([ring.md](ring.md#one-thread)), so core
  needs no atomics. This is what core ADR 0017 already assumes.

## Open questions

- **Fairness.** Is a ratio of mail to admitted input enough, or does
  core need round robin across targets so that one busy actor cannot
  delay another's mail? ([envelope.md](envelope.md))
- **Default sizes.** The pool, the name area and the rings
  ([BEHAVIORS.md](../BEHAVIORS.md#parameters)). Busy servers and test suites
  pull in opposite directions.
- **The MCP server proposal** ([mcp-server.md](../../mcp-server.md))
  places its mapping in the host kernel, "implemented once". With the
  kernel gone, it needs a new home. Candidates: a small C library beside
  the OTLP reader, or core itself for the parts that are only reads of
  core state. Its live views could read `tel` and c2h.
- **The SQLite and PostgreSQL sketches** ([sqlite-host.md](../../sqlite-host.md),
  [postgres-host.md](../../postgres-host.md)) keep a `ta_mailbox` table
  and drain it into core. With the pool in core, the table holds only
  what the host has not yet posted to h2c. `ta_outbox` is an application
  policy for c2h effects, which the protocol permits on receipt. Both
  sketches also keep session images as durable
  state, which this proposal rules out: an image only bounds memory
  ([unloading.md](unloading.md#unloading-bounds-memory-it-is-not-persistence)).
  They would persist a session as [`c2h.state`](../protocol/generated/MESSAGES.md#c2hstate)
  plus its [`c2h.input`](../protocol/generated/MESSAGES.md#c2hinput) records,
  journaled on the application's schedule. A shared transaction with
  `ta_outbox` is the application's choice, not a protocol requirement.
  Their persistence notes would need updating to this contract. Neither
  has been tried.
- **Replay seeds new execution.** It derives configuration and data model
  values and may produce different state. Recording adds no effect hold
  or write-ahead rule. Whole-system replay uses images produced by the new
  execution, with load failures handled by the host; it does not restore
  the old system's exact in-flight mail
  ([recording-and-replay.md](recording-and-replay.md)).
