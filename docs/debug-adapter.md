# Debug adapter: status of every DAP message

What each Debug Adapter Protocol message does in Tinyactors, what it
depends on, and how far each host has got. The design is
[the debug adapter proposal](proposals/debug-adapter.md); decisions move
into ADRs as they are made, and this page links them.

The message list is the complete set in the protocol's JSON schema
(`debugAdapterProtocol.json` in microsoft/debug-adapter-protocol): 45
requests, 17 events.

Both hosts play [the shared DAP transcripts](../tinyactors-test/dap/README.md)
(T1): the common core as request, response and event, so the two
adapters stay in step. That README lists where they differ on purpose.

## Legend

**Plan**

- **yes:** implemented, and the matching capability is declared.
- **fork:** only on a fork; on a live actor the request answers an error
  that says so.
- **no:** not offered. The capability is not declared, and the request, if
  a client sends it anyway, answers `success: false` with a message.

**Needs:** what must exist first. "none" means core and the host already
have what the message needs.

| Mark | Needed |
|---|---|
| C1 | Core: the paused position in `ta_actor_inspection` |
| C2 | Core ADR: operator transitions and abandoning a macrostep |
| C3 | Core ADR: withdrawing one delayed send by owner and send ID |
| C4 | Core ADR: an image restores into any system that registers its revision |
| C5 | Core ADR: source spans on decode and encode |
| C6 | Core ADR: a resume can stop before a microstep's first unit |
| N1 | napi: a `held` scheduling state and delivering one held message |
| N2 | napi: budgets for one actor |
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
| `attach` | yes | `{ address, mode, allow }`: one actor, in watch, fork or live mode; the host checks `allow` | none; fork needs C4 | — | ☑ |
| `restart` | yes | Fork: makes a new copy from the live actor. Live and watch: reattach | C4 | ☑ | ☑ |
| `disconnect` | yes | Detaches; closes the `debug` scope; releases a hold. Never destroys the actor, whatever `terminateDebuggee` says | none | ☑ | ☑ |
| `terminate` | yes | Same as `disconnect`: the debuggee is never terminated | none | ☑ | ☑ |
| `cancel` | no | Every request answers at once, so there is nothing to cancel | — | — | — |
| `runInTerminal` | no | A reverse request; the adapter never asks the client for a terminal | — | — | — |
| `startDebugging` | no | A reverse request; invoked children are threads of one session, not sessions of their own | — | — | — |

### Breakpoints

