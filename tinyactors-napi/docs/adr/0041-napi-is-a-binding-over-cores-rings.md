# ADR 0041: napi is a binding over core's rings

**Status:** Accepted (2026-10-05); not yet implemented. Supersedes napi ADRs 0004, 0005, 0006, 0025 and 0037; refines 0007, 0008, 0009, 0014, 0015 to 0018, 0020, 0035 and 0036; keeps 0022 and 0040. Applies [the ring protocol](../../../docs/proposals/ring-proto/README.md); see "Relations to other ADRs".
**Date:** 2026-10-05

## Context

ADR 0037 made napi a binding over the host kernel, `tinyactors-host`. The
kernel holds the scheduler, mailboxes, routing, loads, offloads, the
effect queue, the recorder, replay and the OTLP encoder. napi drives it
from `pump.c` and wraps `tah_system_*` in `napi_system.c`.

The ring protocol removes the kernel
([migration.md](../../../docs/proposals/ring-proto/reference/migration.md)).
Core and its host exchange records over three rings in memory that core
provides:

- **`h2c`** carries every input, from the host to core.
- **`c2h`** carries everything that leaves the process or must not be
  lost.
- **`tel`** carries telemetry, which may be dropped.

Core keeps one pool of envelopes for every pending input, schedules every
session itself, and runs macrosteps in slots with `ta_run(budget)`. What
the kernel did is now done by core, by a record, or by the binding. This
ADR says what is left for napi.

## Decision

### 1. What napi keeps

napi becomes a binding over core and its rings. It keeps:

- **The JavaScript data model** (`napi_datamodel.c`), unchanged. The data
  model stays a synchronous vtable
  ([data-model.md](../../../docs/proposals/ring-proto/reference/data-model.md)).
- **Value boxes as vrefs** (`values.c`, ADR 0011). A record's data is a
  value reference. The drain takes ownership of it.
- **Codecs** (ADR 0027), and the chart codec for `define(text, { format })`.
- **The public definitions API.** napi builds a complete native
  `ta_scxml_spec` with core's spec builder and posts
  `h2c.definition_register` with its handle. Core answers with
  `c2h.definition_result`. There is no separate native management
  interface
  ([definitions.md](../../../docs/proposals/ring-proto/reference/definitions.md)).
