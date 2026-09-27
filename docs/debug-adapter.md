# Debug adapter: status of every DAP message

What each Debug Adapter Protocol message does in Tinyactors, what it
depends on, and how far each host has got. The design is
[the debug adapter proposal](proposals/debug-adapter.md); decisions move
into ADRs as they are made, and this page links them.

The decisions have since moved away from the proposal in one place. napi's
adapter attaches to production actors, and a debugger never stops one
and never forks one: it watches and pokes, and the view updates live
([napi ADR 0026](../tinyactors-napi/docs/adr/0026-a-debugger-watches-and-pokes-one-live-actor.md)).
The proposal's watch, fork and live modes, holding a live actor, stepping
it, forks and going back are gone from napi. The test host's adapter
runs a developer's workbench, not production: it keeps stopping,
stepping and breakpoints that stop, and has no forks or going back
either.

The message list is the complete set in the protocol's JSON schema
(`debugAdapterProtocol.json` in microsoft/debug-adapter-protocol): 45
requests, 17 events.

Both hosts play [the shared DAP transcripts](../tinyactors-test/dap/README.md)
(T1): the common core as request, response and event, so the two
adapters stay in step. That README lists where they differ on purpose.

## Legend

**Plan**

- **yes:** implemented, and the matching capability is declared.
- **test:** the test host's adapter only; napi answers an error that says
  why, and does not declare the capability.
- **no:** not offered. The capability is not declared, and the request, if
  a client sends it anyway, answers `success: false` with a message.

**Needs:** what must exist first. "none" means core and the host already
have what the message needs.

| Mark | Needed |
|---|---|
| C1 | Core: the paused position in `ta_actor_inspection` |
| C2 | Core ADR: operator transitions and abandoning a macrostep |
| C3 | Core ADR: withdrawing one delayed send by owner and send ID |
| C4 | Core ADR: an image restores into any system that registers its revision (dropped) |
| C5 | Core ADR: source spans on decode and encode |
| C6 | Core ADR: a resume can stop before a microstep's first unit |
| N1 | napi: a `held` scheduling state and delivering one held message (dropped) |
| N2 | napi: budgets for one actor (dropped) |
| N3 | napi: operator transitions in the envelope; settling on abandon |

**Status** per host (tinyactors-test, napi): ☐ not started, ◐ in progress
or partial, ☑ done and tested, — not offered.

## Requests

### Session

| Request | Plan | What it does | Needs | test | napi |
|---|---|---|---|---|---|
| `initialize` | yes | Answers the capabilities below; sends `initialized` | none | ☑ | ☑ |
| `configurationDone` | yes | Ends configuration; a `launch`ed workbench starts running | none | ☑ | ☑ |
| `launch` | yes | tinyactors-test only: runs a chart directory as a workbench. napi answers an error: production is attached to | none | ☑ | — |
| `attach` | yes | `{ address \| sessionID \| actor, allow }`: one actor, watched live and never stopped; `allow` asks for fewer of the pokes the application granted, none watches only | none | — | ☑ |
| `restart` | no | Both: no longer offered. It made a new fork (napi), or a new copy of a fork's source (test host); forks are gone. Disconnect and attach, or launch, again | — | — | — |
| `disconnect` | yes | Detaches; closes the `debug` scope. Never destroys the actor, whatever `terminateDebuggee` says | none | ☑ | ☑ |
| `terminate` | yes | Same as `disconnect`: the debuggee is never terminated | none | ☑ | ☑ |
| `cancel` | no | Every request answers at once, so there is nothing to cancel | — | — | — |
| `runInTerminal` | no | A reverse request; the adapter never asks the client for a terminal | — | — | — |
| `startDebugging` | no | A reverse request; invoked children are threads of one session, not sessions of their own | — | — | — |

### Breakpoints

