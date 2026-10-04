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
| `scheduler.h`, `scheduler.c` | 1,465 | Mailboxes, runnable actors, turn budgets, quarantine, dead letters. Replaced by core's pool and its run queue ([envelope.md](envelope.md)), macrostep slots with deferred lists ([macrostep.md](macrostep.md#macrostep-slots)) and `ta_run`'s budget. Quarantine is removed, not moved: the chain limit ends a runaway's input ([macrostep.md](macrostep.md#runaways-the-chain-limit)). |
| `router.h`, `router.c` | 232 | `ta_external_event_router` for mail between sessions. Mail between local sessions is an envelope in core. Anything else is a [`c2h.send`](../../../../tinyactors-core/src/generated/MESSAGES.md#c2hsend). |
| `effects.h`, `effects.c` | 98 | The per-turn effect queue that kept effects until JavaScript could run. Replaced by c2h records executable on receipt, including before the macrostep end. |
| `adapter_io.h`, `adapter_io.c` | 260 | The I/O processor adapter that copied sends into effects. Replaced by `c2h.send`. |
| `adapter_invoker.h`, `adapter_invoker.c` | 210 | The invoker adapter. Replaced by [`c2h.invoke`](../../../../tinyactors-core/src/generated/MESSAGES.md#c2hinvoke), [`c2h.invoke_cancel`](../../../../tinyactors-core/src/generated/MESSAGES.md#c2hinvoke_cancel), [`c2h.invoke_forward`](../../../../tinyactors-core/src/generated/MESSAGES.md#c2hinvoke_forward) and [`h2c.invoke_report`](../../../../tinyactors-core/src/generated/MESSAGES.md#h2cinvoke_report). |
| `adapter_log.h`, `adapter_log.c` | 34 | The buffered log sink. Replaced by [`c2h.log`](../../../../tinyactors-core/src/generated/MESSAGES.md#c2hlog). |
| `recorder.h`, `recorder.c` | 509 | Collecting the events recorded families took, once per turn. Replaced by [`c2h.input`](../../../../tinyactors-core/src/generated/MESSAGES.md#c2hinput) and the `recorded` bit ([recording-and-replay.md](recording-and-replay.md)). |
| `replay.h`, `replay.c` | 247 | `tah_replay_event`, `tah_replay_events_done`. Replaced by `h2c.spawn` (replaying), `h2c.event` and `h2c.publish`. Core's replay semantics (ADR 0089, 0094) stay. |
| `loads.h`, `loads.c` | 529 | Load records, waiting mailboxes, wakeups. Replaced by waiting lists in the pool and four records ([unloading.md](unloading.md)). |
| `offloads.h`, `offloads.c` | 99 | Images of offloaded actors, kept per address. The host keeps the `blob` from [`c2h.unloaded`](../../../../tinyactors-core/src/generated/MESSAGES.md#c2hunloaded). |
| `directory.h`, `directory.c` | 118 | The in-memory directory with unloaded entries. Core keeps the resident ones. Unloaded addresses are in the host's index. |
| `events.h`, `events.c` | 30 | The cache of interned event names (napi ADR 0004). Run-time names are bytes now ([names.md](names.md)). |
| `hooks.h`, `hooks.c` | 57 | Post-call hooks that told the plumbing about new and removed actors. There is no plumbing left to tell. [`c2h.spawned`](../../../../tinyactors-core/src/generated/MESSAGES.md#c2hspawned) and [`c2h.destroyed`](../../../../tinyactors-core/src/generated/MESSAGES.md#c2hdestroyed) carry the same facts. |
| `params.h`, `params.c` | 22 | Kernel parameters. Removed or replaced ([BEHAVIORS.md](../BEHAVIORS.md#removed-parameters)). |
| `system.h`, `system.c` | 687 | The native system record, the pump driver, entering and leaving core. What is left is napi's: one `ta_system`, its rings, and the pump. |
| `version.c` | 6 | |
| **Total** | **about 4,600** | |

### Moved: still needed, but not in a kernel

| Module | Lines (src) | Goes to |
|---|---|---|
| `otlp.h`, `otlp.c`, `adapter_trace.h`, `adapter_trace.c` | 2,349 | `tinyactors-otlp`: a small C library that reads `tel` and encodes OTLP traces and logs. napi and the test host link it ([telemetry.md](telemetry.md#otlp-tinyactors-otlp)). |
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
| `replay_test` with `TAH_FULL=1` (the out-of-memory sweep over replay) | **not needed for macrosteps**, because a macrostep cannot fail to allocate. Keep a sweep over spawn, load and definition registration, which still allocate. |
| allocator, containers tests | napi, if the code stays there |

## napi

napi becomes a binding over core and its rings. What it keeps:

- the JavaScript data model (`napi_datamodel.c`, about 2,400 lines), unchanged;
- value boxes as vrefs (`values.c`, napi ADR 0011);
- codecs;
- the public definitions API, implemented by complete-spec requests and
  correlated results rather than a separate native management interface
  ([definitions.md](definitions.md));
- the loader and its timeouts, as JavaScript that answers [`c2h.load_needed`](../../../../tinyactors-core/src/generated/MESSAGES.md#c2hload_needed);
- the pump, reduced to a short loop:
  1. post h2c;
  2. call `ta_run`;
  3. read each c2h record and hand it to I/O processors, invokers or the recorder on receipt; macrostep ends report completion;
  4. drain `tel` into `tinyactors-otlp`;
  5. set one timer from [`c2h.wake`](../../../../tinyactors-core/src/generated/MESSAGES.md#c2hwake).

| File | Change |
|---|---|
| `src/pump.c` (832) | rewritten around the loop above, much shorter |
| `src/napi_system.c` (2,115) | loses everything that wrapped `tah_system_*`. Gains ring setup. |
| `src/napi_services.c` (782) | reads `c2h.send`, `c2h.invoke`, `c2h.log` instead of kernel effects |
| `src/napi_loads.c` (555) | the loader answers `c2h.load_needed`. Waiting mail and dead letters go. |
| `src/napi_recorder.c` (167), `src/napi_replay.c` (165) | read `c2h.input`, post replay records. Most of the code goes. |
| `src/napi_trace.c` (722) | drains `tel` |
| `src/napi_children.c`, `src/import.c` | invoked children are local sessions. Their `done.invoke` is an envelope in core. |
| `lib/system.js`, `lib/loader.js`, `lib/tracing.js`, `lib/params.js` | follow the native changes. `send()` settles from the correlated input disposition: bounced, refused, or input_completed after stabilization and invocation-startup follow-up, independently of recording. `PostOptions.signal` aborting before the input is taken posts [`h2c.withdraw`](../../../../tinyactors-core/src/generated/MESSAGES.md#h2cwithdraw). |
| `index.d.ts` | the API it declares mostly stays. Mark what changes meaning with `// status:` (AGENTS.md, "Desired API files"). `setRecorder`'s `families` become recording selectors. Dead letter reasons change. `IOProcessorOptions.location` becomes a function `location({id, address})`; its `origin` template, `IOOrigin`'s `scope` and `token` forms, `openScope()` and `closeScope()` go ([S12](../scenarios/S12-http-request.md), [S13](../scenarios/S13-webhook-callback.md)). `SendResult.consumed` comes from the `ignored` flag of `c2h.input_completed`. |

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
  the catalog's JSON form ([`protocol/generated/messages.json`](../../../../tinyactors-core/src/generated/messages.json)).

A scenario then reads as an h2c script, with expected c2h records. That
is how the [scenarios](../scenarios/) are written. The `ta.*` methods,
which the RPC layer generates one per core function, follow core's API
changes automatically.

## What core gains

| Gain | Reference |
|---|---|
| The three rings, their records, and a reader/writer API for each | [ring.md](ring.md), [MESSAGES.md](../../../../tinyactors-core/src/generated/MESSAGES.md) |
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
| 0016 Invocation is a pluggable service boundary | invoker vtable | **refined**: the boundary is records. The invoker registry becomes a set of declared types (`h2c.type_declare`, registry `invoker`), so an undeclared `<invoke type>` still raises `error.execution`; `ta_system_register_invoker` and the vtable go ([building blocks](building-blocks.md#io-processors-and-invokers-one-type-registry-twice)) |
| 0026, 0054 Delayed sends: actor-owned, globally indexed | sparse state per owner; global index of owners | **refined**: delayed sends are envelopes; the index is over the pool; they still travel with their owner's image |
| 0029 No per-actor external mailbox is required by core | no queue in core | **superseded**: core has a pool, with a list head per actor |
| 0037 Core-owned actor protocol, host-backed directory | directory vtable | **refined**: resident sessions are core's; unloaded ones are the host's index. The directory vtable is removed: an address that is not resident is asked for with `c2h.load_needed` ([unloading.md](unloading.md#no-directory-vtable-not-resident-means-ask)) |
| 0040 Built-in SCXML event processor, host-routed external events | core submits to the host's router | **superseded**: mail between local sessions stays in core |
| 0045 Log records go to a host sink | log sink vtable | **refined**: `c2h.log` |
| 0047 An interrupted macrostep is retryable | a macrostep interrupted by running out of memory can run again | **superseded**: core cannot run out of memory in a macrostep, and a data model or invoker that does answers `TA_OP_ERROR`, so nothing interrupts a macrostep for memory |
| 0055 Deliver is the admission test | `TA_ERR_BUSY` | **superseded**: admission is the move from h2c into the pool; mail for an active session waits on its deferred list in core, so the host never sees busy |
| 0061 A run can pause between units of executable content | the paused state moves into a continuation the actor owns | **refined**: it moves into the session's macrostep slot, and slots take turns between units |
| 0064 An internal queue is lent to a running actor | lent, can grow | **superseded**: one live queue in the running workspace, parked slots' entries in the park area, an ordinary quota plus a compiler-sized mandatory floor of one round of every phase, allocated outside macrosteps; mandatory events beyond the floor use free ordinary entries, and an overflow is a runaway ([macrostep.md](macrostep.md#sizing-mandatory-storage)) |
| 0067 Core traces to a host sink | sink vtable | **refined**: the `tel` ring. Trace parents and step keys are **kept**: trace context travels on records (`h2c.event`, `c2h.send`, `c2h.input` and their kin) and, for local mail, in the envelope ([telemetry.md](telemetry.md#trace-context-across-the-rings)) |
| 0068 Core owns the I/O processor registry; every origin can be answered | a registry of (type, adapter) with a `text` callback; COMPUTED, SCOPED and MESSAGE origins | **refined**: the registry becomes a set of type atoms, declared by `h2c.type_declare` (registry `io`), so an unsupported `<send type>` still raises `error.execution` in the sender's block. `send` becomes `c2h.send`. The binding renders `_ioprocessors.<type>.location`, so `text` and its templates go. Origins are text in `h2c.event`; COMPUTED and SCOPED go, and the processor decides which reply targets are live ([S12](../scenarios/S12-http-request.md), [S13](../scenarios/S13-webhook-callback.md), [building blocks](building-blocks.md#io-origins-adr-0068-one-class-remains)) |
| 0069 An idle actor carries no event | event blocks | **refined**: the taken event stays in its envelope; no blocks |
| 0070 A run that runs out of memory stops where it stands | stopping inside a macrostep | **superseded** for core's memory; capacity becomes `error.execution.capacity` |
| 0073 Core builds, generates and samples trace contexts | `ta_trace_parent` on deliveries, sends and invoke requests; `ta_traceparent_parse` and `_format` | **kept**: core still builds and samples contexts, and the parse and format helpers stay native. The trace parent travels on records: in on `h2c.event`, `h2c.invoke_report` and `h2c.spawn`, out on `c2h.send`, `c2h.invoke`, `c2h.invoke_forward` and `c2h.log`, recorded on `c2h.input`, and in the envelope's name-area text for local mail ([telemetry.md](telemetry.md#trace-context-across-the-rings)) |
| 0075 A macrostep that runs too many microsteps stops as a runaway | counts committed microsteps in one macrostep; 0 means no limit; the host decides | **refined**: counts one input's whole chain (microsteps, selection passes, startup passes), must be finite; at the limit core abandons the input with status `runaway`, and the session takes its next input. The host still decides what to do about a session that keeps running away |
| 0082 Operator transitions and abandoning a macrostep | | **kept**; the chain limit applies its abandon to a runaway's input |
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
| 0008 Faults are per actor | refined: point 1's quarantine is removed; points 2 to 4 (the pump never throws, only misuse throws, generated result codes) stay. Its causes are gone: a refused delivery, a queue that could not grow and out-of-memory back-off no longer exist, and a runaway's input is abandoned by core. The `fault` hook reports `c2h.input_completed` with status `runaway`. `release()` and the `"quarantined"` scheduling state go from `index.d.ts` |
| 0009, 0014 Native toolkit, memory pooled by lifetime | refined: smaller, back in napi |
| 0015 to 0018 Tracing hook, OTLP, encoder | refined: `tinyactors-otlp` drains `tel` |
| 0020 Mail for an unloaded actor waits while a loader brings it back | refined: waits in core's pool; the loader answers `c2h.load_needed` |
| 0025 A quarantined child tells its parent | superseded: there is no quarantine to report. A child whose input ran away is not stuck; a parent waiting for its `done.invoke` relies on its own timeout |
| 0022 Mail a chart sends nowhere raises error.communication | kept, now done by recycling the envelope |
| 0032 to 0034 Persistence by replay; the input log | already superseded by kernel ADR 0006; input observations become `c2h.input` |
| 0035, 0036 Parameters | refined: kernel parameters go |
| 0037 napi is a binding over the host kernel | superseded: a binding over core's rings |
| 0040 Effects are not held | kept: effects may execute on receipt for every live session, independently of macrostep ends and journaling |

### ADRs to write

Each would be one ADR in `tinyactors-core/docs/adr/`, unless noted.

1. **Core talks to its host through three rings.** h2c, c2h and tel, and
   their record format. The vtables for routing, I/O, invocation, logs,
   traces and the directory go; the data model vtable stays.
   - **A full c2h parks the slot.** Before an action that may write c2h,
     and before it evaluates anything, core checks for room for the
     largest record beside every slot's reserve. Without it, the slot
     parks and the action runs once, after the host drains; it never
     fails the action ([macrostep.md](macrostep.md#a-full-c2h-pauses-the-slot)).
     A full pool or name area fails the action in the chart, because only
     sessions free them.
   - **The c2h reserve.** Each record of an input's mandatory batch
     reserves its aligned size plus one pad of up to
     `maxRecordBytes - 8` bytes. Core takes an input only when 2 ×
     `maxRecordBytes` of headroom stays unreserved. `c2hBytes` defaults
     to `(3 × maxConcurrentInputs + 2) × maxRecordBytes`
     ([ring.md](ring.md#the-c2h-reserve)).
   - **A host never makes draining c2h wait for room in h2c**
     ([macrostep.md](macrostep.md#a-full-c2h-pauses-the-slot)).
   - **Type registries are declared through h2c.** I/O processor and
     invoker types are atoms declared by `h2c.type_declare`; an undeclared
     `<send type>` or `<invoke type>` still raises `error.execution`.
     Refines 0016 and 0068.
   - **Definitions are requests and results.** An h2c request carries a
     handle to a complete native `ta_scxml_spec`, built with core's spec
     builder; core compiles and registers it between macrosteps and
     answers with `c2h.definition_result`, with ADR 0058's native
     compile-error struct in bounded host memory. A spec has no byte
     layout; a host with a chart as text uses a chart codec (ADR 0053)
     ([definitions.md](definitions.md#a-native-spec-handle-not-a-byte-layout)).
   - **No directory vtable.** An address that is not resident is asked
     for with `c2h.load_needed`. Refines 0037
     ([unloading.md](unloading.md#no-directory-vtable-not-resident-means-ask)).
2. **A macrostep never allocates core memory.** Capacity becomes
   `error.execution.capacity`. Supersedes 0070 for core's memory. Records
   the figures: per-actor bytes before and after.
   - **Mandatory internal events** (errors, completion data) have a
     floor the compiler sizes as one round of every phase, allocated
     outside macrosteps. Beyond the floor they take free entries of the
     ordinary quota, in one FIFO order with ordinary events. One that fits
     in neither ends the input as a `runaway`, with `runaway_cause`
     `internal_queue` ([macrostep.md](macrostep.md#sizing-mandatory-storage)).
3. **Pending inputs are envelopes in one pool per system.** Supersedes
   0011, 0029, 0040, 0055, 0064, 0069 and 0093. Records the bytes per
   envelope, the pool's default and the ring defaults, measured in step
   4.2.
   - **One run queue in arrival order,** with an admission watermark and
     recycling. `fairnessRatio` between mail and admitted input is the
     only fairness policy. Mail that waited on a deferred list returns to
     the run queue at the position its arrival gives it, not at the head.
     Round robin across targets would be internal scheduling, with no
     protocol change ([envelope.md](envelope.md#fairness)).
   - **No cap per address.** The pool is the only bound on envelopes that
     wait for a load; the host bounds how long they wait
     ([unloading.md](unloading.md#waiting-envelopes-count-against-the-pool)).
   - **Withdrawal is a new record.** Committed records never change.
     [`h2c.withdraw`](../../../../tinyactors-core/src/generated/MESSAGES.md#h2cwithdraw) names
     an admitted input by its h2c seq. If it has not been taken, core
     frees its envelope and writes `c2h.input_refused` (`withdrawn`);
     otherwise the input's own terminal record stands, and only an
     operator abandon stops it
     ([envelope.md](envelope.md#withdrawing-an-admitted-input)).
   - **The host gives memory back.** `ta_system_trim` frees whole free
     pages above each reserve in the pool, the name area, the park
     area and the deadline index, between `ta_run` calls; core never trims on its own
     ([macrostep.md](macrostep.md#parking-one-running-workspace-and-a-park-area)).
4. **Delayed sends are envelopes.** Exact `<cancel>`, one wake, timers
   travel in images. Refines 0026 and 0054.
5. **Only authored names are interned.** Run-time names are bytes,
   matched by lookup. Atoms are never freed and their IDs never reused;
   growth is bounded by what operators register, not by traffic. Refines
   0007. Fixes the memory that grows today
   ([names.md](names.md#atoms-are-never-freed)).
6. **Recorded sessions expose input observations on c2h.** Journaling is
   application policy. Effects execute on receipt in live execution,
   without a write-ahead hold. Replay seeds new execution before publish:
   it derives configuration and data model values and may produce
   different state. Whole-system replay uses images produced by the new
   execution, with load failures handled by the host, and does not
   restore the old system's in-flight mail
   ([recording-and-replay.md](recording-and-replay.md)).
7. **Telemetry is a lossy ring.** `tel.trace` records mirror core's trace
   kinds one for one and are self-contained: names, values as text,
   faults and state lists in the tail. Core stamps each with a clock the
   host installs natively as a C function. `tinyactors-otlp` reads them
   and builds OTLP spans; a `tel.dropped` ends every open span with error
   status. `telBytes` 0 creates no `tel` ring. Trace context travels on
   records as it travels on calls today: `h2c.event`, `h2c.spawn` and
   `h2c.invoke_report` carry a trace parent in; `c2h.send`, `c2h.invoke`,
   `c2h.invoke_forward` and `c2h.log` carry the step's span ID out; local
   mail keeps it in the envelope's text. Refines 0067 and 0073
   ([telemetry.md](telemetry.md#trace-context-across-the-rings)).
8. In `docs/adr/` (the repository's own): **remove `tinyactors-host`**.
   Supersedes kernel ADRs 0001 to 0006.
9. In `tinyactors-napi/docs/adr/`: **napi is a binding over core's
   rings.** Supersedes napi 0037 and the others above.
10. **Macrostep slots.** Up to `maxConcurrentInputs` sessions run inputs at
    once, taking turns between units of executable content; each session
    runs one input at a time, and a long or looping macrostep holds one
    slot. Deferred lists; one running workspace and a park area of chunks
    with a derived ceiling.
    - **The chain limit** counts one input's microsteps, selection passes
      and startup passes, and cannot be turned off. At the limit core
      abandons the input with status `runaway`
      ([macrostep.md](macrostep.md#runaways-the-chain-limit)).
    - **Quarantine is removed** (napi ADRs 0008 and 0025). Slots keep one
      bad actor from taking the loop's turns. Holding a runaway would let
      its mail fill the pool, leave its input's promise pending and stop a
      looping replay seed from being published. An operator reads a live
      actor with `ta_actor_inspect` and intervenes by attaching the
      debugger.

    Refines 0055, 0061 and 0075. Records the bytes per slot record, per
    parked execution and per actor (+12 bytes on today's 332: the head of
    the owner list of delayed sends, and the head and tail of the
    deferred list; [envelope.md](envelope.md#memory-as-estimates)).

### Settled boundaries

These hold with the proposal and need no ADR of their own:

- **Debugging temporarily replaces the production driver.** Attachment
  suspends the pump and transfers exclusive ownership of the whole system.
  Debugger commands stay outside the rings; native primitives support
  inspection, abandonment at legal boundaries and operator transitions
  through the normal engine (ADR 0082). Detachment returns ownership to
  the pump. Reading needs no attachment: `ta_actor_inspect` stays a
  native call between `ta_run` calls and marks a session stable or active
  ([debugging.md](debugging.md#inspection-without-attaching)). A debugger that holds one actor while others run is outside
  the design ([debugging.md](debugging.md)).
- **The data model stays a synchronous vtable.** In-process data models
  gain nothing from requests on a ring, and every action would have to
  become resumable in the middle ([data-model.md](data-model.md)).
- **A `ta_system` is not thread-safe.** The rings are used on the system's
  own thread only. A host that works on other threads puts its own queue or
  channel in front of the system ([ring.md](ring.md#one-thread)), so core
  needs no atomics. Core ADR 0017 already assumes this.

## A rough order of work

[IMPLEMENTATION.md](../IMPLEMENTATION.md) is the order of work. It keeps
the code base and replaces core's plumbing with the building blocks in
phases. Each commit passes `mise run test`. The test host becomes a ring
driver when h2c and c2h arrive, not at the end.

## Open questions

- **Default sizes.** h2c and tel default to 16 KiB each; c2h is derived
  from `maxConcurrentInputs` and `maxRecordBytes` (776 KiB with the
  defaults) ([BEHAVIORS.md](../BEHAVIORS.md#parameters)). Ring size is a
  throughput setting, not a correctness one. Step 4.2 measures the h2c and
  tel defaults and checks the derived c2h against napi's suites.
- **The MCP server proposal** ([mcp-server.md](../../mcp-server.md))
  needs revising against the ring protocol, after Phase 5, when its
  records exist. It is not on this plan's critical path: nothing
  implements its mapping, so deleting the kernel (Phase 6) breaks
  nothing. The revision settles three things:
  - its home, with the kernel gone. A small C library beside
    `tinyactors-otlp` is one candidate: `send` becomes `h2c.event` and
    waits for its `c2h.input_completed`, and `load` becomes the load
    records;
  - `goto`. An operator transition queued behind one actor's mail while
    the system runs conflicts with this proposal, which allows operator
    transitions only while a debugger owns the whole system
    ([debugging.md](debugging.md));
  - its I/O processor's origin scopes, which this proposal removes.
- **The SQLite and PostgreSQL sketches** ([sqlite-host.md](../../sqlite-host.md),
  [postgres-host.md](../../postgres-host.md)) need updating to this
  contract. They keep a `ta_mailbox` table and drain it into core; with
  the pool in core, the table holds only what the host has not yet posted
  to h2c. `ta_outbox` is an application policy for c2h effects, which the
  protocol permits on receipt. Both sketches keep session images as
  durable state, which this proposal rules out: an image only bounds
  memory ([unloading.md](unloading.md#unloading-bounds-memory-it-is-not-persistence)).
  They would persist a session as [`c2h.state`](../../../../tinyactors-core/src/generated/MESSAGES.md#c2hstate)
  plus its [`c2h.input`](../../../../tinyactors-core/src/generated/MESSAGES.md#c2hinput) records,
  journaled on the application's schedule. A shared transaction with
  `ta_outbox` is the application's choice, not a protocol requirement.
  Neither sketch has been tried.
