# ADR 0027: A debugger holds, steps and redirects one actor

**Status:** Accepted and implemented (2026-09-26). Applies tinyactors-core ADRs 0082 to 0087 to the addon; extends ADR 0005 (scheduling states) and ADR 0008 (faults); builds on ADR 0020 (the loader's waiting mail) and core ADR 0075 (runaway quarantine), see "Beside the loader and the runaway limit".
**Date:** 2026-09-26

## Context

The debug adapter proposal (`../../../docs/proposals/debug-adapter.md`)
attaches a Debug Adapter Protocol session to one actor of a running
production system, in one of three modes: watch (read only), fork (a copy
in a system of its own) and live (the actor itself). Its status page is
`../../../docs/debug-adapter.md`, rows N1 to N3.

Core already had what a debugger needs from a chart: pausing at unit
budgets, the paused position in inspection (core ADR 0083), operator
transitions and abandoning a macrostep (0082), withdrawing one delayed
send (0084), images across systems (0085) and source spans (0086). The
addon's scheduler (ADR 0005) had not:

- An actor could not be kept from taking its mail. Quarantine (ADR 0008)
  does that for a fault, but it is a verdict: it reports a fault, and
  `release()` is the only way out.
- The turn's budgets (ADR 0007) were the system's. Stepping one unit means
  running one actor with `action_budget: 1` while every other actor keeps
  the system's budgets.
- An operator transition goes through `ta_actor_deliver()` like an event,
  so it must wait behind the actor's queued mail in the envelope, not jump
  the queue from JavaScript.
- `ta_actor_abandon()` ends a macrostep the addon was running: a `send()`
  waiting for it would never settle, and the runnable queue would still
  hold an entry for an actor that is now between macrosteps.

## Decision

### The scheduler (N1, N2, N3)

1. **A `held` scheduling state (N1).** `tna_sched_hold()` sets the
   `TNA_SCHED_HOLD` flag. An idle or ready actor becomes `HELD` at once;
   one inside a macrostep, or quarantined, becomes `HELD` when its
   macrostep ends, so a live hold never stops an actor in the middle of a
   macrostep and a `send()` waiting for that macrostep still completes. A
   held actor is on no queue; its mail waits in its mailbox, so a `send()`
   with a timeout to a held actor is withdrawn at the timeout rather than
   taken later (the webhook case). `tna_sched_unhold()` puts it back on
   the ready queue, or the runnable one when core gave it a macrostep
   while held (`TNA_SCHED_UNFINISHED`, set by `tna_sched_wake()`).
2. **A fault while held still quarantines and reports.** A held actor runs
   only when stepped; if that macrostep faults, the actor is quarantined
   and the fault reaches `onFault` as any other (ADR 0008). The hold flag
   stays, so after `release()` the actor finishes its macrostep and is
   held again at its end.
3. **Delivering exactly one held message.** `tna_sched_step()` runs one
   macrostep of a `HELD` actor: for the head of its mailbox
   (`TNA_STEP_MESSAGE`), for a macrostep it has to finish
   (`TNA_STEP_MACROSTEP`), or, with an empty mailbox, for the next message
   that arrives (`TNA_STEP_WAITING`). The actor is `HELD` again when that
   macrostep ends.
4. **Held anywhere, for a fork's copy only.** `anywhere` makes a hold take
   effect inside a macrostep too: at once for a runnable actor, at every
   `TA_ACTOR_PAUSED`, and right after each delivery, before the actor
   takes the event. This is what stepping one action on a fork needs. The
   live adapter never passes it: a live actor stops only between
   macrosteps.
5. **The pump reports holds.** When a turn leaves actors held, the pump
   hands `TNA_RECORD_HELD` records to the effects handler (`held`) after
   the turn's trace records, so an observer sees the macrostep's records
   first and the stop second.
6. **Budgets for one actor (N2).** `tna_sched_set_budgets()` stores
   `{ microsteps, actions }` for one actor in a map on the scheduler and
   sets `TNA_SCHED_OWN_BUDGETS`; the resume reads the map only when the
   flag is set. Clearing them removes the entry and the flag. With
   `microstep_start` (a third flag, `TNA_SCHED_MICROSTEP_START`, so the map
   entry stays one word) the resume passes core's
   `pause_at_microstep_start` (core ADR 0087): every microstep it begins
   pauses at its start, after selection and before its first unit. Every
   other actor, and the pump, keep passing false.
7. **Operator transitions in the envelope (N3).** `post()` and `send()`
   take a `transition: { targets, source }`. The envelope carries a
   `tna_jump` (the definition ID the state IDs belong to, the source, the
   targets) in one allocation, in a union with the dead-letter address,
   which only named targets use; `has_jump` says which it is. The message
   queues behind the actor's mail and reaches `ta_actor_deliver()` with
   its `transition` set. A jump for another definition than the actor now
   runs (it was replaced meanwhile), or one core refuses with
   `TA_ERR_INVALID`, becomes a dead letter `refused`; it does not
   quarantine the actor, since the operator's jump was wrong, not the
   chart.
8. **Abandon settles the addon's state (N3).** `ta_actor_abandon` is a
   hooked binding (ADR 0013); its hook calls `tna_sched_abandoned()`. The
   `send()` whose macrostep was abandoned settles with
   `TNA_SETTLE_ABANDONED`, which rejects in JavaScript with code
   `abandoned`. A runnable actor stands between macrosteps again (ready,
   idle, or held), and its runnable-queue entry is skipped when popped.
9. **`ta_actor_withdraw_send` is exposed** as `system.withdrawSend(actor,
   sendID)`; it needs no scheduler state, since a delayed send lives in
   core's timer queue.

### The facade

10. `System` gains `hold(actor, { anywhere })`, `unhold`, `step`,
    `setBudgets`, `mailbox`, `abandon` and `withdrawSend`; `inspect()`
    reports the paused position, step, current event, internal queue,
    invocations and definition ID; scheduling reports `held`. The
    session lists definitions with `definitions()`, core ADR 0078's walk,
    which the addon already had. A module-level `observe()` in
    `lib/system.js` (not exported from the package) lets `lib/` code read
    every turn's traces, `<log>` records and holds beside the
    application's own sinks; when the application has no trace sink it
    installs one that asks for values.

### The debug session

11. **`lib/debug.js` is host-neutral.** `createDebugSession(system,
    { mode, allow, audit, send, holdTimeout })` returns a session that
    takes decoded DAP messages through `receive()` and sends through
    `send()`. It owns no transport and no authentication: the application
    puts it behind its own and decides each user's highest `mode` and the
    `allow`ed operations (`send`, `set`, `goto`, `hold`, `evaluate`). An
    attach may ask for less, never more; a fork allows every operation on
    its copy but `evaluate`, which it needs granted. Every attach and
    detach, console command, evaluation and mutation goes to `audit`
    before it happens, and a hook that throws refuses it.
    `DAPReader` and `encodeDAPMessage` do the base protocol's framing.
12. **`evaluate` is its own permission.** An expression runs in the
    actor's data model, which in this addon is arbitrary JavaScript in the
    application's process. Without `evaluate`, the Variables pane, the
    console commands and `setVariable` still work; watch, hover and
    console expressions and breakpoint conditions are refused, and a
    logpoint's `{expression}` parts are left as written.
13. **Live stops are between macrosteps.** A live session holds the actor
    (point 1), stops when the current macrostep ends, and steps one
    macrostep at a time (`next`, `stepIn` and `stepOut` alike). A hold
    ends by itself after `holdTimeout` (default 30 s), so a forgotten
    session cannot stall production; the release is audited with reason
    `hold timeout`. Breakpoints inside a macrostep act as logpoints live.
14. **A fork is a system of its own (core ADR 0085, ADR 0026).** The
    debug system has a manual clock that starts at the origin's time, so
    `clock +<duration>` runs the copy's future without moving
    production's. The actor's revision is registered there from its
    export, spans included. The copy binds a `structuredClone` of the
    live data root. Every I/O processor and invoker type the chart names
    literally gets a stand-in that only records; a send to another actor
    of the origin fails as to a destroyed one; active invocations become
    unavailable. The session says each of these in the console when it
    forks. The copy is held anywhere with budgets
    `{ resumeBudget: 1, actionBudget: 1, microstepStart: true }`, so it
    steps one action, and it stops before a microstep's first unit as
    before any other: a breakpoint on that unit's line stops before it
    runs, as the test host's adapter does; an image is
    captured at every stop, and `stepBack`, `reverseContinue`,
    `restartFrame` and `restart` restore one.
15. **Source lines come from spans (core ADR 0086).** Stack frames,
    `setBreakpoints`, `breakpointLocations`, `gotoTargets` and
    `loadedSources` use `Definition.sourceOf()`. A span whose source is a
    path, or a `file:` URL, is served as `path`, and the client reads
    the file: statechart() records its builder calls' files, and
    `define(text, { format: 'scxml', name })` names an SCXML document
    (ADR 0019). A chart whose spans name nothing a client can open, an
    SCXML document defined without a name, a child chart inline in its
    parent's `<content>` (its lines count from the start of its content,
    so it is left unnamed rather than given its parent's file and the
    wrong lines), or a child fetched from a URI of another scheme, is a
    virtual source, as in the test host's adapter: `encodeDefinition()`
    writes the definition as SCXML with core's encode-side source map
    (`ta_definition_encode` with `spans`), the source gets a
    `sourceReference` and the name `<family>.scxml`, the `source` request
    answers that text as `application/scxml+xml`, and frames,
    breakpoints, `breakpointLocations` and `gotoTargets` use its lines.
    The session keeps the encoded text while it is attached, not the
    document the chart was defined from, so the text shows the chart as
    core holds it: one element per line, comments and layout gone. A
    chart SCXML cannot express (a builder chart whose expressions are
    functions) and that keeps no spans still works; frames then have no
    line and line breakpoints stay unverified.
