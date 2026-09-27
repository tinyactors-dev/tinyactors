# The debug adapter

`bin/dap` is a debugger for charts that any client of the Debug Adapter
Protocol (DAP) can drive: VS Code, nvim-dap, Emacs dape, and others. It
runs a workbench directory, the same directories `bin/workbench` runs,
headless: no tmux and no REPL, only the actors and a client that watches,
stops and pokes them. Saving a chart still replaces its actors in place.

```text
cd tinyactors-test
mise run build
mise run dap          # DAP on stdin and stdout; a client starts this
bin/dap --port 4711   # or serve sessions on 127.0.0.1:4711
```

The design, and the adapter that production attaches to in
tinyactors-napi, are in [the debug adapter
proposal](../../../docs/proposals/debug-adapter.md). What each DAP message
does, and how far each host has got, is in [the status
map](../../../docs/debug-adapter.md).

## Starting it from a client

Every client starts the adapter and sends `launch` with the workbench
directory. The launch arguments:

| Argument | |
|---|---|
| `workbench` (or `program`) | the directory of chart files; required |
| `stopOnEntry` | stop once every actor has started, before any event reaches one |
| `host` | the `tinyactors-test` executable, when not the one beside `bin/dap` |

`attach` answers an error: attaching to a running actor is what
tinyactors-napi's adapter is for.

### VS Code

VS Code runs a debug adapter only for a debug type an extension
registers: install [tinyactors-vscode](../../../tinyactors-vscode/README.md)
(`mise run build` there writes the `.vsix`). It starts `bin/dap` itself,
found in the workspace or set with `dapPath`:

```json
{
  "type": "tinyactors",
  "request": "launch",
  "name": "Workbench",
  "workbench": "${workspaceFolder}/workbench",
  "stopOnEntry": true
}
```

To run the adapter yourself, for example to read its stderr, start
`bin/dap --port 4711` and add `"debugServer": 4711` to the configuration:
VS Code then connects to the port instead of starting a process. Each
connection is one session with a host of its own.

### nvim-dap

```lua
local dap = require('dap')
dap.adapters.tinyactors = {
  type = 'executable',
  command = '/path/to/tinyactors-test/bin/dap',
}
dap.configurations.ruby = {
  {
    type = 'tinyactors',
    request = 'launch',
    name = 'Workbench',
    workbench = '${workspaceFolder}',
    stopOnEntry = true,
  },
}
```

### Emacs dape

```elisp
(add-to-list 'dape-configs
             `(tinyactors
               modes (ruby-mode ruby-ts-mode)
               command "/path/to/tinyactors-test/bin/dap"
               :type "tinyactors"
               :request "launch"
               :workbench dape-cwd
               :stopOnEntry t))
```

Then `M-x dape RET tinyactors`.

## What the client shows

**Threads** are the workbench's named actors and, below each, the children
its invocations started, named by address (a child by its parent and its
`<invoke id>`) and active leaf states: `counter/acct1 · busy`,
`supervisor/boss invocation worker · working`. A named actor keeps its
thread across a reload. What the adapter writes names an actor the same
way, without the states (`sent bump to counter/acct1`), as napi's adapter
does; the console's `@actor` takes a shorter label (below).

**Stopping is all-stop.** When one actor stops, nothing else runs until
the client continues or steps. Whenever a
breakpoint, a step or a pause is armed, the host resumes one unit at a
time (`action_budget: 1`, core ADR 0061) and pauses at every microstep's
start, after selection and before its first unit
(`pause_at_microstep_start`, core ADR 0086); otherwise it runs with the
workbench's budgets.

**Frames**, innermost first: the unit the actor runs next, as a
debugger's current line is (`assign 2`), the `<if>` and `<foreach>` around
it (`foreach 2 iteration 2`), the block (`onentry of busy`, `transition
idle → busy on bump`, `onexit of busy`), the microstep with its
transitions, and the macrostep with the event it took. At a microstep's
start the top frame is its first unit, which has not run yet; a microstep
with no content stands at its first transition (`transition busy → idle
on done`), and an operator's jump without content at `taking an
operator's jump`. A pause that lands between two microsteps (the
workbench's microstep budget ran out) shows `selecting transitions`. An
actor at rest has one frame, `idle in <states>`, which says what holds it
while the system is stopped: `idle in idle (held; 1 message waiting)`,
counting the console's mail waiting for it. One holding an event it has
not begun has `about to take <event> (before transition selection)`.