| Request | Plan | What it does | Needs | test | napi |
|---|---|---|---|---|---|
| `setBreakpoints` | yes | Lines map to the nearest action, transition or state. Test host: stops before the action, before the transition's content, or on entry or exit. napi: a tracepoint, found in the macrostep's records, that writes where it would have stopped. Conditions, hit conditions and log messages | C5 | ☑ | ☑ |
| `breakpointLocations` | yes | The lines that hold an action, a transition or a state | C5 | ☑ | ☑ |
| `setFunctionBreakpoints` | yes | By name: `past_due` (entry), `exit:past_due`, `on:payment.failed`, `idle → busy` (a transition's ADR 0071 name); tracepoints in napi | none | ☑ | ☑ |
| `setExceptionBreakpoints` | yes | Filters `error.execution`, `error.communication`, `error.platform`, `ignored`, `stopped`, and `runaway` in napi; tracepoints in napi | none | ☑ | ☑ |
| `dataBreakpointInfo` | yes | A data-model location can be watched for writes | none | ☑ | ☑ |
| `setDataBreakpoints` | yes | Stops (test host), or writes a tracepoint's line (napi), after an `ASSIGN` record writes the location | none | ☑ | ☑ |
| `setInstructionBreakpoints` | no | A chart has no instructions | — | — | — |

### Execution

| Request | Plan | What it does | Needs | test | napi |
|---|---|---|---|---|---|
| `continue` | yes | Test host: resumes with the normal budgets. napi: answers success and changes nothing (the actor never stopped), and reports the entry stop again so the client keeps showing it | none | ☑ | ☑ |
| `next` | test | The next action at the same block depth or shallower. On an idle actor, stops before transition selection when it takes its next event. napi: a live actor is never stopped (it held and stepped one before, N1 and N2, both dropped) | none | ☑ | — |
| `stepIn` | test | One unit (`action_budget: 1`): an action, or a `<foreach>` iteration. napi: as `next` | none | ☑ | — |
| `stepOut` | test | To the end of the block, then the microstep, then the macrostep. napi: as `next` | none | ☑ | — |
| `stepBack` | no | Both: no longer offered. It restored images of a fork (C4, dropped); neither adapter keeps a past | — | — | — |
| `reverseContinue` | no | Both: as `stepBack` | — | — | — |
| `restartFrame` | no | Both: as `stepBack` | — | — | — |
| `goto` | yes | An operator transition to the target's state. napi: a poke, queued behind the actor's mail like the console's `goto`; its macrostep shows like any other | C2; napi N3 | ☑ | ☑ |
| `gotoTargets` | yes | The states at a source line; every state when there is no line | C5 for lines | ☑ | ☑ |
| `pause` | test | Test host: stops at the next unit. napi: a live actor is never stopped | none | ☑ | — |
| `stepInTargets` | no | An action has one way in | — | — | — |
| `terminateThreads` | no | Ending a thread would destroy an actor | — | — | — |

### State

| Request | Plan | What it does | Needs | test | napi |
|---|---|---|---|---|---|
| `threads` | yes | The attached actor and its invoked children; every actor in a workbench | none | ☑ | ☑ |
| `stackTrace` | yes | Action, `<foreach>` iterations, block, microstep, macrostep; then a `label` frame and the causal chain | C1 to attach to an actor already paused; C5 for locations | ☑ | ☑ |
| `scopes` | yes | Data model, Event, Configuration, Queues, Timers, Invocations, System | none | ☑ | ☑ |
| `variables` | yes | Expands a scope or a value, with paging | none | ☑ | ☑ |
| `setVariable` | yes | Writes a data-model location; other scopes are read-only | none | ☑ | ☑ |
| `setExpression` | yes | Assigns to a data-model location expression | none | ☑ | ☑ |
| `evaluate` | yes | `repl`: the console commands; `watch`, `hover`, `clipboard`, `variables`: an expression in the data model | none; `goto` C2, `abandon` C2, `cancel` C3 | ☑ | ☑ |
| `completions` | yes | Event names after `send`, state IDs after `goto`, data locations otherwise | none | ☑ | ☑ |
| `exceptionInfo` | test | The fault of the `error.*` event or failed action that stopped the thread. napi: a live actor never stops for an exception | none | ☑ | — |
| `source` | yes | A chart's text: its file, or the encoded chart with the encode-side spans | C5 | ☑ | ☑ |
| `loadedSources` | yes | One source per definition revision | none | ☑ | ☑ |
| `modules` | yes | Definition families and their revisions, retired ones included | none | ☑ | ☑ |
| `locations` | no | No value in a chart names a place in a source | — | — | — |
| `readMemory` | no | A chart has no memory to show | — | — | — |
| `writeMemory` | no | A chart has no memory to show | — | — | — |
| `disassemble` | no | A chart has no instructions | — | — | — |

## Events

| Event | Plan | When | Needs | test | napi |
|---|---|---|---|---|---|
| `initialized` | yes | After `initialize` answers | none | ☑ | ☑ |
| `stopped` | yes | Test host: reasons `breakpoint`, `step`, `pause`, `exception`, `goto`, `data breakpoint`, `function breakpoint`, `entry`. napi: `entry` alone, once after attach and after each `continue`, so the client shows the stack and variables of an actor that is never paused | none | ☑ | ☑ |
| `continued` | yes | When the adapter resumes an actor the client did not ask it to; napi never stops one, so never sends it | none | ☑ | — |
| `exited` | no | Actors do not exit with a code | — | — | — |
| `terminated` | yes | The attached actor finished, was destroyed, or its address was left empty | none | ☑ | ☑ |
| `thread` | yes | `SPAWNED`, `FINISHED` and `DESTROYED` records of the actor and its children | none | ☑ | ☑ |
| `output` | yes | `console`: a summary of each macrostep, and in napi where a breakpoint would have stopped; `stdout`: `<log>` records; `important`: dead letters and dropped sends | none | ☑ | ☑ |
| `breakpoint` | yes | A breakpoint is verified once its definition's spans are known, or moves after a hot replace | C5 | ☑ | ☑ |
| `module` | yes | A definition revision is registered or retired | none | ☑ | ☑ |
| `loadedSource` | yes | A definition revision's source becomes available | none | ☑ | ☑ |
| `process` | yes | After `attach` or `launch`: the host process, `startMethod` `attach` or `launch` | none | ☑ | ☑ |
| `capabilities` | no | Capabilities do not change during a session | — | — | — |
| `progressStart` | no | Nothing the adapter does takes long enough | — | — | — |
| `progressUpdate` | no | As `progressStart` | — | — | — |
| `progressEnd` | no | As `progressStart` | — | — | — |
| `invalidated` | yes | After every macrostep: `threads` and `variables` (test host); `variables` after a focus-keeping `stopped` (napi, the live view) | none | ☑ | ☑ |
| `memory` | no | A chart has no memory to show | — | — | — |

## Capabilities

Declared by both: `supportsConfigurationDoneRequest`,
`supportsFunctionBreakpoints`, `supportsConditionalBreakpoints`,
`supportsHitConditionalBreakpoints`, `supportsEvaluateForHovers`,
`exceptionBreakpointFilters`, `supportsSetVariable`,
`supportsGotoTargetsRequest`, `supportsCompletionsRequest`,
`completionTriggerCharacters`, `supportsModulesRequest`,
`supportsDelayedStackTraceLoading`, `supportsLoadedSourcesRequest`,
`supportsLogPoints`, `supportsSetExpression`, `supportsTerminateRequest`,
`supportsDataBreakpoints`, `supportsBreakpointLocationsRequest`,
`supportsClipboardContext`.

The test host alone: `supportsSteppingGranularity` and
`supportsExceptionInfoRequest`, since only it stops and steps.

`supportTerminateDebuggee` and `supportSuspendDebuggee` are declared false:
detaching never ends or suspends an actor. Every other capability is not
declared, `supportsStepBack`, `supportsRestartFrame` and
`supportsRestartRequest` included: neither adapter forks or keeps a past.

napi declares every breakpoint kind although it never stops: a breakpoint
there is a tracepoint, and the client's breakpoint UI is how a watcher
sets one.

## Console commands

The `repl` context of `evaluate`.

| Command | Does | Allowed in napi | Needs | test | napi |
|---|---|---|---|---|---|
| `send <event> [json]` | Delivers through the host's normal path, with the `debug` origin | with `send` | none | ☑ | ☑ |
| `goto <state…> [from <state>]` | An operator transition | with `goto` | C2 | ☑ | ☑ |
| `abandon` | Abandons a macrostep that never ends | with `goto` | C2 | ☑ | ☑ |
| `cancel <send ID>` | Withdraws one pending delayed send | with `goto` | C3 | ☑ | ☑ |
| `replace <file>` | Hot-replaces the actor with a new revision | with `goto` | none | — | ☑ |
| `clock +<duration>` | Advances the workbench's logical time. napi: no; it moved a fork's own clock (C4, dropped), and on a live system it would move every actor's time | test host only | none | ☑ | — |
| `fork` | Copied the actor into a system of its own. Both: no longer offered: the debugger does not fork (napi ADR 0026) | — | C4 (dropped) | — | — |
| `<expression>` | Evaluated in the data model | with `evaluate` | none | ☑ | ☑ |

## Notes on the tinyactors-test column

The adapter is `tinyactors-test/bin/dap`; see
[its README](../tinyactors-test/ruby/dap/README.md).

- `attach` answers an error in the test host: it launches workbench
  directories, and attaching to a running actor is napi's.
- `stackTrace`: frames have the file and line their element was written
  on (C5) and the top frame is the unit the actor runs next (C1), a
  microstep's start included (C6). After a `caused by` label frame comes
  the causal chain, from the records' `cause`, across actors, back to the
  host or the debug console.
- `source` serves the chart's Ruby file as it was when the revision
  loaded, and, for a chart compiled without spans (one `invoke chart:`
  inlines), the SCXML the codec encodes, with the encode-side spans, by
  `sourceReference`.
- Breakpoints and steps stop at a microstep's start, before its first
  unit (C6).
- It had forks and going back (`fork`, `stepBack`, `reverseContinue`,
  `restartFrame`, `restart`) and dropped them with napi's: they answer an
  error now. Stopping and stepping stay: a workbench runs on a
  developer's machine, not in production.
- `gotoTargets` answers the states at a line (those written there, else
  the state an action or transition there belongs to), or every state
  without a line. `goto` of a resting actor with nothing waiting, while
  the system is stopped, is taken at once and stops with reason `goto`
  when the jump's macrostep ends; otherwise it waits in the actor's
  mailbox, as in napi.
- `terminated` follows `terminate`; a workbench has no single attached
  actor whose end would send it.
- `thread` events come from comparing the actor table after each run, not
  from the lifecycle records, so a reload's replacement keeps its thread.
- `setVariable` and `setExpression` write through capture, unload and load
  with a changed binding (core ADR 0038), since core has no call that
  writes an actor's data root. They refuse invoked children and actors with
  active invocations.
- `replace` is not a console command in the test host: saving the chart
  file replaces its actors.
- `cancel` and the Timers scope name a delayed send by its ID as the
  chart holds it (ADR 0048), as napi does.
- What the console sends an actor, events and jumps, waits in that
  actor's mailbox, and the Scheduler's monitor hands it over when the
  actor rests and the system runs, as napi's mailbox does. `abandon` of a
  stopped macrostep stops again with reason `pause`.
- The shared transcripts that stop, step or wait at a breakpoint are
  this host's only; napi plays the rest and its own `live-view`. Console results,
  summaries, `<log>` output, names and scope labels are napi's words; what
  still differs is listed in [the transcripts'
  README](../tinyactors-test/dap/README.md#where-the-adapters-differ).

## Notes on the napi column

The adapter is `createDebugSession()` in `tinyactors-napi/lib/debug.js`
([ADR 0026](../tinyactors-napi/docs/adr/0026-a-debugger-watches-and-pokes-one-live-actor.md));
`mise run //tinyactors-napi:dap-server` serves it for an example system.
Every ☑ is covered by `tinyactors-napi/test/debug.js`, on Node and Bun.

- **Watch and poke, live.** The session never stops the actor. After
  `attach` it reports one `stopped` event (reason `entry`, a description
  saying the actor is watched live and never paused), since a DAP client
  shows a thread's stack and variables only while it is stopped, and
  from then on, after each macrostep of the actor, writes its summary,
  reports it stopped again with `preserveFocusHint` (the client reads the
  stack without moving the focus) and sends `invalidated` for the
  variables. Never for the stacks: VS Code answers that by replaying the
  entry stop, which took the focus, out of the Debug Console mid-typing.
  Frames keep their IDs across macrosteps. A poke (`send`, `goto`,
  `abandon`, `cancel`, `setVariable`, `setExpression`, the `goto`
  request) makes a transition the watcher sees as a new macrostep, or
  writes the data model at once. `pause`, `next`, `stepIn`, `stepOut` and
  `exceptionInfo` answer an error that says a live actor is never
  stopped; `continue` changes nothing.
- **Modes are gone.** The proposal's watch, fork and live modes were
  built and removed: an attach names the actor and `allow`, the pokes it
  asks for; with none it only watches. Forks, holds with their timeout,
  one actor's own budgets and going back are no longer offered (N1, N2
  and C4 dropped): a debugger never stops or forks a production actor.
- **Breakpoints are tracepoints.** Line, function, data and exception
  breakpoints are found in a macrostep's records; where one would have
  stopped, the console says so (`would have stopped after macrostep 3:
  entered past_due`), and a logpoint writes its message.
- `runaway` is napi's alone: the addon quarantines an actor whose
  macrostep runs its microstep limit (core ADR 0075, 100,000 by default),
  and says so as `important` output; with the filter the console adds a
  tracepoint's line. `abandon` then `goto` moves it out of its loop (core
  ADR 0082). The test host sets no microstep limit (core's default is
  none), so a runaway never stops its actors, and its adapter pauses a
  macrostep that never ends instead. The shared `initialize` transcript
  matches the five filters both declare as a prefix.
