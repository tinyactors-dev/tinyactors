# A debug adapter: attach to an actor, poke it, unwedge it

**Status:** Proposed
**Touches:** tinyactors-core (source spans, operator transitions, images across systems, inspection), tinyactors-scxml, tinyactors-test (a Ruby adapter), tinyactors-napi (a JavaScript adapter and scheduler changes), a new VS Code extension
**Not a plan:** nothing here is decided or scheduled. ADRs and roadmaps win.

## Goal

A debugger for charts, spoken over the Debug Adapter Protocol (DAP), so
that VS Code, nvim-dap, Emacs dape, Zed and Helix work as clients without
a client of our own.

The use case that shapes it is a long-lived, low-traffic actor in
production, such as a Stripe subscription actor that sees a few events a
month. An operator wants to attach to that one actor, look at its
configuration and data, send it messages and see what it does, and, at
3am, move a wedged actor out of the state it is stuck in. The chart that
wedges is never the one someone prepared for it, so none of this may need
the chart's author to have planned for it.

The debugger is interactive, not after the fact. Tracing and the OTLP
export (core ADR 0067, napi ADR 0015) already cover looking back at what
happened.

## Three modes

An attach names one actor and a mode. The server checks what the mode
allows against the connecting user's rights, not against what the client
asks for.

```json
{ "address": "#_actor_subscription/acct_1",
  "mode": "watch" | "fork" | "live",
  "allow": ["send", "set", "goto", "hold"] }
```

- **watch.** Nothing ever stops and nothing is sent. The adapter marks the
  actor watched (napi's `watchTraces({ sessions })`), so every trace kind of
  every macrostep it runs, and of everything those cause, reaches the
  adapter. Breakpoints act as logpoints and report "would have stopped".
  For a low-traffic actor this costs almost nothing. It works today.
- **fork.** The adapter captures the actor and rebuilds a copy with
  `ta_actor_reconstruct` in a system of its own, where it is never routable
  (core ADR 0052). Everything is allowed on the copy. Today an image can be
  restored only into the system that captured it; see "A fork needs its
  own system" below.
- **live.** The attached actor itself. `send`, `set` and `goto` as `allow`
  permits. Stopping the live actor needs `hold`, and stops only between
  macrosteps (see "Holding a live actor" below).

### What a fork is for

- **Running the future now.** `ta_session_image_next_deadline` gives the
  next renewal or dunning timer. Time is logical (core ADR 0012), so the
  copy's clock can jump a month ahead and the renewal can be stepped
  through today.
- **Mirroring the next real event.** When production delivers to the
  attached actor, the adapter delivers a copy to the fork too, where it
  stops at breakpoints.
- **Trying a fix on real state.** Replace the fork with an edited revision
  (`ta_actor_prepare_replacement`, as the workbench does) and see what the
  fix does to this customer's state before it ships.
- **Trying an unwedge first.** Jump the copy, step through what that
  causes, check the data, then do the same on the live actor.
- **Unloaded actors.** A low-traffic actor is often unloaded, and its image
  is already in host storage. A fork can be made from that image without
  loading the production actor.

### What a fork must get right

- **Copy the data model.** `reconstruct` binds the copy to the root the
  image kept, which is the live actor's own root (napi `index.d.ts`,
  `reconstruct`). The fork gets a deep copy (`structuredClone` in napi),
  or stepping the copy changes production data.
- **Stub the effects.** The fork's system has recording stand-ins for its
  I/O processors and invokers, so no step on a copy sends a real request.
- **Say when the copy differs.** Restoring an image turns each active
  invocation into `error.communication.invoke.<id>.unavailable`. A chart
  that uses `<invoke>` takes a different path in the copy, and the adapter
  says so on attach.
- **Register the exact revision.** An image restores only against the
  revision it was captured under (`ta_session_image_definition`).

### A fork needs its own system

Logical time belongs to a system (core ADR 0012): `ta_system_set_time`
moves every actor in it. A fork in the production system would share
production's clock, so `clock +3d` would move time for every actor, and its
sends would go through production's I/O processors. A fork therefore lives
in a debug system of its own, with its own clock, stub I/O processors and
stub invokers, created by the adapter in the same process.

Neither core nor napi can do that today:

- **Core.** An image is only partly independent of the system that
  captured it. It copies addresses (core ADR 0065) and I/O origins (core
  ADR 0068) as text, and `ta_image_localize_io_origin` re-interns an origin
  in the system that restores it (`src/session_image.c`). But events keep
  their `ta_event_name_id`, and delayed sends their event name and their
  I/O type atom, and both are interned per system (`event_names` and
  `atoms` in `src/system.h`). Another system that registers the same
  revision interns names in its own order, so the IDs would name other
  events. Runtime references to *other* actors (an event's origin, a
  parent) name slots of the capturing system.
- **napi.** `handleOf` in `lib/image.js` refuses an image whose `origin` is
  not the system it is restored into.

The core half is one more core ADR: **an image restores into any system
that registers its revision.** Names and I/O types go into the image as
text, as addresses and I/O origins already do, and are interned on
restore (core ADR 0049 makes every interned identity readable back). A
runtime reference to another actor becomes unreachable in another system:
a send to it fails as one to a destroyed actor does, which in a fork is
what should happen. The napi half is dropping the `origin` check, with a
test that restores across systems. Host values need care there too.
napi's value boxes belong to a system (`tna_values` in the system wrapper,
`src/napi_system.h`), though each box points at the `tna_values` it came
from (`src/values.c`). The fork's data root is a fresh `structuredClone`
bound on restore, so what still crosses are the boxes an image retains:
queued event data and delayed-send data. napi's half has to show that a
debug system can hold and release another system's boxes, or copy them
into its own on restore; the test host's values need the same check.

Until then, a fork can live in the capturing system: stepping, `send`,
`goto` and `set` work, but `clock` does not, and its effects must be
intercepted by the host instead of going to separate stubs.

A session image is not a wire format: it holds references to live host
values (core ADR 0038). So the debug adapter for production runs inside
the production process, and the fork lives there too. It opens no socket
of its own (see "Transport, authentication and audit" below).

## Poking: the debug console

Every DAP client has a debug console, which sends each line as an
`evaluate` request with `context: "repl"`. Sending messages and the other
operator commands live there, not in custom requests, which only a VS Code
extension could call.

```text
> send payment.failed {"attempt": 3}
  macrostep 42: active → past_due on payment.failed
    assign .failures = 1
    sent billing.retry to #_actor_billing/acct_1 (delay 3d)
> .failures
  1
> clock +3d
```

| Command | Does | Allowed in |
|---|---|---|
| `send <event> [json]` | Delivers through the same router and mailbox as any mail, under a sampled, watched traceparent | fork; live with `send` |
| `goto <state…> [from <state>]` | An operator transition (below) | fork; live with `goto` |
| `abandon` | Abandons a macrostep that never ends (below) | fork; live with `goto` |
| `replace <file>` | Hot-replaces the actor with a new revision; it restarts from the initial state and keeps its data. napi refuses while the actor is inside a macrostep, so a runaway macrostep needs `abandon` first | fork; live with `goto` |
| `cancel <send ID>` | Withdraws one pending delayed send of this actor (below) | fork; live with `goto` |
| `clock +<duration>` | Advances logical time | fork |
| `fork` | Copies the live actor and switches to the copy | live |
| `<expression>` | Evaluated in the actor's data model | all |

- **Completion.** The `completions` request offers the event names the
  actor's transitions listen for after `send` (the workbench already
  computes them, `tinyactors-test/ruby/workbench/repl.rb`), state IDs after
  `goto`, and data locations otherwise.
- **Replies.** The debugger is a sender the actor can answer. The adapter
  registers a `debug` I/O processor, or uses napi's `openScope()` (core ADR
  0068), so `<send targetexpr="_event.origin">` reaches the console. On
  detach the scope is closed, and events naming it then carry no origin.
- **Queueing.** A message from the debugger queues behind real mail. While
  the actor is stopped, core answers `TA_ERR_BUSY`, so both wait in the
  host's mailbox. The console shows a message as queued until the actor
  takes it.
- **Audit.** A debugger message starts its own trace, tagged as coming
  from the debugger. Everything it causes, including an outgoing Stripe
  request, is in that one trace in the OTLP export.

## Unwedging without foresight

A chart that nobody prepared for 3am has no admin transitions. The
operator needs a jump that works on any chart. Core ADR 0030 rules out a
`goto_state()` that changes the configuration outside the interpreter's
transition semantics. It does not rule out a jump that core *runs as a
transition*: that is what an admin transition would have done, built from
the running actor instead of written into the chart.

