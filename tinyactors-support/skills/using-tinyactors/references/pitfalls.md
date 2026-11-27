# Pitfalls

Each one is real: its symptom, its cause, its fix. Where the tracker at
mcp.tinyactors.dev has an issue for it, its identifier is given: call
`known_issues` (or `get_issue` with the identifier) for a workaround or a
fix, and `report_issue` with the same identifier if it costs you time,
so the costliest are fixed first.

Checked against `@tinyactors/node` 0.4.0 unless it says otherwise.

## A chart that does nothing

- **No `datamodel` attribute.** The chart gets SCXML's null datamodel,
  which evaluates no expression and says nothing: its `<log>`s, `<data>`
  and conditions do nothing. Write `datamodel="ecmascript"` (or
  `"python"`) on every chart.
- **A delay without its unit.** `delay="30"` or `delayexpr="seconds"` is
  refused: a static one is a compile error, a computed one raises
  `error.execution` and nothing is sent. Write `30s`, `1.5s`, `500ms`;
  `delayexpr="seconds + 's'"`.

## An event that goes to the wrong transition

**Event names match by prefix, word by word.** `event="join"` takes
`join`, `join.refused` and `join.anything`. An answer named after its
request lands in the request's transition: a chart that asks on `join`
and gets `join.refused` back asks again, for ever, and `await
host.idle()` never resolves. Name answers so that they do not extend the
request (`refused.join`, `listed` for `list`). `tinyactors lint` reports
a transition that can never be taken because an earlier one's event
covers it.

```js
import { compileCharts, Host } from '@tinyactors/node';

const compiled = compileCharts([{ name: 'member.scxml', text: `
<scxml xmlns="http://www.w3.org/2005/07/scxml" version="1.0"
       datamodel="ecmascript" name="member" initial="s">
  <state id="s">
    <transition event="join"><log expr="'the join transition took ' + _event.name"/></transition>
    <transition event="join.refused"><log expr="'never reached'"/></transition>
  </state>
</scxml>` }]);
const host = new Host(compiled, { system: 'demo', log: (entry) => console.log(entry.text) });
compiled.close();
host.spawn('member').send('join.refused');
await host.idle();
host.close();
```

```output
the join transition took join.refused
```

## Scripts and helpers

- **A helper and a `<data>` of the same name**
  (`tinyactors:tinyactors:1`). On **quickjs** the chart-level
  `function label()` silently replaces the variable `label`: the
  variable reads as the function, its value is gone, and nothing warns.
  On **native** the variable shadows the helper, and calling it is
  `error.execution` (`TypeError: label is not a function`). Give helpers
  and variables different names (helpers as verbs, `formatLabel`).
- **A helper cannot call a helper from another chart-level `<script>`
  on native** (`tinyactors:tinyactors:2`): `ReferenceError` at run
  time; quickjs allows it. Keep a chart's helpers in one `<script>` (or
  one `src` file), or pass the other helper's result in.
- **Top-level `let`, `const` and `class` in a `<script>` are refused on
  native**: the script raises `error.execution` each time it runs, and
  `compileCharts` warns at its line. A top-level `var` is a variable of
  the session (prefer a `<data>`). Inside a function or a block they are
  fine. In a helper file, write constants as functions that return them.
- **A helper sees no session variable by name on native.** Pass it what
  it reads: `price(items)`, not `price()` reading `items`.
- **`Date` in a chart on native reads the process's clock, not the
  host's** (`tinyactors:tinyactors:3`), so a `SimulatedClock` does not
  move it. A chart cannot read the host's clock: give it times as event
  data, or ask your program for the time through an I/O processor.
- **`<send>` has no `cond`.** The attribute is ignored and the send
  always goes out (core's `tinyactors check` warns of it;
  `compileCharts` in 0.4.0 does not). Wrap the send in `<if cond="...">`.

## Values

- **What crosses is copied, and must be in the value table.** A
  function, a `Map`, a `Date` or a class's instance as event data or
  params is refused (`UnsupportedError`). A chart never holds your
  object; changing it after a send changes nothing in the chart.
- **A variable outside the value table keeps its actor in memory** on
  native (and on Python): hibernation and snapshots refuse it, and the
  error names the variable. A DOM from XML data counts too. quickjs
  stores its own values (`Date`, `Map`, `Set`).
- **Sessions are `bigint`s in the API and numbers in typed methods'
  results.** Compare with `Number(actor.id) === listed`; key a `Map` by
  `String(actor.id)`.

## Handlers

- **A handler's argument is valid only while the handler runs.** Reading
  `send.data` after it returned throws `TA_ERROR_STALE`. Copy out what you
  keep, or make `receive` `async`: the host fills the view before it moves
  on, and you may answer later.
- **An answer to an actor that ended is dropped**: `send.answer()` and
  `send.fail()` return `false` (Python: `False`). Check the result when
  it matters (an HTTP response waiting on it).
- **A handler's error ends the process by default** (Node.js, Bun): it
  goes to `onHandlerError`, else rejects a waiting `host.idle()`, else is
  an uncaught exception. Give the host an `onHandlerError` in a server.
- **`actor.done` must be taken while the actor lives.** Read
  `const done = actor.done` before the actor can end; once it ended,
  `done` rejects with `StaleError` and its donedata is gone. `onDone` sees
  every end.
- **`ask` has no timeout.** Pass `{ signal: AbortSignal.timeout(ms) }`
  (Python: `asyncio.wait_for`), or a chart that never answers holds the
  caller for ever.

## Names and missing actors

- **A second spawn under a taken name is not refused.** Within one
  version the oldest holder keeps the name. Give each name one owner, the
  collection actor that spawns it ([designing-actors.md](designing-actors.md)).
- **A send to a name nobody holds** is `error.communication` for a chart
  at once, and a `StaleError` for your program's `host.send`. Pick one
  of the three ways per kind of actor (never end and hibernate; route
  through your processor; handle the error).
- **The events still queued when an actor ends are dropped**, silently.
  A chart whose events stand for requests your program holds tells your
  program from its `<final>` first.

## The process

- **An open host keeps the process alive only while it has a deadline**
  (a delayed send, a timeout). An actor waiting for an answer that has no
  I/O behind it lets the process exit. Keep the process alive with your
  server; give every wait a timeout.
- **A host on the runtime's clock drifts with a re-armed delay**: a
  chart that re-arms `delay="20ms"` each tick falls behind by the turn's
  latency every tick. Give a cadence that must hold as absolute times.
