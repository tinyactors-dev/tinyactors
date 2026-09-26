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
registers. That extension (X1 in the status map) is not written yet; it
needs no code beyond its `package.json`:

```json
{
  "name": "tinyactors-debug",
  "engines": { "vscode": "^1.80.0" },
  "contributes": {
    "debuggers": [{
      "type": "tinyactors",
      "label": "Tinyactors workbench",
      "program": "/path/to/tinyactors-test/bin/dap",
      "languages": ["ruby"]
    }]
  }
}
```

With it installed, a launch configuration starts the adapter itself:

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
its invocations started, named by address and active leaf states:
`counter/acct1 · busy`, `worker (actor 4294967298) · working`. A named
actor keeps its thread across a reload.

**Stopping is all-stop.** When one actor stops, nothing else runs until
the client continues or steps. The host single-steps on `action_budget: 1`
(core ADR 0061) whenever a breakpoint, a step or a pause is armed, and
runs with the workbench's budgets otherwise.

**Frames**, innermost first: the unit the actor runs next, as a
debugger's current line is (`assign 2`), the `<if>` and `<foreach>` around
it (`foreach 2 iteration 2`), the block (`onentry of busy`, `transition
idle → busy on bump`, `onexit of busy`), the microstep with its
transitions, and the macrostep with the event it took. Between two
microsteps the top frame is `selecting transitions`. An actor at rest has
one frame, `idle in <states>`; one holding an event it has not begun has
`about to take <event> (before transition selection)`. Frames have no
source locations yet: those need source spans (core ADR 0086).

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
| Event | `_event` of the step the actor is in |
| Configuration | the active states as a tree, and what each history state recorded |
| Queues | the internal queue, the event being taken, and console messages waiting |
| Timers | pending delayed sends: event, target, when each is due |
| Invocations | each invocation, and the thread of its child |
| System | `_sessionid`, `_name`, `_ioprocessors`, logical time, the definition |

**Output.** Each macrostep that ends writes a summary to the console: the
event, the transitions, guards, assignments with their values, sends and
raised events. `<log>` records are `stdout`. The workbench's own notes are
`console`, and what a reload loses is `important`.

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

A resume that selects a microstep always runs its first unit before it
can pause (`action_budget` counts units run, and a microstep budget
pauses only between microsteps), so no stop is possible before a
microstep's first unit. Everything after it is reachable: a breakpoint on
a state's entry or exit, or on a transition, stops before the first unit
of its block, unless that unit is the microstep's first, in which case it
stops after it. `idle → busy` on a transition with content stops at its
second action; `busy` stops before `busy`'s onentry when the microstep
exited a state with an onexit or ran transition content first.

A step of an idle actor waits for its next event and stops before the
macrostep that takes it, as the proposal's "`next` on an idle actor" says.
The request answers at once, a `continued` event says everything runs, and
when nothing is due the console says what to do: `send` it one, or move
the clock to the next delayed send.

## Breakpoints

| Kind | |
|---|---|
| Function | `busy` (entering), `exit:busy`, `on:payment.failed` (a macrostep takes it; stops before selection), `idle → busy` (a transition, `->` works too). Conditions are jq over the data model; hit conditions are `5` or `>= 5` (from the fifth hit), `== 5`, `% 5` |
| Exception | `error.execution`, `error.communication`, `error.platform`, `ignored` (an event enabled no transition), `stopped` (an allocation failed) |
| Data | a data-model location of one actor; stops after an `<assign>` writes it or anything under it |

Line breakpoints answer an error until source spans exist (core ADR 0086).

## The debug console

| Command | |
|---|---|
| `send [@actor] <event> [json]` | delivers to the frame's actor, or the one named, with the console as its origin |
| `clock +<duration>` | advances logical time: `+500ms`, `+5s`, `+2m`, `+1h`, `+3d` |
| `<expression>` | jq over the actor's data model |

Expressions see `$_event`, `$_sessionid`, `$_name`, `$_ioprocessors` and
`In()`, bound by the adapter: `$_event` is the event of the step the actor
is stopped in, and `In()` tests the configuration after its last completed
microstep (core ADR 0061). Completion offers the events the actor listens
for after `send`, and data locations otherwise.

The console is a sender an actor can answer (core ADR 0068): the adapter
registers an I/O processor of type `debug` with the `io.connection`
instrument, opens one scope for the session, and delivers each `send` with
an origin in it. A chart's `<send targetexpr="$_event.origin"
typeexpr="$_event.origintype">` then shows in the console as a reply. A
message to an actor that is inside a macrostep waits, and the Queues scope
shows it, until the actor can take it.

`goto`, `abandon`, `cancel` and `fork` answer what they need: core ADRs
0082, 0084 and 0085. `replace` is not a command here: saving the chart
file replaces its actors.

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
| `breakpoints.rb` | function, exception and data breakpoints, found in trace records |
| `threads.rb` | threads with stable IDs |
| `frames.rb` | stack frames from an actor's position |
| `variables.rb` | scopes, variables, and writing the data model |
| `expressions.rb` | jq over the data model, with the system variables bound |
| `console.rb` | the console commands, completions, the `debug` I/O processor |
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