- `abandon` waits for the pump to finish a microstep the system's action
  budget paused, then abandons between two microsteps; a system whose
  action budget pauses inside every microstep of a loop answers "try
  again".
- `setDataBreakpoints` matches an `<assign>` to a location written as a
  data-model path; an `<assign>` whose location is an expression that
  computes the path is not matched.
- `stackTrace`, `setBreakpoints`, `breakpointLocations` and
  `gotoTargets` use the lines a chart was written on (C5): statechart()'s
  builder calls, or an SCXML document named by `define(text, { format:
  'scxml', name })`. A chart whose spans name no file (a document defined
  without a name, a child inline in `<content>`, whose lines count from
  its content, or a child fetched from a URI that is not a file) is a
  virtual source, as in the test host: the SCXML the codec encodes, with
  the encode-side spans, by `sourceReference`, and its frames and
  breakpoints are on that text's lines ([ADR
  0026](../tinyactors-napi/docs/adr/0026-a-debugger-watches-and-pokes-one-live-actor.md)
  point 11).
- `source` serves that encoded SCXML by `sourceReference`, and each
  definition's exported spec (JSON) as a loaded source of its own; a file
  the spans name is the client's to read.
- `replace <revision>` takes a revision already registered in the system,
  not a file: the application loads definitions.