### An operator transition

Core builds one transition at run time and takes it as the first
microstep of a macrostep:

- **Source:** a state that is active now, or the root by default. A region
  of a `<parallel>` as the source moves only that region.
- **Targets:** any states of the chart, including history states ("put it
  back where it was") and a top-level `<final>`, which ends the session
  with its done data and its completion.
- **Type:** internal, no guard, no executable content.

The normal engine does the rest: the exit set, `<onexit>`, recording
history, cancelling invocations, the entry set, `<onentry>`, default
initial states, `done.state` events. Eventless transitions and the
internal queue then run to a stable configuration, as after any
transition. The actor cannot end up in a configuration the engine could
not produce.

One shape, following the rule that APIs change in place: an optional
member of `ta_actor_deliver`'s parameters.

```c
/* When present, the event is taken by this transition instead of by
 * transition selection: no chart transition sees it. */
typedef struct {
    ta_state_id source;   /* TA_STATE_NONE: the root */
    TA_COUNTED_BY(target_count) const ta_state_id *targets;
    uint32_t target_count;
} ta_operator_transition;
```

Riding on delivery reuses admission (a busy actor answers
`TA_ERR_BUSY`), the trace parent, the budgets, and `_event`: handlers see
the operator's event, for example `ta.operator.goto` with its data and
origin, so an `<onentry>` can tell a jump from a normal entry. The trace
has a `MICROSTEP` record with `transition` `TA_TRANSITION_NONE` and a flag
saying it was an operator's.

### Abandoning a macrostep

A macrostep that never becomes stable (eventless transitions cycling
forever) answers `TA_ERR_BUSY` to everything, a jump included. The second
operation abandons it: the continuation and the internal queue are
discarded, and the actor keeps the configuration after its last completed
microstep, which core ADR 0061 section 2 already makes a legal one.
Effects already released stay released. A trace record lists the internal
events that were dropped. A jump then works as usual.

On a live actor this is the one case where the host takes an actor out of
scheduling inside a macrostep: the macrostep never ends, so no waiting
caller would ever have seen it finish.

### The other tools

- **Setting data.** `setVariable` fixes data before a jump, so `<onentry>`
  sees the corrected values.
- **Replacement.** Hot-replacing with a new revision also needs no
  foresight, but the actor restarts from the initial state and loses its
  delayed sends. The adapter lists what would be dropped before it
  replaces. A jump keeps everything except the part it moves.

The chart cannot refuse a jump: that is the point. An author's own
assumptions, such as data matching the state, can break. That is why the
adapter offers a fork first and records who jumped where.

## Source spans in the codec interfaces

Line breakpoints, frame locations, `gotoTargets` and "Jump to Cursor" need
a map from compiled IDs to places in a source, and back. Today a position
exists only on the error path (`ta_chart_position`, core ADR 0058). Two
sides, both in the codec interfaces of core ADR 0053:

- **Decode.** A spec node gets an optional span: a source index, an offset
  and length, and a line and column. The codec fills it in; zero is
  unknown, so a spec built in C costs nothing. The source index covers
  files a `ta_chart_src_resolver` pulled in. Line and column are needed as
  well as the offset because the Ruby DSL's source is the `file:line` an
  element was written on. Compiling copies the spans into a side table of
  the definition (per definition, not per actor, core ADR 0018), and core
  answers where a state, transition, action or expression is.
- **Encode.** `ta_definition_encode` fills the same table for the text it
  writes. The adapter can then serve a chart it has no file for as a
  virtual source with correct lines, which is the usual case in
  production.

From that table the host builds line → ID for `setBreakpoints`, snapping a
line to the nearest action, transition or state. Faults already carry an
offset within their expression (ADR 0058), so an exception can point at a
column.

## The DAP mapping

✓ works with core as it is and needs only the adapter. **host** needs work
in napi or the workbench. **core** needs a core ADR.

### Session

| DAP | Mapping | |
|---|---|---|
| `initialize` | The capabilities below | ✓ |
| `attach` | Address, mode and `allow`, checked by the server | host |
| `launch` | Workbench only: run a chart directory locally | ✓ |
| `restart` | Fork: make a new copy from the live actor. Live: reattach | ✓ |
| `disconnect` / `terminate` | Detach. The actor is never destroyed, even with `terminateDebuggee`. The reply scope is closed | host |

### Threads and execution

| DAP | Mapping | |
|---|---|---|
| `threads` | The attached actor and its invoked children; every actor in the workbench. Named by address and active leaf states | ✓ |
| `thread` event | From the `SPAWNED`, `FINISHED` and `DESTROYED` records | ✓ |
| `pause` | In a fork, a busy actor stops at the next unit. A live actor has its mailbox held and reports `stopped` when its current macrostep ends | host |
| `continue` | Resume with the normal budgets and release held mail | ✓ |
| `stepIn` | One unit (`action_budget: 1`, core ADR 0061): an action or a `<foreach>` iteration, into `<if>` and `<foreach>` bodies | ✓ |
| `next` | The next action at the same block depth or shallower | host |
| `stepOut` | To the end of the block, then the microstep, then the macrostep | host |
| Stepping granularity | `instruction` one unit, `statement` one action (the default), `line` one microstep | ✓ |
| `stepBack` / `reverseContinue` | Fork only: restore the image captured at each stop | host |
| `restartFrame` | Fork only: the image captured when that macrostep began | host |
| `gotoTargets` | The states at a source line, or every state | core (spans) |
| `goto` | An operator transition | **core** |

### Stopping

| DAP | Mapping | |
|---|---|---|
| `setBreakpoints` | A line maps to an action, a transition or a state: stop before the action, before the transition's content, or on entry or exit | core (spans), host |
| `setFunctionBreakpoints` | `past_due` (entry), `exit:past_due`, `on:payment.failed` (a macrostep takes that event), `idle → busy` (a transition by its core ADR 0071 name) | host |
| `condition` | Evaluated in the data model where the stop would happen | host |
| `hitCondition` | Counted by the adapter | host |
| `logMessage` | A logpoint: output, no stop. The only kind in watch mode | host |
| `setExceptionBreakpoints` | Filters `error.execution`, `error.communication`, `error.platform`, `ignored` (an event that enabled no transition), `stopped` (out of memory), `dead-letter` (napi) | ✓ |
| `dataBreakpointInfo` / `setDataBreakpoints` | A data location; stops after an `ASSIGN` record writes it | host |
| `stopped` reasons | `breakpoint`, `step`, `pause`, `exception`, `goto`, `data breakpoint`, `function breakpoint` | ✓ |

On a live actor, only breakpoints between macrosteps stop it (see "Holding a
live actor" below); inside a macrostep they act as logpoints.

Breakpoints start with the host single-stepping on `action_budget: 1` and
comparing each position with the breakpoints. A breakpoint set over
compiled IDs in core comes later, and only if that is too slow.

### State

| DAP | Mapping | |
|---|---|---|
| `stackTrace` | Innermost first: the action, the enclosing `<foreach>` iterations, the block (`<onentry>` of S, content of T), the microstep, the macrostep and its event. After a `label` frame, the causal chain: the step that sent the event, following each record's `cause` across actors | ✓ (the position comes from `PAUSED`; core: put it in inspection) |
| `scopes` | Data model, Event, Configuration, Queues, Timers, Invocations, System | ✓ |
| Data model | `ta_actor_datamodel_root`, expanded lazily | ✓ |
| Event | `_event`: name, type, sendid, origin, origintype, invokeid, data | ✓ |
| Configuration | The active state tree and the saved history values | ✓ |
| Queues | The internal queue, the current macrostep's event, and the host mailbox, including queued debugger messages | ✓, host |
| Timers | Delayed sends: send ID, event, target, and when each is due in logical time | ✓ |
| Invocations | Each active invoke, linking to the child's thread | ✓ |
| System | `_sessionid`, `_name`, `_ioprocessors`, now, the definition revision | ✓ |
| `setVariable` | Data model only, through the host's data model; the other scopes are read-only | host |
| `evaluate` (`watch`, `hover`) | An expression in the data model; hovering a `cond` shows its value now | host |
| `evaluate` (`repl`) | The console commands | host |
| `completions` | Event names, state IDs, data locations | ✓ |

### Sources and modules

| DAP | Mapping | |
|---|---|---|
| `loadedSources` / `loadedSource` event | A chart's file when there is one, otherwise a virtual source with a `sourceReference` | ✓ |
| `source` | Text from `ta_definition_encode`, with the encode-side spans | core (spans) |
| `modules` / `module` event | Definition families and their revisions, retired ones included | ✓ |

### Events the adapter sends

- `output` with category `console`: a summary of each macrostep (the
  event, guards and their results, transitions, assignments with values,
  sends). With `stdout`: `<log>` records from the log sink. With
  `important`: dead letters and dropped delayed sends.
- `invalidated` after every macrostep, so the client re-fetches variables
  and threads.
- `continued`, `stopped`, `thread`, `module`, `loadedSource`.
- `terminated` when the attached actor finishes, is destroyed, or its
  address is left empty.

### Capabilities

Declared: `supportsConfigurationDoneRequest`,
`supportsConditionalBreakpoints`, `supportsHitConditionalBreakpoints`,
`supportsLogPoints`, `supportsFunctionBreakpoints`,
`supportsDataBreakpoints`, `exceptionBreakpointFilters`,
`supportsEvaluateForHovers`, `supportsCompletionsRequest`,
`supportsSetVariable`, `supportsStepBack` (fork),
`supportsRestartFrame` (fork), `supportsGotoTargetsRequest`,
`supportsSteppingGranularity`, `supportsLoadedSourcesRequest`,
`supportsModulesRequest`, `supportsRestartRequest`,
`supportTerminateDebuggee: false`.

Left out: `disassemble`, `readMemory` and `writeMemory`, `stepInTargets`,
`terminateThreads`. A chart has no memory or instructions to show, and
ending a thread would destroy an actor.

## Two adapters: tinyactors-test and napi

The adapter is built twice, on purpose. The one in **tinyactors-test** is
Ruby, in the workbench: it debugs the conformance suite, shrunk scenarios
and charts under development, and it drives core through the same
generated JSON-RPC bindings as every test, so it exercises core's side of
the debugger directly. The one in **napi** is JavaScript: it is the one
production attaches to. Each is written in its host's language and owns
its host's policy; they share the protocol mapping in this document and a
transcript suite that keeps them in step.

### tinyactors-test: Ruby over the JSON-RPC host

Everything the adapter needs is already bound, so it is Ruby only. No C
changes to the host.

| Piece | Built from |
|---|---|
| DAP session, framing, console, completions | Ruby in `ruby/workbench/`, next to the REPL, which already completes event names |
| Holding and stepping | The Ruby driver owns scheduling (the scenario runner's run-to-quiescence loop), so holding an actor is leaving it out of that loop, and stepping is `ta.actor.resume` with `action_budget: 1` |
| Trace records | A `trace.recording` instrument; the adapter reads its journal after each step (`ruby/tinyactors_test/trace.rb`) |
| Evaluate, hover, conditions | The jq data model: `json.transform` against the data root, as the workbench's guard display does (`session.rb`) |
| Scopes | `ta.actor.inspect`, `ta.actor.datamodel_root`, `ta.actor.active_states` |
| The `debug` I/O processor | An `io.recording` instrument registered for the `debug` type; replies are its journal |
| Forks | `ta.actor.capture` and `ta.actor.reconstruct` into a second system handle, once core restores images across systems |
| `replace` | The workbench's existing replacement (`Session#replace_actor`) |
| Source spans | The Ruby DSL records `caller_locations` for each element; today it records none |
| Transport | stdio, launched by the client; `mise run dap` in `tinyactors-test` |

### napi: JavaScript in `lib/`, C only in the scheduler

The split follows the one napi already has. Hand-written C is plumbing:
"mailboxes, ready and runnable queues, router, directory, pump, clock"
(napi ADR 0013 point 8), and the post, mailbox, deliver and resume path
never goes through generated code. Policy over data that C hands over once
per turn is JavaScript, as for traces (napi ADRs 0015 and 0016); C does
more only where volume demands it (napi ADR 0017). A debugger runs at
human speed, and the host-kernel proposal wants a host-neutral `lib/` the
browser build can share, so the adapter is JavaScript.

**`lib/debug.js`**

| Piece | Built from |
|---|---|
| The DAP session object, framing, modes and `allow`, the audit hook | JSON and policy; the application supplies transport and authentication |
| Console, `evaluate`, `setVariable`, `completions` | The data model is JavaScript objects (napi ADR 0001), so evaluating and setting is plain JavaScript |
| Scopes and variables | `inspect()` and the data root |
| Stack frames and output | Spans read with `decodeTraces()`, named with the definitions' `inspect()` |
| Sharing the trace sink | A wrapper around `setTraceSink`, so the application's sink and the adapter both get each turn's bytes, and `watchTraces({ sessions })` while attached |
| The `debug` I/O processor | `registerIO('debug', …)` and `openScope()` |
| Forks | `capture`, `structuredClone` of the root, `reconstruct` into a debug `System` with stub processors registered through `registerIO` |
| Stepping and breakpoints, first version | One resume with `action_budget: 1` per turn; the turn's `PAUSED` record reaches JavaScript at the end of the turn, which decides whether to go on |
| Source-map lookup, virtual sources | Tables read from definitions |

**C in the scheduler** (and later in the host kernel)

| Piece | Why C |
|---|---|
| A `held` scheduling state | Modelled on `quarantined` (napi ADR 0008, `TNA_SCHED_QUARANTINED` in `src/scheduler.h`): out of the scheduling queues, mail kept but not delivered, released on request. A separate state, because a fault while held must still quarantine and report. Plus "deliver exactly one held message", for `next` |
| Budgets for one actor | The debugged actor resumes with `action_budget: 1` while the rest keep `resumeBudget` and `actionBudget`; the pump chooses budgets per resume |
| Operator transitions in mail | A `goto` queues behind real mail, so the envelope carries it to `ta_actor_deliver` on the hot path |
| `abandon` | Settles napi's own state for the actor: its `send()` waiter rejects and its runnable entry is cleared |
| Breakpoints in the trace sink, later | Only if one turn per unit is too slow: a per-actor set of IDs checked where napi's C sink receives `PAUSED` |

The new core functions (operator transition, `abandon`, withdrawal) get
their bindings from the generator once `bindings.rb` classifies them (napi
ADR 0013); the facade methods on them are a few lines of `lib/`.

### Keeping the two in step

A shared suite of DAP transcripts: a chart, then the requests a client
sends and the responses and events the adapter must answer with. Each
host runs every transcript. napi builds charts in JavaScript and does not
import SCXML documents, so a transcript names its chart, and each host
has a fixture of that chart in its own authoring form, with the same
state IDs and events. Transcripts name places by state, transition and
action names, not by line, because the two fixtures' lines differ. Where
an expression is needed, a transcript carries a jq and a JavaScript form.

### All the pieces

| Piece | Where | Needed for |
|---|---|---|
| Source spans on decode and encode | a core ADR extending 0053 and 0058; tinyactors-scxml; the Ruby DSL and napi's builder | line breakpoints, frame locations, `gotoTargets`, virtual sources |
| Operator transition and abandoning a macrostep | a core ADR narrowing 0030 | `goto`, `abandon` |
| An image restores into any system that registers its revision | a core ADR refining 0038 and 0051 | forks with their own clock and stubs |
| Withdrawing one delayed send by owner and send ID | the core ADR that adds replay's withdrawal by time | `cancel` |
| The paused position in `ta_actor_inspection` | core | attaching to an actor that is already paused |
| A breakpoint set over compiled IDs | core or napi's sink, later | only if single-stepping through the host is too slow |
| Recording operator jumps | [replay-based persistence](replay-persistence.md) | debugging past events; replay keeping manual fixes |
| The Ruby adapter | tinyactors-test | the workbench, the conformance suite, scenarios |
| `lib/debug.js` and the scheduler changes | tinyactors-napi | production attach |
| The transcript suite | shared by both hosts | keeping the two adapters in step |
| An extension that registers the debug type | new, about 50 lines | VS Code; the other clients need only configuration |

Neither adapter waits for the other. Watch mode, `send`, stepping on
`action_budget`, scopes, evaluate and completions need no new core API
and can be built in both hosts first. Forks with their own clock wait for
the image ADR, `goto` and `abandon` for the operator ADR, and line
breakpoints for source spans.

## Debugging the past

The event that caused a bug usually arrived weeks ago. A capture shows
only the present, and traces exist only if the actor was watched then.
With replay-based persistence, the adapter could replay an actor's log
into a fork, stop at the event from three weeks ago, and step back from
there. An operator jump is an input like a delivered event, so the log has
to record it, with states by name rather than ID, because replay runs
against the current revision.

## Answers from the ADRs

The first draft left five points open. Each is answered by an existing
ADR, or by the code that implements it.

### Replacing an actor inside a macrostep

Core does not check the incumbent: `ta_actor_prepare_replacement` and
`ta_actor_activate_replacement` refuse only a *replacement* that is busy
(`src/actor.c`). napi refuses the incumbent: napi ADR 0005 point 12 says
`activate()` answers `busy` while the incumbent is inside a macrostep,
because activation moves the incumbent's waiting mail to the replacement
and must not drop mail it has accepted. So in napi, `replace` cannot
rescue a macrostep that never ends. That is what `abandon` is for, and it
comes first. Relaxing napi's rule would not be better: the replacement
binds the incumbent's data-model root (core ADR 0037), which a half-run
microstep may have half changed.

### Cancelling one delayed send from outside the chart

There is no public operation. Core ADR 0012 scopes cancellation to the
session that created the send, and a `<cancel>` inside the chart is the
only way in. Core already has the mechanism: core ADR 0054 removes a
cancelled or withdrawn send from its owner's array in O(k + log A), and
`ta_timer_withdraw` does it for completion sends (`src/invoke.c`).

The root roadmap's next core item ("Close the core API gaps for log-based
durability") already needs a withdrawal: [replay-based
persistence](replay-persistence.md) withdraws pending sends due at or
before a given time when replay ends. An operator withdrawal by actor and
send ID belongs in the same ADR. It names the owning actor, so it keeps
ADR 0012's scope: only that actor's sends can be withdrawn. The send ID is
the text core owns (core ADR 0048), which the Timers scope shows.

### Holding a live actor, webhook timeouts and retries

napi ADR 0005 point 11 settles what a waiting caller sees:

- A `send()` promise completes when the macrostep its message starts ends.
  A macrostep paused by a budget has not ended.
- With `timeout` or `signal`, a message *still in a mailbox* is withdrawn
  and never delivered. A message the actor *already took* is not undone:
  its macrostep runs, and only the promise rejects.
- With `until: "accepted"`, the promise resolves as soon as the message is
  queued.

So a hold that stops the actor **between macrosteps**, with its mailbox
held, is safe for a webhook. A handler that waits for completion times
out, its message is withdrawn, and Stripe's retry delivers it once, later.
A handler that acknowledges on `accepted` has already answered, and its
message waits for the hold to end.

A hold that stops the actor **inside** a macrostep a webhook started is
the one that duplicates: the handler's promise rejects, Stripe retries,
and the first delivery still completes when the hold ends. So a live
`hold` stops only between macrosteps, and breakpoints inside a live
actor's macrosteps are not offered. Stepping through a macrostep is what
a fork is for.

### Transport, authentication and audit

Core ADR 0002 makes Tinyactors an in-process library whose embedder owns
I/O. napi follows it for traces: napi ADR 0015 hands records to a
JavaScript hook with "no HTTP", and napi ADR 0016 hands the application
OTLP bytes that it posts itself, with no telemetry dependency.

The debug adapter follows the same rule. napi opens no port and checks no
credentials. It offers a DAP session object: the application passes it
each incoming DAP message and sends on what it returns. The application
puts it behind its own transport and authentication (a WebSocket behind
its admin login, stdio over SSH), and decides `mode` and `allow` for each
user before it creates the session.

For audit, the debugger's messages, jumps and withdrawals run as
macrosteps under watched trace contexts, so they reach the application's
trace sink like everything else, marked as the debugger's. Attaching,
detaching and each console command go to a hook the application stores
wherever it keeps audit records.

### `next` on an idle actor

Core ADR 0011 gives the host the external queue and the choice of which
event an actor gets next. An idle actor holds no event (core ADR 0069) and
has nothing to step. So `next`, `stepIn` and `stepOut` on an idle actor
are the host's to define: the adapter takes the next message from the
held mailbox and stops at the start of its macrostep, before transition
selection.

When the mailbox is empty, DAP allows the request to answer at once and
send `stopped` later, so the actor stays running until the next message
arrives. In a fork nothing arrives by itself: time moves only when told
to (core ADR 0012), so the console says what is next due and suggests
`clock`.
