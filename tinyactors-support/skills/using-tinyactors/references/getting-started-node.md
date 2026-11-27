# Getting started: Node.js, Bun and browsers

`@tinyactors/node` runs on Node.js 22.3 or later and on Bun, on macOS and
Linux (x64 and arm64). `@tinyactors/wasm` is the same API compiled to
WebAssembly, for browsers and for platforms with no prebuilt addon
(Windows); its guide is `docs/guide/browser.md` in `@tinyactors/node`.
This page is written against **0.4.0**, the release on npm; `main` can be
ahead of it, and each release's notes say what changed.

## Install

```sh
npm install @tinyactors/node     # or: pnpm add, bun add
```

Nothing compiles on install: the addon is prebuilt, one per platform, as
an optional dependency. An install with `--omit=optional` fails at import.

## A project

Keep one chart per kind of actor in a `charts/` directory, and compile
the directory once at start-up with `compileDir`. A document's file name
is only for error messages and `src`; you spawn a chart by its
`<scxml name>`.

```
my-app/
  charts/
    ticket.scxml      <scxml name="ticket">
  main.mjs
```

A support ticket: replies go out to the customer by mail, a status request
is answered to whoever asked, and resolving it ends the actor with its
donedata.

```xml charts/ticket.scxml
<scxml xmlns="http://www.w3.org/2005/07/scxml" version="1.0"
       datamodel="ecmascript" name="ticket" initial="open">
  <datamodel>
    <data id="customer"/>
    <data id="replies" expr="0"/>
  </datamodel>
  <state id="open">
    <transition event="reply">
      <assign location="replies" expr="replies + 1"/>
      <send type="mail" event="notify" targetexpr="customer">
        <param name="text" expr="_event.data.text"/>
      </send>
    </transition>
    <transition event="status">
      <send targetexpr="_event.origin" event="current">
        <param name="replies" expr="replies"/>
      </send>
    </transition>
    <transition event="resolve" target="resolved"/>
  </state>
  <final id="resolved">
    <donedata><param name="replies" expr="replies"/></donedata>
  </final>
</scxml>
```

```js main.mjs
import { compileDir, Host } from '@tinyactors/node';

// The I/O processor: the chart's <send type="mail">, your code's I/O.
const mail = {
  receive(send) {
    console.log(`mail to ${send.target}: ${send.data.text}`);
    send.answer('mailed');               // an event back to the ticket; it has no use for it here
  },
};

const compiled = compileDir(new URL('./charts/', import.meta.url));
const host = new Host(compiled, {
  system: 'support',                     // this system's id: tinyactors://support/...
  ioProcessors: { mail },
  onDone: (actor, donedata) => console.log(`${actor} ended with`, donedata),
});
compiled.close();                        // the host copied the image

const ticket = host.spawn('ticket', { name: 'ticket.4711', params: { customer: 'ada@example.com' } });
ticket.send('reply', { text: 'We are on it.' });
console.log(await ticket.ask('status'));  // [event, data] of the chart's answer to _event.origin
ticket.send('resolve');
await ticket.done;
host.close();
```

```output
mail to ada@example.com: We are on it.
[ 'current', { replies: 1 } ]
Actor(1) ended with { replies: 1 }
```

What it shows:

- **`compileDir(url)`** compiles every `*.scxml` under the directory; a
  mistake in the XML throws a `CompileError` with every diagnostic, and
  JavaScript that does not parse is a warning in `compiled.diagnostics`.
- **`new Host(charts, options)`**: `system` is required. `ioProcessors`
  maps each `<send type>` your charts use to an object with
  `receive(send)`; a plain function is a `TypeError`.
- **`host.spawn(chart, { name, params })`** returns an `Actor`. `params`
  set the chart's top-level `<data>`; `name` makes it findable as
  `host.actor('ticket.4711')` and as `tinyactors://support/@ticket.4711`.
- **`actor.ask(event, data)`** resolves with the chart's answer to
  `_event.origin`, as `[event, data]`. It has no timeout of its own: pass
  `{ signal: AbortSignal.timeout(5000) }`.
- **`actor.done`** and the host's **`onDone`** give an actor's donedata
  when it reaches a top-level `<final>`; **`onError(actor, error)`**
  reports one that faulted.
- The host runs on the event loop by itself: there is no `run()`. Inputs
  are taken at once and run in a later turn. **`await host.idle()`**
  waits until nothing is left to do; tests and scripts use it, a server
  does not need it.

## The I/O processor's side

`receive(send)` gets a **view** of the send, valid while it runs: read
`send.event`, `send.data`, `send.target`, `send.sendid`, `send.sender`
there. Then do one of:

- `send.answer(event, data)`: an event back to the sender, with the
  send's `sendid`;
- `send.fail()`: the sender gets `error.communication`;
- nothing, when the send needs no answer.