After the macrostep, a `caused by` label frame starts the causal chain:
the step that sent the event the macrostep took (`supervisor/boss: sent
bump in step 3.1`, at the line of the send), then the step that sent that one's, across
actors, back to `the debug console sent poke` or `the host delivered
tick`. It follows each MACROSTEP_BEGIN record's `cause`, the step key of
the step that produced the event (core ADR 0067), to the SENT record of
that step. The adapter keeps the last thousand macrosteps and steps it
saw; a step older than that, or before the adapter started, ends the
chain with a frame that says it was not seen.

Each frame is at the file and line its element was written on (core ADR
0085): the chart DSL records the caller of every `state`, `transition`,
`initial` and action call, and `ta.definition.span` answers it by ID. An
action's frame is at its action, a block's at its state or transition, a
microstep's at its first transition (a jump's at its first target), and a
resting actor's at its first leaf state. The DSL knows lines only, so
columns are 1.

A chart an `invoke chart:` inlines is compiled without spans (the DSL
leaves them out of inline content). Its frames are in a virtual source
instead: the SCXML the codec encodes from the definition
(`ta_definition_encode` with its source map, core ADR 0085), served by
`sourceReference` as `worker.scxml`, at the place the encode wrote each
element. It is also that revision's loaded source.

Where an actor stands comes from `ta.actor.inspect` (core ADR 0083): its
step key and the next unit, so an actor reloaded from an image, as writing
data does, still shows where it stands. A `<foreach>` that has not run
and the same loop before its first iteration have the same position (the
ADR's open point 1); only the innermost loop's iteration is known, so an
enclosing `<foreach>` is shown without one (open point 2). The
transitions of the microstep and the event of the macrostep come from
the trace records.

**Scopes**, the same for every frame of a thread:

| Scope | |
|---|---|
| Data model | the data root, read again on each expansion; writable |
| Event | `_event` of the step the actor is in; at rest, of its last macrostep (`taken: in its last macrostep`) |
| Configuration | the active states as a tree, and what each history state recorded |
| Queues | the internal queue, the event being taken, and the console's `mailbox` |
| Timers | pending delayed sends, each by its send ID: event, target, when it is due |
| Invocations | each invocation, and the thread of its child |
| System | `_sessionid`, `_name`, `_ioprocessors`, `now`, `actor` (slot:generation), the definition, `residency`, `scheduling`, `published`, `allocatedBytes` |

The names are napi's adapter's wherever both hosts show the same thing;
`_event`'s SCXML fields (`sendid`, `origintype`, `invokeid`) and
`_ioprocessors` are real SCXML data only this host has.

**Output.** Each macrostep that ends writes its summary to the console as
one output: the event, each microstep with what ran in it (conditions,
assignments with their values, sends, raised events, failures), and the
configuration it ended in:

```text
counter/acct1 macrostep 2: bump {"n":1}
  idle → busy on bump
    assign .count = 1
    assign .last = {"n":1}
  now in busy
```

Transitions and actions there are named as napi's trace spans name them
(`idle on ping (internal)`, `<assign> in idle on fail (internal)`).
`<log>` records are `stdout`, as `label: value` (`busy: 1`). The
workbench's own notes are `console`, and what a reload loses is
`important`.

**Modules** are definition revisions, named by family, with the revision
as the version; a revision a reload retired stays listed as `(retired)`.
**Loaded sources** are the chart file of each revision, with the text it
had then, so a retired revision still shows the chart it ran.

## Stepping

| Request | `statement` (the default) | `instruction` | `line` |
|---|---|---|---|
| `stepIn` | one unit: an action, a `<foreach>`, or one of its iterations | one unit | to the next microstep |
| `next` | until a unit at the same depth or shallower runs, or the block ends | one unit | to the next microstep |
| `stepOut` | to the end of the block (between microsteps: of the macrostep) | as `statement` | to the end of the macrostep |

`stepIn` at `statement` granularity passes over a stop that names the
unit it started at, so the repeated `<foreach>` position above costs no
extra step; `instruction` stops there anyway.

Stepping stops at each microstep's start (core ADR 0086): `stepIn` from a
microstep's last unit, a step by microsteps (`line`), and a step between
two microsteps all stop before the next microstep's first unit, where
nothing of it has run: no `<onexit>`, no transition content, no entry. A
breakpoint on a state's entry or exit, on a transition, or on a line stops
before the first unit of its block or its action, the microstep's first
included. A breakpoint whose block has no unit (a state with no
`<onexit>`) hits at the next place the actor stops: in a microstep with no
content, its start.

A step of an idle actor waits for its next message and stops before the
macrostep that takes it, as the proposal's "`next` on an idle actor" says.
The request answers at once, a `continued` event says everything runs, and
when nothing is due the console says what to do: `send` it one, or move
the clock to the next delayed send. Console mail waiting for it is its
next message.

## Breakpoints

| Kind | |
|---|---|
| Function | `busy` (entering), `exit:busy`, `on:payment.failed` (a macrostep takes it; stops before selection), `idle → busy` (a transition, `->` works too). Conditions are jq over the data model; hit conditions are `5` or `>= 5` (from the fifth hit), `== 5`, `% 5` |
| Exception | `error.execution`, `error.communication`, `error.platform`, `ignored` (an event enabled no transition), `stopped` (an allocation failed) |
| Data | a data-model location of one actor (`counter/acct1 ["count"]`); stops after an `<assign>` writes it or anything under it (`count = 5`) |
| Line | a line of a chart's Ruby file; conditions, hit conditions and log points (below) |

A line breakpoint stands for the element written on that line, the most
specific when there are several (an action, then a transition, then a
state), or for the nearest line that has one, the following line on a
tie. The client shows it at that line. On an action it stops before the
action runs; on a transition, before the transition's content; on a
state, before its onentry. It stops with reason `breakpoint`. A log point
(`logMessage`) writes its message to the console, each `{expression}`
replaced by its value in the data model, and does not stop.

Breakpoints are resolved against every definition the workbench runs, and
resolved again whenever those change. A breakpoint set
before the actors start is unverified until its chart loads; saving a
chart that moves lines moves the breakpoint to what is now on or near its
line. Both are announced with a `breakpoint` event. `breakpointLocations`
lists the lines that hold an element. A breakpoint is matched against
where the actor stands, not against trace records, so it stops before a
`<log>`, which writes none, like any other unit.

## The debug console

| Command | |
|---|---|
| `send [@actor] <event> [json]` | puts the event in the mailbox of the frame's actor, or the one named, with the console as its origin |
| `goto [@actor] <state…> [from <state>]` | an operator transition (core ADR 0082): at once when the system is stopped and the actor rests with nothing waiting, and then stops with reason `goto`; otherwise it waits in the mailbox |
| `abandon [@actor]` | abandons the actor's macrostep (core ADR 0082); it keeps the configuration after its last completed microstep, and a stopped system stops again there |
| `cancel [@actor] <send ID>` | withdraws one pending delayed send (core ADR 0084), by the ID the Timers scope shows: the chart's, or `#_send.<n>` (ADR 0048) |
| `clock +<duration>` | advances the workbench's logical time: `+500ms`, `+5s`, `+2m`, `+1h`, `+3d` |
| `<expression>` | jq over the actor's data model |

`@actor` is a thread's label: `acct1`, or `boss.worker` for a child.

Expressions see `$_event`, `$_sessionid`, `$_name`, `$_ioprocessors` and
`In()`, bound by the adapter: `$_event` is the event of the step the actor
is stopped in, and `In()` tests the configuration after its last completed
microstep (core ADR 0061). Completion offers the events the actor listens
for after `send`, state IDs after `goto` and `from`, and data locations
otherwise.

The console is a sender an actor can answer (core ADR 0068): the adapter
registers an I/O processor of type `debug` with the `io.connection`
instrument in the workbench's system, opens one scope there, and delivers each `send` with an origin in it. A
chart's `<send targetexpr="$_event.origin"
typeexpr="$_event.origintype">` then shows in the console as a reply
(`counter/acct1 replied pong 4`).

**The mailbox.** What the console sends an actor, events and jumps alike,
waits in that actor's mailbox, oldest first, as a message to one of
napi's actors does, and the Queues scope shows it. The Scheduler's monitor
hands the actor its next message when the actor rests and its system
runs, before the actor's next round: on `continue`, on a step, or at once
while everything runs. A message sent while the system is stopped says so
(`sent bump to counter/acct1; it waits in the mailbox (1 message):
continue or step to deliver it`), and a breakpoint on `on:bump` stops
before the macrostep that takes it. Mail from other actors goes through
the router as always; the mailbox holds only the console's. A message the
actor refuses when it is handed over (a jump `from` a state no longer
active) leaves the mailbox, and the console says why.

## Unwedging an actor

A chart that nobody prepared for an operator has no admin transitions.
`goto` delivers an event, `operator.goto` with the targets (and `from`) as
its data, together with an operator transition: core takes that event by
a transition built from the targets instead of by the chart's transitions,
through the normal engine, with every exit, entry, history and invocation
a chart transition would have (core ADR 0082). Its handlers see the
operator's event as `_event`. When the system is stopped and the actor
rests with nothing in its mailbox, the jump is delivered at once: its
macrostep runs, only that actor runs meanwhile, and the system then stops
with reason `goto`. Otherwise it waits in the mailbox (`goto resting
queued`) and is taken like any message, with no stop of its own; core
answers busy to a jump of an actor inside a macrostep (ADR 0082), so the
queue is the host's. The console shows the jump's summary, whose
microstep reads `goto: exited <states>, entered <states>`. A jump carries
no console origin: it is delivered directly, not through the `debug` I/O
processor.

A macrostep that never ends is ended with `abandon`: the actor keeps the
configuration after its last completed microstep, and a system stopped in
that macrostep stops again with reason `pause` (`its macrostep was
abandoned`), where the actor now rests. One stopped inside a microstep
first finishes that microstep (core abandons only between microsteps), and
its units run as usual.

`cancel` withdraws a pending delayed send of the actor, even while its
macrostep is stopped (core ADR 0084). A send its target refused once is
its receiver's and is not withdrawn.

The DAP `goto` request does what the console's `goto` does, to one state.
Its targets come from `gotoTargets`: at a line of a chart's file, the
states written on that line, else the state an action or a transition
written there belongs to; without a line, every state of the chart (the
source's, else the stopped thread's). `replace` is not a command here:
saving the chart file replaces its actors.

The adapter keeps no past: `stepBack`, `reverseContinue`, `restartFrame`
and `restart` answer an error, and their capabilities are not declared.
It stops and steps the workbench's own actors, on a developer's machine;
napi's adapter, which attaches to production actors, never stops one
(napi ADR 0026).

## Writing data

`setVariable` and `setExpression` write a data-model location. Core has
no operation that changes an actor's data root, so the adapter uses the
one the public API allows: it captures the actor, unloads it, and loads it
again with the changed root as its data-model binding (core ADR 0038). The
configuration, the delayed sends, and a macrostep stopped between two
units all survive, and the thread stays the same. Two cases are refused
rather than harmed: an invoked child has no address to load it at, and a
load ends every active invocation with `error.communication`.

## Session end

`terminate` ends the session with a `terminated` event and leaves the
actors as they are; `disconnect` then shuts down the host the adapter
started. The adapter never destroys an actor on its own, and declares
`supportTerminateDebuggee: false`.

## How it is put together

| File | |
|---|---|
| `protocol.rb` | the base protocol: `Content-Length` framing |
| `adapter.rb` | requests, the event loop, launching the workbench |
| `control.rb` | the Scheduler's monitor: budgets, reading trace records, stops and steps |
| `breakpoints.rb` | line, function, exception and data breakpoints, found in trace records and positions |
| `source_map.rb` | lines of the chart files and the elements written on them (core ADR 0085) |
| `threads.rb` | threads with stable IDs |
| `frames.rb` | stack frames from an actor's position |
| `variables.rb` | scopes, variables, and writing the data model |
| `expressions.rb` | jq over the data model, with the system variables bound |
| `console.rb` | the console commands, completions, the `debug` I/O processor, the actors' mailboxes |
| `operator.rb` | jumps, abandoning a macrostep, withdrawing a delayed send |
| `catalog.rb` | modules and loaded sources |
| `labels.rb` | names for compiled IDs (core ADR 0071) |

One thread touches the host: it answers requests, runs the system and
applies reloads. A reader thread turns the input into messages, and the
workbench's watcher turns saves into reloads; both only queue work. The
system runs through `TinyactorsTest::Scheduler`, the same loop as every
scenario and the workbench REPL, with `Control` as its monitor. Between
two resumes the Scheduler asks whether a message is waiting, so a `pause`
lands at once even in a macrostep that never ends.

`ruby/dap_selftest.rb` starts `bin/dap` over pipes, drives it with a
scripted client against a workbench directory it writes, and checks every
message the adapter implements and every one it refuses. `mise run test`
runs it; `DAP_TRACE=1` prints every message.

## Shared transcripts

`ruby/dap_transcripts.rb` plays the DAP transcripts in `dap/transcripts/`
against this adapter, as tinyactors-napi's `test/dap-transcripts.js` plays
them against its own: each is a behaviour both adapters must show, so the
two stay in step. `mise run test` runs it. To add one, write the
transcript, and the chart in both forms (`dap/charts/<chart>.rb` and
`.js`) if it needs a new one, then play it in both hosts:

```text
ruby ruby/dap_transcripts.rb <name>               # DAP_TRACE=1 prints every message
cd ../tinyactors-napi && node test/dap-transcripts.js <name>
```

The format, the matchers, and where the two adapters differ are in
[`dap/README.md`](../../dap/README.md).