- **The loader and its timeouts, as JavaScript.** The loader answers
  [`c2h.load_needed`](../../../tinyactors-core/src/generated/MESSAGES.md#c2hload_needed)
  with `h2c.load`, `h2c.spawn` or `h2c.load_failed`, echoing its
  `request_seq`. Mail for an unloaded actor waits in core's pool, not in
  napi
  ([unloading.md](../../../docs/proposals/ring-proto/reference/unloading.md)).
  napi keeps the index of unloaded sessions: a `Map` from address to
  `{image, wake_at}` and a min-heap by `wake_at`.
- **The pump**, reduced to a short loop (section 2).
- **The generated bindings and `lib/`**, as the public API (section 4).

### 2. The pump is a short loop

One pump turn:

1. **Post h2c.** Flush the bounded result buffer first, then load
   answers, load failures and clock updates, then new inputs.
2. **Call `ta_run`.** It admits from h2c and runs macrosteps. It returns
   on the budget, when idle, or with `TA_RUN_C2H_FULL`; then napi drains
   c2h and calls it again.
3. **Drain c2h.** Hand each record on receipt to an I/O processor
   (`c2h.send`), an invoker (`c2h.invoke`, `c2h.invoke_cancel`,
   `c2h.invoke_forward`), the logger (`c2h.log`), the recorder
   (`c2h.input`) or the loader (`c2h.load_needed`). Settle promises from
   `c2h.input_completed`, `c2h.input_refused` and `c2h.bounced`.
4. **Drain `tel` into `tinyactors-otlp`.** Then call the JavaScript trace
   hook with `{traces, logs}` bytes.
5. **Arm one timer** from
   [`c2h.wake`](../../../tinyactors-core/src/generated/MESSAGES.md#c2hwake).
   napi may merge it with the timer of its unloaded-session index.

**The drain rule**
([ARCHITECTURE.md, "The host loop"](../../../docs/proposals/ring-proto/ARCHITECTURE.md#the-host-loop)):

- The drain consumes each c2h record it reads. It decodes the record,
  takes its value references, calls the handler and releases the
  record's ring space.
- It never awaits a handler before it releases the record.
- Draining c2h never waits for room in h2c.
- A result a handler posts (an I/O acknowledgement, an invoke report)
  goes to h2c if there is room. Otherwise it goes into napi's bounded
  result buffer, which the next turn flushes. If that buffer is full,
  napi drops the result by policy; the chart's timeout covers it.

The setImmediate and setTimeout driving of ADR 0007 stays. What a turn
does inside is the loop above.

### 3. Telemetry and the clock

- **`tinyactors-otlp`** is a small C library that reads `tel.trace` and
  encodes OTLP traces and logs into memory napi provides (ADRs 0016 to
  0018). napi and the test host link it
  ([telemetry.md](../../../docs/proposals/ring-proto/reference/telemetry.md#otlp-tinyactors-otlp)).
- **The wall clock is a C function.** napi installs it at system creation
  (`clock_gettime`). Core calls it for each `tel` record it writes. It
  never calls into JavaScript.
- **Logical time** enters only by `h2c.time` and each input's `at`.

### 4. The JavaScript API

- **`send()` settles from its input's disposition.** It resolves from
  `c2h.input_completed` for its `source_seq`, after stabilization and
  invocation-startup follow-up, whether or not the session is recorded.
  It rejects from `c2h.input_refused` or `c2h.bounced`.
  `SendResult.consumed` comes from the `ignored` flag of
  `c2h.input_completed`.
- **An `AbortSignal` posts `h2c.withdraw`.** When `PostOptions.signal`
  aborts before the input is taken, napi posts
  [`h2c.withdraw`](../../../tinyactors-core/src/generated/MESSAGES.md#h2cwithdraw)
  with the input's h2c seq. Core answers `c2h.input_refused`
  (`withdrawn`). After the input is taken, its own terminal record
  stands.
- **Quarantine goes.** The chain limit ends a runaway's input, and slots
  keep one bad actor from taking the loop's turns
  ([macrostep.md](../../../docs/proposals/ring-proto/reference/macrostep.md#runaways-the-chain-limit)).
  `release()`, the `"quarantined"` scheduling state, and the parent's
  `error.communication.invoke.<id>.quarantined` event go. The `fault`
  hook reports `c2h.input_completed` with status `runaway`.
- **Invoked children are local sessions.** Their `done.invoke` is an
  envelope in core.
- **`index.d.ts` follows,** and marks with `// status:` what the
  implementation has not caught up with (AGENTS.md, "Desired API
  files"):
  - `setRecorder`'s `families` become recording selectors;
  - dead letter reasons change: `waiting-full`, `not-found`, `rejected`
    and `load-failed` become recycled `error.communication` to senders,
    or dropped external events reported on `tel`;
  - `IOProcessorOptions.location` becomes a function
    `location({id, address})`;
  - its `origin` template, `IOOrigin`'s `scope` and `token` forms,
    `openScope()` and `closeScope()` go
    ([S12](../../../docs/proposals/ring-proto/scenarios/S12-http-request.md),
    [S13](../../../docs/proposals/ring-proto/scenarios/S13-webhook-callback.md));
  - `release()` and `"quarantined"` go.

  The rest of the API keeps its shape.

## Relations to other ADRs

From the table "napi ADRs that depend on the kernel" in
[migration.md](../../../docs/proposals/ring-proto/reference/migration.md#napi-adrs-that-depend-on-the-kernel):

| napi ADR | Relation |
|---|---|
| 0004 Event names are interned once | **superseded**: run-time names are bytes |
| 0005 Per-target mailboxes and a ready queue | **superseded**: core's pool |
| 0006 The addon routes actor-to-actor events | **superseded**: core routes them |
| 0007 The event loop drives a bounded pump | **refined**: the pump loop of section 2 |
| 0008 Faults are per actor | **refined**: point 1's quarantine is removed; points 2 to 4 (the pump never throws, only misuse throws, generated result codes) stay. Its causes are gone: a refused delivery, a queue that could not grow, out-of-memory back-off. A runaway's input is abandoned by core |
| 0009, 0014 Native toolkit; memory pooled by lifetime | **refined**: smaller, and back in napi |
| 0015 to 0018 Tracing hook, OTLP, encoder | **refined**: `tinyactors-otlp` drains `tel` |
| 0020 Mail for an unloaded actor waits while a loader brings it back | **refined**: the mail waits in core's pool; the loader answers `c2h.load_needed` |
| 0022 Mail a chart sends nowhere raises `error.communication` | **kept**, now done by recycling the envelope |
| 0025 A quarantined child tells its parent | **superseded**: there is no quarantine to report. A parent waiting for `done.invoke` relies on its own timeout |
| 0035, 0036 Parameters | **refined**: kernel parameters go |
| 0037 napi is a binding over the host kernel | **superseded**: a binding over core's rings |
| 0040 Effects are not held | **kept**: effects may run on receipt for every live session, independently of macrostep ends and journaling |

ADRs 0032 to 0034 (persistence by replay, the input log) were already
superseded by kernel ADR 0006. Their input observations become
`c2h.input`.

## Consequences

- **napi is a binding in the README's sense:** values, a data model, a
  pump and an API, over core.
- **The host never routes mail, never orders inputs for a busy actor,
  never keeps timers for resident actors,** and never holds anything from
  the middle of a macrostep.
- **Files that shrink or change:**

  | File | Change |
  |---|---|
  | `src/pump.c` (832 lines) | rewritten around the loop; much shorter |
  | `src/napi_system.c` (2,115) | loses everything that wrapped `tah_system_*`; gains ring setup |
  | `src/napi_services.c` (782) | reads `c2h.send`, `c2h.invoke`, `c2h.log` instead of kernel effects |
  | `src/napi_loads.c` (555) | the loader answers `c2h.load_needed`; waiting mail and dead letters go |
  | `src/napi_recorder.c` (167), `src/napi_replay.c` (165) | read `c2h.input`, post replay records; most of the code goes |
  | `src/napi_trace.c` (722) | drains `tel` |
  | `src/napi_children.c`, `src/import.c` | children are local sessions |
  | `lib/system.js`, `lib/loader.js`, `lib/tracing.js`, `lib/params.js` | follow the native changes |

  The kernel's allocator and containers (about 650 lines) come back to
  napi; what stays unused is deleted. The kernel's pump timings,
  definition churn and cursor paging (`host_metrics.c`, `definitions.c`,
  `enumerate.c`) move to napi.
- **Suites to rewrite:** `route.js`, `loads.js`, `session-loads.js`,
  `recorder.js`, `replay-events.js`, `persist.js`, `session-inputs.js`,
  `scale.js` and `allocations.js`. The zero-allocation property moves
  from "after warm-up" to "always, in a macrostep".
- **Breaking API changes,** all inside this repository (AGENTS.md: change
  in place): those of section 4.
- **Effects are at most as late as a drain.** A record is executable on
  receipt, including before its macrostep ends.

## What this ADR does not decide

- **Core's side of the protocol:** the rings, the pool, slots, the chain
  limit, names and capacity errors. Those are core ADRs
  ([migration.md, "ADRs to write"](../../../docs/proposals/ring-proto/reference/migration.md#adrs-to-write)).
- **The order of work.** [IMPLEMENTATION.md](../../../docs/proposals/ring-proto/IMPLEMENTATION.md)
  is the plan.
- **Ring sizes and defaults.** h2c and tel default to 16 KiB; c2h is
  derived. Step 4.2 measures them against napi's suites.
- **The size of napi's bounded result buffer and submission table,** and
  its drop policy.
- **Journaling.** `c2h.input` is an observation; when and where an
  application journals it is the application's policy.
- **Debugging.** Attaching a debugger suspends the pump and takes the
  whole system ([debugging.md](../../../docs/proposals/ring-proto/reference/debugging.md)).
- **The MCP server.** [mcp-server.md](../../../docs/proposals/mcp-server.md)
  is an experimental proposal that napi would bind. It needs revising
  against the ring protocol and is not part of this ADR.