- The System scope shows `_sessionid`, `_name`, the time, the definition
  and the scheduling state; `_ioprocessors` is not shown there.
- A macrostep's summary is one `output`, a failure's text names the
  action and the fault's message (`scxml.fault.message` on span events),
  an invoked child is named by its `<invoke id>`, and the Event scope
  uses `_event`'s names (`sendid`), as the test host does.
- A microstep's frame names its transitions only once the microstep has
  ended (`microstep 2.1` while it runs): napi reads spans, and a
  microstep's span ends when the next one begins.

## Prerequisites

| Mark | What | Where | Status |
|---|---|---|---|
| C1 | The paused position in `ta_actor_inspection` | tinyactors-core [ADR 0083](../tinyactors-core/docs/adr/0083-inspection-reports-where-a-paused-actor-stands.md) | ☑ |
| C2 | Operator transitions and abandoning a macrostep | tinyactors-core [ADR 0082](../tinyactors-core/docs/adr/0082-operator-transitions-and-abandoning-a-macrostep.md) | ☑ |
| C3 | Withdrawing one delayed send | tinyactors-core [ADR 0084](../tinyactors-core/docs/adr/0084-an-operator-can-withdraw-one-pending-delayed-send.md) | ☑ |
| C4 | Images restore into another system | dropped: only a debugger's fork needed it, and the debugger does not fork (tinyactors-napi [ADR 0026](../tinyactors-napi/docs/adr/0026-a-debugger-watches-and-pokes-one-live-actor.md)); an image restores only into the system that captured it | ✗ |
| C5 | Source spans on decode and encode | tinyactors-core [ADR 0085](../tinyactors-core/docs/adr/0085-a-spec-says-where-its-elements-were-written-and-an-encode-says-where-it-wrote-them.md), tinyactors-scxml, the Ruby DSL, napi's builder | ☑ |
| C6 | A resume stops before a microstep's first unit | tinyactors-core [ADR 0086](../tinyactors-core/docs/adr/0086-a-resume-can-stop-before-a-microsteps-first-unit.md) | ☑ |
| N1 | `held` scheduling state | dropped: a debugger never stops a production actor (tinyactors-napi [ADR 0026](../tinyactors-napi/docs/adr/0026-a-debugger-watches-and-pokes-one-live-actor.md), which records that it was built and removed) | ✗ |
| N2 | Budgets for one actor | dropped with N1: it stepped a held actor or a fork one action at a time | ✗ |
| N3 | Operator transitions in the envelope; settling on abandon | tinyactors-napi [ADR 0026](../tinyactors-napi/docs/adr/0026-a-debugger-watches-and-pokes-one-live-actor.md) | ☑ |

The prerequisites sit on core ADRs 0074 to 0080 and napi ADRs 0019 to 0024:

- C2 abandons a macrostep that core ADR 0075 stopped as a runaway; napi
  quarantines such an actor, and its adapter stops for it with the
  `runaway` filter.
- C5: registering a chart again is answered by its definition (core ADR
  0077), which then takes the new spans, so both adapters read the lines
  again after a hot reload.
- N1 and N2 are dropped, so nothing here adds the hold or release state
  for resident actors that napi ADR 0020's decision 4 declined.

| Mark | What | Where | Status |
|---|---|---|---|
| T1 | Shared DAP transcript suite | [tinyactors-test/dap](../tinyactors-test/dap/README.md), played by both hosts' tests | ☑ |
| X1 | VS Code extension that registers the debug type | [tinyactors-vscode](../tinyactors-vscode/README.md) | ☑ |