To answer after I/O, make `receive` `async` (or return a promise): the
host copies every field before it moves on, and the answer arrives later
as an ordinary event. A `receive` that throws, or whose promise rejects
before answering, fails the send and reports the error to
`onHandlerError` (else to `host.idle()`, else as an uncaught exception,
which ends the process).

```js
import { compileCharts, Host } from '@tinyactors/node';

const compiled = compileCharts([{ name: 'order.scxml', text: `
<scxml xmlns="http://www.w3.org/2005/07/scxml" version="1.0"
       datamodel="ecmascript" name="order" initial="charging">
  <state id="charging">
    <onentry>
      <send type="payments" event="charge" id="charge"><param name="amount" expr="4200"/></send>
      <send event="timeout" delay="30s" id="timeout"/>
    </onentry>
    <transition event="charged" target="paid"><cancel sendid="timeout"/></transition>
    <transition event="error.communication" target="failed"><cancel sendid="timeout"/></transition>
    <transition event="timeout" target="failed"/>
  </state>
  <final id="paid"><onentry><log expr="'paid, answer to ' + _event.sendid"/></onentry></final>
  <final id="failed"><onentry><log expr="'failed: ' + _event.name"/></onentry></final>
</scxml>` }], { warningsAsErrors: true });

const payments = {
  async receive(send) {                       // async: it may answer after I/O
    const { amount } = send.data;             // read what you need; the view is filled in for you
    await new Promise((resolve) => setTimeout(resolve, 10));   // the payment provider
    if (amount > 0) send.answer('charged', { amount });
    else send.fail();
  },
};
const host = new Host(compiled, { system: 'shop', ioProcessors: { payments }, log: (entry) => console.log(entry.text) });
compiled.close();
host.spawn('order');
await host.idle();
host.close();
```

```output
paid, answer to charge
```

## Errors, where they show

- **At compile time:** `CompileError`, with `error.diagnostics`.
- **At a call:** a `TinyactorsError` subclass: `StaleError` for an actor
  that ended or never was, `StateError` for a call the host cannot make
  now, `InvalidError` for an argument it refuses (a chart it does not
  have), `UnsupportedError` for a value outside the value table.
- **In a chart:** the events `error.execution` (its own code failed) and
  `error.communication` (a send or a service failed). They are events,
  not exceptions; without a transition for them they are dropped.
- **In your handlers:** `onHandlerError(error, effect)`, else the
  turn's error.

```js
import { compileCharts, CompileError, Host, StaleError } from '@tinyactors/node';

try {
  compileCharts([{ name: 'bad.scxml', text: `<scxml xmlns="http://www.w3.org/2005/07/scxml" version="1.0"
      datamodel="ecmascript" name="bad"><state id="s"><transition event="go" target="nowhere"/></state></scxml>` }]);
} catch (error) {
  if (!(error instanceof CompileError)) throw error;
  console.log(error.message);
}

const compiled = compileCharts([{ name: 'quote.scxml', text: `<scxml xmlns="http://www.w3.org/2005/07/scxml" version="1.0"
    datamodel="ecmascript" name="quote" initial="s"><state id="s"><transition event="accept" target="done"/></state><final id="done"/></scxml>` }]);
const host = new Host(compiled, { system: 'shop' });
compiled.close();
const quote = host.spawn('quote', { name: 'quote.1' });
quote.send('accept');
await host.idle();
try {
  host.actor('quote.1');
} catch (error) {
  if (!(error instanceof StaleError)) throw error;
  console.log('quote.1 has ended:', error.status);
}
host.close();
```

```output
bad.scxml:2:55: error: unknown state id 'nowhere'
quote.1 has ended: TA_ERROR_STALE
```

## Values

What crosses between your program and a chart (event data, spawn params,
answers) is copied, and must be in the value table: `undefined`, `null`,
booleans, numbers, `bigint`s up to 64 bits, strings, `Uint8Array`s,
arrays and plain objects. A function, a `Map`, a `Date` or a class's
instance is refused with `UnsupportedError`. Sessions are `bigint`s in
the API (`actor.id`) and numbers in the typed methods' results: key a
`Map` by `String(actor.id)`.

## Time

Time in the API is milliseconds. A delay in a chart is written with its
unit, `30s`, `1.5s` or `500ms`; `delayexpr` must give such a string
(`delayexpr="minutes * 60 + 's'"`). The host reads the runtime's clock;
a test gives it a `SimulatedClock` instead ([testing.md](testing.md)).

## Where next

- [designing-actors.md](designing-actors.md) before the second chart.
- [choices.md](choices.md) for the datamodel, persistence, services and
  other systems.
- The guides in `node_modules/@tinyactors/node/docs/guide/`:
  `charts.md` (scripts, XML data, `<invoke>`), `io-processors.md`,
  `missing-actors.md`, `services.md`, `hibernation.md`, `snapshots.md`,
  `migration.md`, `other-systems.md`, `telemetry.md`, `testing.md`,
  `quickjs.md`, `browser.md`.