| Request | Plan | What it does | Needs | test | napi |
|---|---|---|---|---|---|
| `setBreakpoints` | yes | Lines map to the nearest action, transition or state; stops before the action, before the transition's content, or on entry or exit. Live: only stops between macrosteps; inside a macrostep they act as logpoints. Conditions, hit conditions and log messages | C5 | ☑ | ☑ |
| `breakpointLocations` | yes | The lines that hold an action, a transition or a state | C5 | ☑ | ☑ |
| `setFunctionBreakpoints` | yes | By name: `past_due` (entry), `exit:past_due`, `on:payment.failed`, `idle → busy` (a transition's ADR 0071 name) | none | ☑ | ☑ |
| `setExceptionBreakpoints` | yes | Filters `error.execution`, `error.communication`, `error.platform`, `ignored`, `stopped`, and `dead-letter` and `runaway` in napi | none | ☑ | ◐ |
| `dataBreakpointInfo` | yes | A data-model location can be watched for writes | none | ☑ | ☑ |
| `setDataBreakpoints` | yes | Stops after an `ASSIGN` record writes the location | none | ☑ | ☑ |
| `setInstructionBreakpoints` | no | A chart has no instructions | — | — | — |

### Execution

| Request | Plan | What it does | Needs | test | napi |
|---|---|---|---|---|---|
| `continue` | yes | Resumes with the normal budgets and releases held mail | none; live needs N1 | ☑ | ☑ |
| `next` | yes | The next action at the same block depth or shallower. On an idle actor, takes the next held message and stops before transition selection | fork or test host: none; live: N1, N2 | ☑ | ☑ |
| `stepIn` | yes | One unit (`action_budget: 1`): an action, or a `<foreach>` iteration | as `next` | ☑ | ☑ |
| `stepOut` | yes | To the end of the block, then the microstep, then the macrostep | as `next` | ☑ | ☑ |
| `stepBack` | fork | Restores the image captured at the previous stop | C4 | ☑ | ☑ |
| `reverseContinue` | fork | Restores images back to the previous breakpoint | C4 | ☑ | ☑ |
| `restartFrame` | fork | Restores the image captured when that macrostep began | C4 | ☑ | ☑ |
| `goto` | yes | An operator transition to the target's state | C2; live needs N3 | ☑ | ☑ |
| `gotoTargets` | yes | The states at a source line; every state when there is no line | C5 for lines | ☑ | ☑ |
| `pause` | yes | Fork and test host: stop at the next unit. Live: hold the mailbox, stop when the macrostep ends | live needs N1 | ☑ | ☑ |
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
| `evaluate` | yes | `repl`: the console commands; `watch`, `hover`, `clipboard`, `variables`: an expression in the data model | none; `goto` C2, `abandon` C2, `cancel` C3, `clock` C4 | ☑ | ☑ |
| `completions` | yes | Event names after `send`, state IDs after `goto`, data locations otherwise | none | ☑ | ☑ |
| `exceptionInfo` | yes | The fault of the `error.*` event or failed action that stopped the thread | none | ☑ | ☑ |
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
| `stopped` | yes | Reasons `breakpoint`, `step`, `pause`, `exception`, `goto`, `data breakpoint`, `function breakpoint`, `entry` | none | ☑ | ☑ |
| `continued` | yes | When the adapter resumes an actor the client did not ask it to | none | ☑ | ☑ |
| `exited` | no | Actors do not exit with a code | — | — | — |
| `terminated` | yes | The attached actor finished, was destroyed, or its address was left empty | none | ☑ | ☑ |
| `thread` | yes | `SPAWNED`, `FINISHED` and `DESTROYED` records of the actor and its children | none | ☑ | ☑ |
| `output` | yes | `console`: a summary of each macrostep; `stdout`: `<log>` records; `important`: dead letters and dropped sends | none | ☑ | ☑ |
| `breakpoint` | yes | A breakpoint is verified once its definition's spans are known, or moves after a hot replace | C5 | ☑ | ☑ |
| `module` | yes | A definition revision is registered or retired | none | ☑ | ☑ |
| `loadedSource` | yes | A definition revision's source becomes available | none | ☑ | ☑ |
| `process` | yes | After `attach` or `launch`: the host process, `startMethod` `attach` or `launch` | none | ☑ | ☑ |
| `capabilities` | no | Capabilities do not change during a session | — | — | — |
| `progressStart` | no | Nothing the adapter does takes long enough | — | — | — |
| `progressUpdate` | no | As `progressStart` | — | — | — |
| `progressEnd` | no | As `progressStart` | — | — | — |
| `invalidated` | yes | After every macrostep: `threads` and `variables` | none | ☑ | ☑ |
| `memory` | no | A chart has no memory to show | — | — | — |

## Capabilities

Declared: `supportsConfigurationDoneRequest`, `supportsFunctionBreakpoints`,
`supportsConditionalBreakpoints`, `supportsHitConditionalBreakpoints`,
`supportsEvaluateForHovers`, `exceptionBreakpointFilters`,
`supportsStepBack`, `supportsSetVariable`, `supportsRestartFrame`,
`supportsGotoTargetsRequest`, `supportsCompletionsRequest`,
`completionTriggerCharacters`, `supportsModulesRequest`,
`supportsRestartRequest`, `supportsExceptionInfoRequest`,
`supportsDelayedStackTraceLoading`, `supportsLoadedSourcesRequest`,
`supportsLogPoints`, `supportsSetExpression`, `supportsTerminateRequest`,
`supportsDataBreakpoints`, `supportsBreakpointLocationsRequest`,
`supportsClipboardContext`, `supportsSteppingGranularity`.

`supportTerminateDebuggee` and `supportSuspendDebuggee` are declared false:
detaching never ends or suspends an actor. Every other capability is not
declared.

`supportsStepBack` and `supportsRestartFrame` are declared in every mode;
on a live actor the requests answer an error that says to fork.

## Console commands

The `repl` context of `evaluate`.

| Command | Does | Allowed in | Needs | test | napi |
|---|---|---|---|---|---|
| `send <event> [json]` | Delivers through the host's normal path, with the `debug` origin | fork; live with `send` | none | ☑ | ☑ |
| `goto <state…> [from <state>]` | An operator transition | fork; live with `goto` | C2 | ☑ | ☑ |
| `abandon` | Abandons a macrostep that never ends | fork; live with `goto` | C2 | ☑ | ☑ |
| `cancel <send ID>` | Withdraws one pending delayed send | fork; live with `goto` | C3 | ☑ | ☑ |
| `replace <file>` | Hot-replaces the actor with a new revision | fork; live with `goto` | none | ☐ | ☑ |
| `clock +<duration>` | Advances logical time | fork; the test host's workbench | C4 for forks | ☑ | ☑ |
| `fork` | Copies the live actor and switches to the copy | live | C4 | ☑ | ☑ |
| `<expression>` | Evaluated in the data model | all | none | ☑ | ☑ |

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
- Forks (`fork` in the console) live in a system of their own (C4); the
  adapter keeps an image at each stop of a fork's thread and at the start
  of each of its macrosteps, which `stepBack`, `reverseContinue`,
  `restartFrame` and `restart` restore into a fresh system at the image's
  time. On a thread that is not a fork they answer an error.
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
  writes an actor's data root; a fork is captured, destroyed and
  reconstructed instead. They refuse invoked children and actors with
  active invocations.
- `replace` is not a console command in the test host: saving the chart
  file replaces its actors.
- `cancel` and the Timers scope name a delayed send by its ID as the
  chart holds it (ADR 0048), as napi does.
- What the console sends an actor, events and jumps, waits in that
  actor's mailbox, and the Scheduler's monitor hands it over when the
  actor rests and the system runs, as napi's mailbox does. `abandon` of a
  stopped macrostep stops again with reason `pause`. Console results,
  summaries, `<log>` output, names and scope labels are napi's words; what
  still differs is listed in [the transcripts'
  README](../tinyactors-test/dap/README.md#where-the-adapters-differ).

## Notes on the napi column

The adapter is `createDebugSession()` in `tinyactors-napi/lib/debug.js`
([ADR 0020](../tinyactors-napi/docs/adr/0027-a-debugger-watches-and-pokes-one-live-actor.md));
`mise run //tinyactors-napi:dap-server` serves it for an example system.
Every ☑ is covered by `tinyactors-napi/test/debug.js`, on Node and Bun.

- Live stepping is one macrostep: `next`, `stepIn` and `stepOut` each
  deliver one held message, or finish the macrostep in progress. On a
  fork, stepping is by action as the table says, and a fork stops at each
  microstep's start before its first unit (C6), so a breakpoint on that
  unit stops before it runs.
- `setExceptionBreakpoints` is ◐: every filter but `dead-letter`. A dead
  letter is reported as `important` output, but does not stop.
- `runaway` is napi's alone: the addon quarantines an actor whose
  macrostep runs its microstep limit (core ADR 0075, 100,000 by default),
  says so as `important` output, and with the filter stops with reason
  `exception`; `abandon` then `goto` moves it out of its loop (core ADR
  0082). The test host sets no microstep limit (core's default is none),
  so a runaway never stops its actors, and its adapter pauses a macrostep
  that never ends instead. The shared `initialize` transcript matches the
  five filters both declare as a prefix.
- `setDataBreakpoints` stops after an `<assign>` to a location written as
  a data-model path; an `<assign>` whose location is an expression that
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
  0027](../tinyactors-napi/docs/adr/0027-a-debugger-watches-and-pokes-one-live-actor.md)
  point 15).
- `source` serves that encoded SCXML by `sourceReference`, and each
  definition's exported spec (JSON) as a loaded source of its own; a file
  the spans name is the client's to read.
- `replace <revision>` takes a revision already registered in the system,
  not a file: the application loads definitions.
- The System scope shows `_sessionid`, `_name`, the time, the definition
  and the scheduling state; `_ioprocessors` is not shown there.
- A fork does not mirror the live actor's later events into the copy: it
  is the actor as it was when forked (or restarted).
- A macrostep's summary is one `output`, a failure's text names the
  action and the fault's message (`scxml.fault.message` on span events),
  an invoked child is named by its `<invoke id>`, and the Event scope
  uses `_event`'s names (`sendid`), as the test host does.
- A microstep's frame names its transitions only once the microstep has
  ended (`microstep 2.1` while it runs): napi reads spans, and a
  microstep's span ends when the next one begins. For the same reason an
  exception or a data breakpoint stops a fork after the macrostep, not at
  the next unit.

## Prerequisites

| Mark | What | Where | Status |
|---|---|---|---|
| C1 | The paused position in `ta_actor_inspection` | tinyactors-core [ADR 0083](../tinyactors-core/docs/adr/0083-inspection-reports-where-a-paused-actor-stands.md) | ☑ |
| C2 | Operator transitions and abandoning a macrostep | tinyactors-core [ADR 0082](../tinyactors-core/docs/adr/0082-operator-transitions-and-abandoning-a-macrostep.md) | ☑ |
| C3 | Withdrawing one delayed send | tinyactors-core [ADR 0084](../tinyactors-core/docs/adr/0084-an-operator-can-withdraw-one-pending-delayed-send.md) | ☑ |
| C4 | Images restore into another system | tinyactors-core [ADR 0085](../tinyactors-core/docs/adr/0085-an-image-restores-into-any-system-that-registers-its-revision.md), tinyactors-napi [ADR 0026](../tinyactors-napi/docs/adr/0026-a-system-restoring-another-systems-image-copies-its-boxes.md) | ☑ |
| C5 | Source spans on decode and encode | tinyactors-core [ADR 0086](../tinyactors-core/docs/adr/0086-a-spec-says-where-its-elements-were-written-and-an-encode-says-where-it-wrote-them.md), tinyactors-scxml, the Ruby DSL, napi's builder | ☑ |
| C6 | A resume stops before a microstep's first unit | tinyactors-core [ADR 0087](../tinyactors-core/docs/adr/0087-a-resume-can-stop-before-a-microsteps-first-unit.md) | ☑ |
| N1 | `held` scheduling state | tinyactors-napi [ADR 0027](../tinyactors-napi/docs/adr/0027-a-debugger-watches-and-pokes-one-live-actor.md) | ☑ |
| N2 | Budgets for one actor | tinyactors-napi [ADR 0027](../tinyactors-napi/docs/adr/0027-a-debugger-watches-and-pokes-one-live-actor.md) | ☑ |
| N3 | Operator transitions in the envelope; settling on abandon | tinyactors-napi [ADR 0027](../tinyactors-napi/docs/adr/0027-a-debugger-watches-and-pokes-one-live-actor.md) | ☑ |

The prerequisites sit on core ADRs 0074 to 0080 and napi ADRs 0019 to 0024:

- C2 abandons a macrostep that core ADR 0075 stopped as a runaway; napi
  quarantines such an actor, and its adapter stops for it with the
  `runaway` filter.
- C4 names a captured image's system as core ADR 0074's offloaded bytes
  do, by address and token; offloaded bytes still load only where they
  were written, and napi's `unload()` offloads, so a fork restores from an
  in-memory image.
- C5: registering a chart again is answered by its definition (core ADR
  0077), which then takes the new spans, so both adapters read the lines
  again after a hot reload.
- N1: holding is a debugger's, not the flow control napi ADR 0020's
  decision 4 declined; a held actor is resident, so its loader is not
  involved.
| T1 | Shared DAP transcript suite | [tinyactors-test/dap](../tinyactors-test/dap/README.md), played by both hosts' tests | ☑ |
| X1 | VS Code extension that registers the debug type | [tinyactors-vscode](../tinyactors-vscode/README.md) | ☑ |
