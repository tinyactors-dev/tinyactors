# A debug adapter: attach to an actor, poke it, unwedge it

**Status:** Sketch (2026-09-25)
**Touches:** tinyactors-core (source spans, operator transitions, inspection), tinyactors-scxml, tinyactors-test, tinyactors-napi, a new VS Code extension
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
  `ta_actor_reconstruct`, which does not touch the directory (core ADR 0052),
  so the copy is never routable. Everything is allowed on the copy.
- **live.** The attached actor itself. `send`, `set` and `goto` as `allow`
  permits. Stopping the live actor needs `hold`, and it resumes by itself
  after a timeout.

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

A session image is not a wire format: it holds references to live host
values (core ADR 0038). So the debug server for production runs inside the
production process, and the fork lives there too.

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
| `replace <file>` | Hot-replaces the actor with a new revision; it restarts from the initial state and keeps its data | fork; live with `goto` |
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
| `pause` | A busy actor stops at the next unit. An idle one has its mailbox held and reports `stopped` at its stable configuration | host |
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

## Where the pieces go

| Piece | Where | Needed for |
|---|---|---|
| Source spans on decode and encode | a core ADR extending 0053 and 0058; tinyactors-scxml; the Ruby DSL | line breakpoints, frame locations, `gotoTargets`, virtual sources |
| Operator transition and abandoning a macrostep | a core ADR narrowing 0030 | `goto`, `abandon` |
| The paused position in `ta_actor_inspection` | core | attaching to an actor that is already paused |
| A breakpoint set over compiled IDs | core, later | only if host single-stepping is too slow |
| Recording operator jumps | [replay-based persistence](replay-persistence.md) | debugging past events; replay keeping manual fixes |
| Debug server, `debug` I/O processor, forks, modes and audit | tinyactors-napi | production attach |
| A DAP adapter over the existing JSON-RPC | tinyactors-test | the first version: the workbench and failing scenarios |
| An extension that registers the debug type | new, about 50 lines | VS Code; the other clients need only configuration |

The first version fits the test host: the Ruby driver already owns
scheduling, the JSON-RPC host binds every core function, and the
`trace.recording` instrument exists. It would let a failing W3C IRP chart
or a shrunk scenario be stepped through in an editor. napi follows, for
production attach, once the core pieces exist.

## Debugging the past

The event that caused a bug usually arrived weeks ago. A capture shows
only the present, and traces exist only if the actor was watched then.
With replay-based persistence, the adapter could replay an actor's log
into a fork, stop at the event from three weeks ago, and step back from
there. An operator jump is an input like a delivered event, so the log has
to record it, with states by name rather than ID, because replay runs
against the current revision.

## Open points

1. Whether `ta_actor_prepare_replacement` accepts an incumbent that is
   inside a macrostep. Its header comment does not say. If it refuses,
   `abandon` is the only way out of a macrostep that never ends, and
   `replace` must say so.
2. Whether an operator can cancel one delayed send from outside the
   chart. A timer that should not fire is a common wedge; this may need a
   core operation of its own.
3. How a live `hold` interacts with a webhook handler that waits for its
   macrostep: Stripe retries a webhook that times out. The hold timeout
   may need to be shorter than the caller's.
4. Who may attach, and to what. The debug server sees customer data and
   can change production state, so it needs authentication, an audit log,
   and read-only by default. None of that is designed here.
5. Whether `next` should wait for the next event when the actor is idle, or
   do nothing.