16. **A development transport, not a product.** `tools/dap-server.js`
    (`mise run dap-server`) serves sessions over stdio or `--port` for an
    example system. It grants every permission and is not in the package.

## Beside the loader and the runaway limit

The addon's scheduling grew two things this ADR had to fit beside:

- **Holding is not flow control.** ADR 0020 records the user's decision
  (decision 4, option D) that hosts get no hold or release state for
  resident actors: a host pauses an actor by unloading it, and a loader
  answers "not now". The hold here is a debugger's, on one actor an
  operator attached to, for as long as a session lasts, and it ends by
  itself after `holdTimeout`. It is not a way to stop intake: mail to a
  held actor keeps collecting in its mailbox, counted against
  `maxQueuedMessages`, and a `send()` with a timeout is withdrawn at the
  timeout (point 1).
- **A held actor is resident**, so no loader is involved: `unload()`
  refuses it as `busy` (its scheduling state is `held`, not `idle`), and
  mail that waited for a load reaches its mailbox when the load finishes,
  as for any resident actor. An envelope carries either an operator
  transition or an address, never both: a jump is only ever posted to an
  actor ID (point 7), while an address belongs to mail that waits for a
  load or is a dead letter to a name.
- **A runaway** (core ADR 0075) is quarantined with fault code `runaway`,
  and ADR 0075's `release()` lifts the microstep limit for the rest of that
  macrostep (the entry's `UNLIMITED` flag). A debugger has a third choice:
  abandon the runaway macrostep (core ADR 0082), which also clears
  `UNLIMITED`, jump out of the loop, and `release()`, after which the next
  macrostep is limited again (`test/runaway.js`). The pump hands a
  runaway's fault record to the effects handler (`runaway`) after the
  turn's traces, as it does holds, and the debug session reports it as
  `important` output and, with the `runaway` exception filter, as an
  exception stop (`test/debug.js`).
- **Flags.** The scheduling entry's 8 flag bits hold `BACKOFF`,
  `FINISHED`, ADR 0075's `UNLIMITED`, and this ADR's `HOLD`, `UNFINISHED`,
  `OWN_BUDGETS`, `ANYWHERE` and `MICROSTEP_START`: they are full, so the
  next flag makes the entry grow or takes a bit of `reserved`.

## Costs

Measured on the development machine (x86-64, clang -O2):

| | Before | After |
|---|---|---|
| `tna_sched_entry` | 16 bytes | 16 bytes (the new flags fit in `flags`) |
| envelope (`tna_envelope`) | 104 bytes | 104 bytes (`jump` shares the address's room; `has_jump` sits in padding) |
| `tna_message` | 32 bytes | 40 bytes |
| `tna_sched` | 976 bytes | 1032 bytes (the budgets map) |
| an operator transition | — | 16 bytes + 4 per target, one allocation per jump |

The sizes compare this change with the addon it was rebased onto
(`ec949d4`).

The hot path gains one predictable branch at each of: the resume (the
`OWN_BUDGETS` flag; `ANYWHERE` is tested only on `TA_ACTOR_PAUSED`), the
delivery (`has_jump`, then `ANYWHERE` after it), the settle (the `HOLD`
flag), the post (a `NULL` jump), and the envelope's release (`has_jump`
replaces the address test). The tracing benchmark, 100,000 actors, base
28bca20 and this change run alternately on a shared machine, in ns per
event, untraced:

| Run | Before | After |
|---|---|---|
| 1 | 1115 | 1151 |
| 2 | 1100 | 1082 |
| 3 | 1116 | 1127 |

The traceparent, sampled-out and macrostep columns move the same way.
The difference is within the machine's noise. On that shared machine
(about 1,100 ns an event, against the ~440 ns `bench-check` was written
for) `mise run //tinyactors-napi:bench-check` fails its 40 ns budgets for
the base 28bca20 as well as for this change, by similar margins (sampled
out: 148 to 154 ns over untraced for the base, 89 to 203 ns for this
change, in alternate runs); it is to be run again on a quiet machine.
These runs predate core ADRs 0074 to 0080 and the addon's work on top of
them (the loader, the runaway limit, the chunked free list and OTLP
staging); this change was rebased onto them afterwards, and the runs are
to be repeated with `bench-compare` against that base (napi's roadmap,
"Debugging"). The sizes in the table above are of the rebased code.

## Alternatives

- **Hold by quarantining.** Quarantine already keeps an actor out of
  scheduling, but it reports a fault, counts in the fault metrics and is
  left by `release()`. Mixing the two would make a debugger's stop look
  like a failure in production dashboards.
- **Hold anywhere on a live actor.** Stopping inside a macrostep keeps a
  `send()` waiting and holds whatever the actor's caller is waiting for.
  The proposal chose between-macrostep stops live; the anywhere hold is
  only for a copy nobody waits on.
- **Budgets per turn instead of per actor.** Setting the turn's budgets to
  one action would slow every other actor in the system to a crawl while
  a debugger steps.
- **Jumps from JavaScript straight to `ta_actor_deliver()`.** That would
  let an operator transition overtake the actor's queued mail and run
  while the actor is inside a macrostep, which core refuses.
- **A transport in the package.** Authentication and who may do what are
  the application's; a built-in server would have to guess both.

## Consequences

- A held actor's mail grows without bound while it is held; the hold
  timeout bounds how long.
- `tna_message` grew by 8 bytes; each queued message is 8 bytes larger.
- A dead letter `refused` now also means "the operator's jump was
  refused".
- The debugger's reads (`inspect`, `mailbox`, captures) run on the event
  loop between turns, like any other facade call.

## Testing

- `test/native/debugging_test.c` (under ASan, `mise run
  //tinyactors-napi:test-native-asan`): hold and step, a hold waiting for
  the macrostep, a timed-out `send()` withdrawn while held, a fault while
  held, a wake while held, holding anywhere, budgets for one actor (with
  and without a stop at a microstep's start),
  operator transitions (queued, refused, for another definition), and an
  abandoned macrostep.
- `test/operate.js`: the facade methods.
- `test/debug.js`, on Node and Bun: framing, options and permissions, the
  audit hook, watch, live and fork modes, holding with its timeout,
  breakpoints of every kind, stepping, the console, scopes and variables,
  modules and sources, SCXML documents with and without a name and their
  inline and fetched children, forks with stepBack, reverseContinue,
  restartFrame and restart, and the session's end.
- `test/tracing.js` gained small OTLP requests, for the trace hand-off fix
  below.

## A fix on the way

Bun moves a small `Buffer` (1,000 bytes or fewer) into a new
`ArrayBuffer` when its `ArrayBuffer` is first asked for, so
`napi_trace.c` wrote a small OTLP request into memory the `Buffer` no
longer used. It now writes the request before asking for its
`ArrayBuffer` (ADR 0018 unchanged otherwise).
