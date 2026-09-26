# ADR 0027: A debugger watches and pokes one live actor

**Status:** Accepted and implemented (2026-09-26). Applies tinyactors-core ADRs 0082 to 0084 and 0086 to the addon; extends ADR 0005 (scheduling states) and ADR 0008 (faults); sits beside ADR 0020 (the loader's waiting mail) and core ADR 0075 (runaway quarantine), see "Beside the loader and the runaway limit".
**Date:** 2026-09-26

## Context

The debug adapter proposal (`../../../docs/proposals/debug-adapter.md`)
attaches a Debug Adapter Protocol session to one actor of a running
production system. Its status page is `../../../docs/debug-adapter.md`,
rows N1 to N3.

Core has what a debugger needs from a chart: the paused position in
inspection (core ADR 0083), operator transitions and abandoning a
macrostep (0082), withdrawing one delayed send (0084) and source spans
(0086). The addon's scheduler (ADR 0005) did not carry the rest:

- An operator transition goes through `ta_actor_deliver()` like an event,
  so it must wait behind the actor's queued mail in the envelope, not jump
  the queue from JavaScript.
- `ta_actor_abandon()` ends a macrostep the addon was running: a `send()`
  waiting for it would never settle, and the runnable queue would still
  hold an entry for an actor that is now between macrosteps.

The user decided what a debugger may do to a production actor: it watches
and pokes, and the view updates live. A poke (send, goto, abandon, cancel,
set) creates a transition, which yields a new macrostep the watcher sees.
No production actor is ever stopped by a debugger, and the debugger does
not fork one (see Alternatives).

## Decision

### The scheduler (N3)

1. **Operator transitions in the envelope.** `post()` and `send()` take a
   `transition: { targets, source }`. The envelope carries a `tna_jump`
   (the definition ID the state IDs belong to, the source, the targets) in
   one allocation, in a union with the dead-letter address, which only
   named targets use; `has_jump` says which it is. The message queues
   behind the actor's mail and reaches `ta_actor_deliver()` with its
   `transition` set. A jump for another definition than the actor now runs
   (it was replaced meanwhile), or one core refuses with `TA_ERR_INVALID`,
   becomes a dead letter `refused`; it does not quarantine the actor, since
   the operator's jump was wrong, not the chart.
2. **Abandon settles the addon's state.** `ta_actor_abandon` is a hooked
   binding (ADR 0013); its hook calls `tna_sched_abandoned()`. The `send()`
   whose macrostep was abandoned settles with `TNA_SETTLE_ABANDONED`, which
   rejects in JavaScript with code `abandoned`. A runnable actor stands
   between macrosteps again (ready or idle), and its runnable-queue entry is
   skipped when popped. Core abandons only between microsteps: an actor the
   system's action budget paused inside one finishes it in the next turns,
   as it would anyway, and the session abandons it as soon as it stands
   between two.
3. **`ta_actor_withdraw_send` is exposed** as `system.withdrawSend(actor,
   sendID)`; it needs no scheduler state, since a delayed send lives in
   core's timer queue.
4. **The pump hands a runaway to the effects handler** (`runaway`, with
   the actor) after the turn's trace records, so a debugger has read the
   runaway macrostep's spans when it hears of it.

### The facade

5. `System` gains `mailbox`, `abandon` and `withdrawSend`; `inspect()`
   reports the paused position, step, current event, internal queue,
   invocations and definition ID. The session lists definitions with
   `definitions()`, core ADR 0078's walk, which the addon already had. A
   module-level `observe()` in `lib/system.js` (not exported from the
   package) lets `lib/` code read every turn's traces, `<log>` records,
   runaways and definitions beside the application's own sinks; when the
   application has no trace sink it installs one that asks for values.

### The debug session

6. **`lib/debug.js` is host-neutral.** `createDebugSession(system,
   { allow, audit, send })` returns a session that takes decoded DAP
   messages through `receive()` and sends through `send()`. It owns no
   transport and no authentication: the application puts it behind its own
   and decides the `allow`ed pokes of each user (`send`, `set`, `goto`
   covering goto, abandon, cancel and replace, and `evaluate`). An attach
   names one actor (`address`, `sessionID` or `actor`) and may ask for
   fewer permissions, never more; with none it only watches. Every attach
   and detach, console command, evaluation and mutation goes to `audit`
   before it happens, and a hook that throws refuses it. `DAPReader` and
   `encodeDAPMessage` do the base protocol's framing.
7. **`evaluate` is its own permission.** An expression runs in the
   actor's data model, which in this addon is arbitrary JavaScript in the
   application's process. Without `evaluate`, the Variables pane, the
   console commands and `setVariable` still work; watch, hover and
   console expressions and breakpoint conditions are refused, and a
   logpoint's `{expression}` parts are left as written.
8. **The view is live; nothing is held.** A DAP client shows a thread's
   stack and variables only while the thread is stopped. After attach the
   session reports the actor with one `stopped` event (reason `entry`, a
   description saying it is watched live and never paused), and from then
   on, after each macrostep of the actor, a poke's included, it writes the
   macrostep's summary as output and sends `invalidated` for stacks and
   variables, so the client reads them again. `continue` answers success,
   changes nothing, and reports the entry stop again, so a client that
   thought the thread ran shows it once more. `pause`, `next`, `stepIn`
   and `stepOut` answer an error that says a live actor is never stopped;
   `stepBack`, `reverseContinue`, `restartFrame`, `restart` and
   `exceptionInfo` are refused too, and their capabilities are not
   declared.
9. **Breakpoints are tracepoints.** Line, function, data and exception
   breakpoints are found in the records of a macrostep that ended. Where
   one would have stopped, the session writes a line saying so; a
   logpoint writes its message. Conditions and hit conditions decide
   whether it writes. The `runaway` exception filter adds such a line to
   the notice every runaway gets (point 4).
10. **`goto` is a poke.** The DAP `gotoTargets` and `goto` requests stay
    declared: a `goto` queues an operator transition, as the console's
    `goto` does, and its macrostep shows like any other. Offering it lets
    a client's "jump to cursor" move a chart to a state without a console
    line; it never stops the actor.
11. **Source lines come from spans (core ADR 0086).** Stack frames,
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
12. **A development transport, not a product.** `tools/dap-server.js`
    (`mise run dap-server`) serves sessions over stdio or `--port` for an
    example system. It grants every permission (`--watch` grants none)
    and is not in the package.

## Beside the loader and the runaway limit

- **No tension with ADR 0020.** ADR 0020 records the user's decision
  (decision 4, option D) that hosts get no hold or release state for
  resident actors: a host pauses an actor by unloading it, and a loader
  answers "not now". This ADR adds no such state. A debugged actor keeps
  its scheduling states and takes its mail as any other; a poke is mail
  like any other, counted against `maxQueuedMessages`, and an envelope
  carries either an operator transition or an address, never both: a jump
  is only ever posted to an actor ID (point 1), while an address belongs
  to mail that waits for a load or is a dead letter to a name.
- **A runaway** (core ADR 0075) is quarantined with fault code `runaway`,
  and ADR 0075's `release()` lifts the microstep limit for the rest of that
  macrostep (the entry's `UNLIMITED` flag). A debugger has a third choice:
  abandon the runaway macrostep (core ADR 0082), which also clears
  `UNLIMITED`, jump out of the loop, and `release()`, after which the next
  macrostep is limited again (`test/runaway.js`). The session reports a
  runaway as `important` output (point 4).
- **Flags.** The scheduling entry's 8 flag bits hold `BACKOFF`,
  `FINISHED` and ADR 0075's `UNLIMITED`; this ADR takes none, and five
  are free.

## Costs

Measured on the development machine (x86-64, clang -O2), against
tinyactors-napi on main:

| | Main | This ADR |
|---|---|---|
| `tna_sched_entry` | 16 bytes | 16 bytes |
| envelope (`tna_envelope`) | 104 bytes | 104 bytes (`jump` shares the address's room; `has_jump` sits in padding) |
| `tna_message` | 32 bytes | 40 bytes (the `jump` pointer) |
| `tna_sched` | 976 bytes | 976 bytes |
| an operator transition | — | 16 bytes + 4 per target, one allocation per jump |

The hot path gains one predictable branch at each of: the delivery
(`has_jump`), the post (a `NULL` jump), and the envelope's release
(`has_jump` replaces the address test). The throughput of this change is
to be measured with `bench-compare` against main on a quiet machine
(napi's roadmap, "Debugging").

A system whose action budget pauses an actor inside every microstep of a
loop (an `actionBudget` smaller than the loop's shortest microstep) gives
`abandon` no moment between microsteps: the session answers "try again".
With the default budgets a resume pauses between microsteps.

## Alternatives

- **Hold the actor, step it, and fork it (built, then removed).** The
  first version of this ADR held a live actor between macrosteps
  (`hold`, `unhold`, `step`, a `held` scheduling state, a hold timeout),
  gave one actor budgets of its own to step one action at a time and to
  stop at a microstep's start (core ADR 0087), and forked the actor into
  a debug system of its own (core ADR 0085, napi ADR 0026) with its own
  clock, where it could be stopped anywhere, stepped, and stepped back to
  images kept at its stops. It worked, and cost four scheduling flags, a
  budgets map in `tna_sched` (56 bytes) and a record kind. The user
  decided against it: a debugger never stops a production actor, and it
  does not fork one. A held actor is a hold state for a resident actor,
  which ADR 0020 decision 4 had declined for hosts; a fork brought a
  second system, box copying between systems and images across systems
  into core, for a view a developer gets from the test host's workbench.
  Everything that existed only for holds and forks was removed with it.
- **Hold by quarantining.** Quarantine already keeps an actor out of
  scheduling, but it reports a fault, counts in the fault metrics and is
  left by `release()`. It is a verdict, not a debugger's stop.
- **Jumps from JavaScript straight to `ta_actor_deliver()`.** That would
  let an operator transition overtake the actor's queued mail and run
  while the actor is inside a macrostep, which core refuses.
- **Declare no capability for what cannot stop.** Leaving breakpoints
  undeclared would hide the tracepoints a watcher wants; declaring them
  and saying in the console where each would have stopped keeps the
  client's breakpoint UI useful without stopping anything.
- **A transport in the package.** Authentication and who may do what are
  the application's; a built-in server would have to guess both.

## Consequences

- `tna_message` grew by 8 bytes; each accepted message is 8 bytes larger
  while it is accepted, not while it waits (the envelope did not grow).
- A dead letter `refused` now also means "the operator's jump was
  refused".
- The debugger's reads (`inspect`, `mailbox`, the scopes) run on the event
  loop between turns, like any other facade call; a view is of the actor
  as it stood then, and the next macrostep replaces it.
- A developer who wants to stop and step a chart does it in the test
  host's workbench, whose adapter keeps stepping and breakpoints that stop
  on its own actors.

## Testing

- `test/native/debugging_test.c` (under ASan, `mise run
  //tinyactors-napi:test-native-asan`): operator transitions (queued,
  refused, for another definition, withdrawn, left waiting at close) and
  an abandoned macrostep (refused inside a microstep, settled between
  two).
- `test/operate.js`: the mailbox, jumps, abandon, withdrawSend and what
  inspect() reports.
- `test/debug.js`, on Node and Bun: framing, options and permissions, the
  audit hook, the entry stop and the live view, pokes from the console
  and the goto request, the requests refused because nothing stops,
  breakpoints of every kind as tracepoints, scopes and variables, modules
  and sources, SCXML documents with and without a name and their inline
  and fetched children, a runaway, and the session's end.
- `test/dap-transcripts.js` plays the shared transcripts that do not
  stop (`../../../tinyactors-test/dap/`), and `live-view`, which is napi's.
- `test/tracing.js` gained small OTLP requests, for the trace hand-off fix
  below.

## A fix on the way

Bun moves a small `Buffer` (1,000 bytes or fewer) into a new
`ArrayBuffer` when its `ArrayBuffer` is first asked for, so
`napi_trace.c` wrote a small OTLP request into memory the `Buffer` no
longer used. It now writes the request before asking for its
`ArrayBuffer` (ADR 0018 unchanged otherwise).
