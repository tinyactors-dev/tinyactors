# @tinyactors/node

Statecharts as actors, for Node.js and Bun. Each actor is a state machine
with SCXML semantics (nested and parallel states, history, eventless and
delayed transitions, invoked children) that runs on a native runtime and
talks to other actors by message. Use it when you have many long-lived
things with rules about what may happen next: orders, subscriptions,
devices, sessions, workflows.

- **Many actors, cheaply.** An actor costs a few hundred bytes of native
  memory, so hundreds of thousands fit in one process. A quiet actor can be
  unloaded from memory and comes back when mail arrives for its name.
- **Charts you can read and test.** Write them with a fluent JavaScript
  builder or as SCXML documents. A manual clock makes timers deterministic
  in tests, and `inspect()` shows exactly where an actor is.
- **Observable in production.** Every step can be traced as OpenTelemetry
  spans and counted as metrics, with no telemetry dependency.

## Install

```text
npm install @tinyactors/node
bun add @tinyactors/node
```

The install brings a prebuilt native addon for your platform: no compiler,
no `node-gyp`, no postinstall script. Supported: macOS 11 or newer on Apple
silicon and Intel, and Linux on x64 and arm64, with glibc or musl
(Alpine). It needs Node.js 22.14 or newer, or Bun.

## Quick start

```js
const { createSystem, statechart, named } = require('@tinyactors/node');

const light = statechart({ family: 'light', revision: 'v1' })
  .data('switches', 0)
  .state('off', (s) => s.on('toggle', (t) => t.when(named('belowLimit')).target('on').assign('switches', named('counted'))))
  .state('on', (s) => s.on('toggle', (t) => t.target('off')));

async function main() {
  const system = createSystem();
  system.registerFunction('belowLimit', function () { return this.switches < 3; });
  system.registerFunction('counted', function () { return this.switches + 1; });
  const lamp = system.spawn(system.define(light), { name: 'lamp' });
  const result = await lamp.send('toggle'); // resolves once the step it started has ended
  console.log(result.status, lamp.inspect().activeStates, lamp.data().switches); // stable [ 'on' ] 1
  system.close();
}

main();
```

A **system** runs actors. `define()` compiles a chart once; `spawn()` starts
an actor of it. The first state is the initial one, and `data()` declares
the actor's data. A chart's `family` and `revision` name it, so a new
revision can replace a running actor's chart later.

**Write guards and actions as named functions.** Register a function once
with `system.registerFunction(name, fn)` and call it from a chart with
`named(name)`: a guard is `t.when(named('belowLimit'))`, an action
`t.script(named('audit'))`, a computed value `assign('switches',
named('counted'))`. The function runs with `this` bound to the actor's
data and gets the context (the event, `In()`, the system) as its argument.
Named functions are the preferred way to write a chart:

- the chart can be written as SCXML (`cond="belowLimit()"`) or another
  syntax and read back, in this process or another that registers the same
  names;
- a misspelled name is reported when the chart is defined, not when it
  runs;
- the same chart built twice is one definition;
- the functions live with the system, where they can be tested and reused.

A function written inline, `t.when(function () { return this.switches < 3; })`,
works too and is quickest for a one-off. Building such a chart again with
new closures makes another definition, and the built-in data model cannot
write it as text. An evaluating data model can: the example ECMAScript one
(`tools/ecmascript-datamodel.js`) writes it with
`encode('scxml', { datamodel: 'ecmascript' })`, which is faithful only when
the function uses nothing but `this` and its argument.

**Values stay yours, and can be written as text.** A value in a chart is
the object you passed. When the chart is written as text, the built-in data
model writes JSON, and `new Name(arguments)` for `Date`, `Map`, `Set`,
`RegExp`, typed arrays, and your own classes when they describe themselves:

```js
const { serialize } = require('@tinyactors/node');

class Money {
  constructor(amount, currency) { this.amount = amount; this.currency = currency; }
  [serialize]() { return { name: 'Money', arguments: [this.amount, this.currency] }; }
}
system.registerClass('Money', Money); // wherever the text is read
```

The text makes new values when it runs; it is a copy, not the object.

## Charts from SCXML

Charts can also be SCXML documents. `<scxml name>` becomes the family:

```js
const { createSystem } = require('@tinyactors/node');

const door = `
<scxml xmlns="http://www.w3.org/2005/07/scxml" version="1.0" name="door" initial="closed">
  <state id="closed">
    <transition event="open" target="opened"/>
  </state>
  <state id="opened">
    <transition event="close" target="closed"/>
  </state>
</scxml>`;

async function main() {
  const system = createSystem();
  const definition = system.define(door, { format: 'scxml', name: 'door.scxml' });
  const actor = system.spawn(definition, { name: 'front' });
  await actor.send('open');
  console.log(actor.inspect().activeStates); // [ 'opened' ]
  system.close();
}

main();
```

tinyactors ships no expression language: running expression text means
running code with your process's rights. Without one, expressions in a
document may be `In(state)`, data names, JSON values, `name()`, a call
of a function registered with `registerFunction()`, and `new Name(...)`, a
class registered with `registerClass()`. `system.registerDatamodel(name,
{ compile(source, role) })` plugs in a compiler of your choice, and
documents that name it in their `datamodel` attribute use it.

## Named actors, send and post

An actor spawned with a `name` has an address, `{ family, name }`. Mail can
go to the address, and charts send to other actors by URI:

```js
const { createSystem, statechart } = require('@tinyactors/node');

const counter = statechart({ family: 'counter', revision: 'v1' })
  .data('count', 0)
  .state('counting', (s) => s.on('add', (t) => t.assign('count', function ({ event }) { return this.count + event.data; })));

const reporter = statechart({ family: 'reporter', revision: 'v1' })
  .state('idle', (s) => s.on('report', (t) => t.send('add', (send) => send.to('#_actor_counter/visits').data(1))));

async function main() {
  const system = createSystem();
  system.spawn(system.define(counter), { name: 'visits' });
  const page = system.spawn(system.define(reporter), { name: 'home' });

  system.post({ family: 'counter', name: 'visits' }, 'add', 10); // queue and return
  await system.send(page, 'report');                             // resolves when that step ends
  await system.flush();                                          // let the mail it sent arrive

  const visits = system.findActor({ family: 'counter', name: 'visits' });
  console.log(visits.data().count); // 11
  system.close();
}

main();
```

`post()` queues a message and returns. `send()` returns a promise that
resolves when the step the message started has ended (or, with `{ until:
'accepted' }`, when it is queued), and rejects if the message is lost or
the step fails; it takes a `timeout` and an `AbortSignal`. Messages to one
actor arrive in the order they were sent.

A chart targets a named actor by `#_actor_<family>/<name>`, each part
percent-encoded, so a name may hold `/` or `%`. `actorURI()` writes it with
core's encoder: `actorURI({ family: 'orders', name: 'eu/42' })` is
`'#_actor_orders/eu%2F42'`, and that is also the `event.origin` of the
actor's messages (core ADR 0096).

Data a chart sends is a copy (SCXML C.1, napi ADR 0039): after
`send('add', (send) => send.data(function () { return this.cart; }))`,
changing `this.cart` does not change what the receiver got, and the
receiver changing its event data does not change the sender's. The same
holds for an I/O processor's `request.data`, an invoked child's params,
and autoforwarded events. Plain objects, arrays, `Date`, `Map`, `Set`,
`RegExp`, typed arrays, `Buffer` and values that describe themselves with
`[serialize]()` (their class registered with `registerClass()`) are
copied; functions and other class instances are shared, as before.
`raise()` and mail you `post()` or `send()` from JavaScript are not
copied: they do not leave a session.

## Timers

A delayed send is a timer that belongs to the actor. With a manual clock,
tests move time themselves:

```js
const { createSystem, statechart } = require('@tinyactors/node');

const session = statechart({ family: 'session', revision: 'v1' })
  .state('active', (s) => s
    .entry((e) => e.send('expire', (send) => send.id('timeout').after('30s')))
    .exit((e) => e.cancel('timeout'))
    .on('touch', (t) => t.target('active'))   // leaving and re-entering re-arms the timer
    .on('expire', (t) => t.target('expired')))
  .final('expired');

async function main() {
  // A manual clock moves only when you advance it: timers are testable.
  const system = createSystem({ clock: 'manual', finished: 'retain' });
  const user = system.spawn(system.define(session), { name: 'alice' });
  await system.flush();

  await system.advanceBy(20_000);
  await user.send('touch');
  console.log(user.inspect().delayedSends.map((send) => [send.id, send.due])); // [ [ 'timeout', 50000 ] ]

  await system.advanceBy(30_000);
  console.log(user.inspect().finalState); // expired
  system.close();
}

main();
```

The default clock is real time, and an idle system's timer does not keep
the process alive. Neither does a pending `send()`, nor its `timeout`: a
script that waits for a timeout keeps the process alive itself.

## Unloading and virtual actors

```js
const { createSystem, statechart } = require('@tinyactors/node');

const cart = statechart({ family: 'cart', revision: 'v1' })
  .data('items', [])
  .state('open', (s) => s.on('add', (t) => t.assign('items', function ({ event }) { return [...this.items, event.data]; })));

// Where your application keeps actors' data: a database, in real life.
const store = new Map();

async function main() {
  const system = createSystem();
  const definition = system.define(cart);

  // Unload a quiet actor to free its memory; mail to its address brings it back.
  const bob = system.spawn(definition, { name: 'bob' });
  await bob.send('add', 'apples');
  system.unload(bob);
  await system.send({ family: 'cart', name: 'bob' }, 'add', 'pears');
  console.log(system.findActor({ family: 'cart', name: 'bob' }).data().items); // [ 'apples', 'pears' ]

  // A loader makes a family virtual: an actor is spawned from stored data
  // the first time mail arrives for its name, after a restart for example.
  store.set('carol', { items: ['milk'] });
  system.setLoader('cart', ({ name }) => (store.has(name) ? { spawn: definition, binding: store.get(name) } : 'not-found'));
  await system.send({ family: 'cart', name: 'carol' }, 'add', 'bread');
  console.log(system.findActor({ family: 'cart', name: 'carol' }).data().items); // [ 'milk', 'bread' ]
  system.close();
}

main();
```

Mail that arrives while a loader runs waits and is delivered in order once
the actor is back. An unloaded actor's own timers wake it too. To survive a
restart, store an actor's `data()` and `inspect().delayedSends`, and spawn
it again with `{ binding, pendingSends }`.

## Persistence by replay

A named actor can live on as the journal of the events it took: record
them, evict it, and rebuild it from the journal when it is needed again
(kernel ADR 0006). `system.setRecorder()` hands your recorder, once per
turn, the events the actors of the families you name took, as their charts
saw them (`ActorEvent`s), each with the time it was taken, and when each
address must be woken. You journal them as you like; to rebuild an actor,
you replay its journal into an actor that crosses nothing until it is
published. Storage is yours: two tables are enough, a journal of events
per address and a wake per address.

```js
const { createSystem, statechart } = require('@tinyactors/node');

const counter = statechart({ family: 'counter', revision: 'v1' })
  .data('n', 0)
  .state('s', (s) => s.on('inc', (t) => t.assign('n', ({ data }) => data.n + 1)));

// Your store: two database tables in real life.
const journals = new Map(); // address -> its events, { event, at }, in order
const wakes = new Map();    // address -> when tinyactors needs the actor back
const key = ({ family, name }) => `${family}/${name}`;

async function main() {
  const system = createSystem();
  const definition = system.define(counter);

  // Journal each batch, serializing the events during the call.
  system.setRecorder(({ records, wakes: changed }) => {
    for (const { address, event, at } of records) {
      journals.set(key(address), [...(journals.get(key(address)) ?? []), JSON.parse(JSON.stringify({ event, at }))]);
    }
    for (const { address, at } of changed) at === null ? wakes.delete(key(address)) : wakes.set(key(address), { address, at });
  }, { families: ['counter'] });

  // The whole loader: an address's journal, replayed into an actor whose life began at 0.
  system.setLoader('counter', (address) => ({ replay: { spawn: definition, at: 0, events: journals.get(key(address)) ?? [] } }));

  // After a restart: bring back every actor that has a timer due.
  for (const { address, at } of wakes.values()) system.wakeAt(address, at);

  const c = system.spawn(definition, { name: 'c' });
  await c.send('inc');
  await c.send('inc');
  system.evict(c); // its journal is now its only state
  await system.flush();
  await system.send({ family: 'counter', name: 'c' }, 'inc'); // replayed, then takes the new mail
  console.log(system.findActor({ family: 'counter', name: 'c' }).data().n); // 3
  system.close();
}

main();
```

Rebuilding by hand is three calls: `system.prepare(definition, { address,
at })`, `prepared.replay(event, at)` for each journaled event, then
`system.publish(prepared)`. A replay crosses no border: it sends nothing,
starts no service and writes no log. A timeout the actor sent itself is in
its journal, so the rebuild reproduces it exactly; one still pending fires
once, late if its time has passed, and an evicted actor's wake brings it
back when its next timer is due. An event is recorded once the macrostep it
started has ended, and the turn's batch reaches your recorder before the
turn's `send()` promises settle: a caller that needs a message durable
awaits `send()`, then your journal's write.

Your journal can hold any `ActorEvent`s, not only what the recorder hands
you: domain facts a chart sends to a journal I/O processor, a fixture in a
test, an export from another system. To upgrade, replay the journal into
the new revision: `system.prepare(v2, { incumbent, at })`, then `replay`
and `publish`.

## Crashes and repeated messages

What a crash costs is explicit (kernel ADR 0006 section 5). Events your
recorder was handed but did not store yet are lost from the journal, and
an event is recorded only once the macrostep it started has ended, so a
crash before then leaves it out. Mail a receiver had not taken is in no
journal. Recovery is end to end: a sender that needs an answer times out
and retries, and a client retries its request. So delivery is **at least
once where someone retries**, and at most once where nothing does. A
rebuilt actor may also repeat once a delayed send to another actor or to
an I/O processor that fired after the last event it took (see below). A
repeat carries the same `origin`, and the same `sendid` when the `<send>`
has an `id`.

If a repeat would hurt:

- **Loop with events rather than sending from one long macrostep.** The
  chart sends itself `next` per item, so each item is a short macrostep of
  its own, recorded when it ends: a rebuild after a crash resumes at the
  item it had reached, and a retry repeats at most one item.
- **Make receivers idempotent on business keys**, such as a reservation
  keyed by (order, SKU). That also absorbs a client's HTTP retries.
- **Guard with states:** an event with no transition in the receiver's
  current state is ignored.
- **Give outward effects an idempotency key** built from the business
  key, for example `Idempotency-Key: order-42-charge`.
- **A delayed effect that must happen once: a timeout to itself, then the
  send.** An actor rebuilt from its journal of recorded events
  (`setRecorder()`, kernel ADR 0006) may repeat once a delayed send to
  another actor or to an I/O processor that fired before it took its last
  event: that firing is not one of its events. A timeout it sends itself
  is: replay rebuilds it exactly, and the send made in its handler happens
  once.

```js
// Not: .send('remind', (send) => send.to('#_actor_mailer/main').after(60_000))
const reminder = statechart({ family: 'reminder', revision: 'v1' })
  .state('idle', (s) => s.on('arm', (t) => t.target('armed')
    .send('due', (send) => send.after(60_000))))         // a timeout to itself: in its own journal
  .state('armed', (s) => s.on('due', (t) => t.target('sent')
    .send('remind', (send) => send.to('#_actor_mailer/main'))))
  .state('sent');
```

```js
const { statechart } = require('@tinyactors/node');

// One item per macrostep: a rebuild resumes at the item it had reached.
const order = statechart({ family: 'order', revision: 'v1' })
  .data('order', null)
  .data('items', [])
  .data('next', 0)
  .state('idle', (s) => s.on('place', (t) => t
    .assign('order', ({ event }) => event.data.order)
    .assign('items', ({ event }) => event.data.items)
    .target('reserving')))
  .state('reserving', (s) => s
    .entry((e) => e.send('step'))             // no target: to itself, a macrostep of its own
    .on('step', (t) => t
      .when(function () { return this.next < this.items.length; })
      .send('reserve', (send) => send.to('#_actor_stock/main').data(function () {
        return { order: this.order, sku: this.items[this.next] }; // the business key
      }))
      .assign('next', function () { return this.next + 1; })
      .send('step'))                            // the next item, in the next macrostep
    .on('step', (t) => t.target('placed')))
  .state('placed');
```

## Invoking child charts

```js
const { createSystem, statechart } = require('@tinyactors/node');

async function main() {
  const system = createSystem();

  const download = system.define(statechart({ family: 'download', revision: 'v1' })
    .data('url', null)
    .state('fetching', (s) => s.on('fetched', (t) => t.target('done')))
    .final('done', (f) => f.doneData((d) => d.content(({ data }) => `saved ${data.url}`))));

  const job = system.define(statechart({ family: 'job', revision: 'v1' })
    .data('result', null)
    .state('working', (s) => s
      // The child runs as its own actor while `working` is active, and is
      // stopped if the parent leaves the state first.
      .invoke((i) => i.type('scxml').id('dl').content(download).param('url', 'https://example.com/a.zip'))
      .on('progress', (t) => t.send('fetched', (send) => send.to('#_dl')))
      .on('done.invoke.dl', (t) => t.target('finished').assign('result', ({ event }) => event.data)))
    .state('finished'));

  const actor = system.spawn(job);
  await system.flush();
  await actor.send('progress');
  await system.flush();
  console.log(actor.data().result); // saved https://example.com/a.zip
  system.close();
}

main();
```

An invoker registered with `createSystem({ invokers })` runs any other kind
of service the same way: it is started when the state is entered, reports
events and a result, and is cancelled when the state is left.

## Talking to the outside world

A chart's `send` with `via(type)` goes to an I/O processor you register.
Replies come back as ordinary events:

```js
const { createSystem, statechart } = require('@tinyactors/node');

// An I/O processor is how a chart's <send> reaches the outside world. This
// one pretends to call an HTTP API and posts the answer back to the sender.
const http = {
  async send(request, context) {
    const reply = { ok: true, echo: request.data };           // your fetch() goes here
    context.post(request.source, 'http.response', reply);
  },
};

const order = statechart({ family: 'order', revision: 'v1' })
  .data('reply', null)
  .state('new', (s) => s.on('submit', (t) => t.target('waiting')
    .send('charge', (send) => send.via('http').to('https://payments.example/charge').data(({ event }) => event.data))))
  .state('waiting', (s) => s.on('http.response', (t) => t.target('paid').assign('reply', ({ event }) => event.data)))
  .state('paid');

async function main() {
  const system = createSystem({ io: { http } });
  const actor = system.spawn(system.define(order), { name: 'o-1' });
  await actor.send('submit', { amount: 42 });
  await system.flush();
  console.log(actor.inspect().activeStates, actor.data().reply); // [ 'paid' ] { ok: true, echo: { amount: 42 } }
  system.close();
}

main();
```

A processor that throws, or whose promise rejects, fails the send: the
chart gets `error.communication`. Events you post from outside can carry an
`origin`, so a chart can answer a WebSocket connection or an HTTP request
with `send('reply', (send) => send.toOrigin())`: the target is the event's
`origin` and the processor its `originType`. The same reply answers
another actor or an invoked service.

A chart that asks for effects by name can say so with `effect(name, data?,
build?)`: it is `send(name)` through the processor registered as
`effects` (exported as `EFFECTS`), so the processor dispatches on
`request.event` and a test swaps in a recording one:

```js
const effects = {
  send(request, context) {
    const run = { charge: chargeCard, 'email.receipt': sendReceipt }[request.event];
    return run(request.data, context);
  },
};

const checkout = statechart({ family: 'checkout', revision: 'v1' })
  .state('cart', (s) => s.on('pay', (t) => t.target('paying')
    .effect('charge', ({ event }) => ({ amount: event.data.total }))
    .effect('email.receipt', undefined, (send) => send.after('1s'))));

const system = createSystem({ io: { effects } });
```

The third argument is the send's block: `send.via('mail')` sends one
effect through another processor. An exported chart shows a plain `send`
(napi ADR 0038).

## Tracing and metrics

```js
const { createSystem, statechart, decodeTraces, metricsToOTLP } = require('@tinyactors/node');

async function main() {
  const system = createSystem();
  // Once per pump turn, the spans that ended, as OTLP protobuf bytes: POST
  // them to any OTLP/HTTP collector's /v1/traces. Here we decode them.
  system.setTraceSink((traces) => {
    for (const span of decodeTraces(traces)) console.log(span.name);
  }, { resource: { 'service.name': 'shop' } });

  const light = system.define(statechart({ family: 'light', revision: 'v1' })
    .state('off', (s) => s.on('toggle', (t) => t.target('on')))
    .state('on'));
  const lamp = system.spawn(light, { name: 'lamp' });
  await lamp.send('toggle');
  await system.flush();
  // scxml.spawned, scxml.macrostep bootstrap, scxml.macrostep toggle

  // Counters and gauges as plain data, or as OTLP bytes for /v1/metrics.
  const snapshot = system.metrics();
  console.log(snapshot.metrics.map((metric) => metric.name).slice(0, 3));
  console.log(metricsToOTLP(snapshot, { resource: { 'service.name': 'shop' } }).byteLength > 0);
  system.close();
}

main();
```

Spans cover each step an actor takes, with detail levels from one span per
step to every transition, condition and assignment. A W3C `traceparent`
passed to `post()`, `send()` or `spawn()` puts the steps it causes into the
caller's trace. `watchTraces()` traces chosen actors in full whatever the
sampling ratio, and failures are also written as OTLP log records.

## System parameters

A system's limits and budgets are parameters, set once when it is created
and never changed (core ADR 0092, napi ADR 0035). Every one has a default
and bounds; an unknown name or a value out of bounds throws `invalid`.

```js
const { createSystem, parameterInfo } = require('@tinyactors/node');

// Event payloads of up to 16 KiB arrive whole in traces (`values: true`);
// the default cuts them at 1024 bytes and marks them `.truncated`.
const system = createSystem({ parameters: { valueBytes: 16384, maxQueuedMessages: 10_000 } });
console.log(system.parameters.valueBytes);       // 16384
console.log(parameterInfo().interpreterBytes);        // { default, min, max, unit, owner }
system.close();
```

They include core's (`microstepLimit`, `interpreterBytes`, `renderBytes`,
`maxActors`, ...), the host kernel's (`valueBytes`, `traceStagingBytes`,
the scheduling budgets, the mailbox limits) and napi's own
(`registeredDatamodels`, `datamodelNameBytes`); `owner` says which. Every trace request's resource
carries them as one attribute, `tinyactors.params`: the defaults' version,
then each parameter that differs from its default, in name order
(`v1;valueBytes=16384`), or `v1` alone, so a trace says which caps applied
(napi ADR 0036).

## TypeScript

Types are included (`index.d.ts`). `statechart<Data>()` types an actor's
data, so `assign()`, `data()` and `inspect().data` are checked against it.

`statechart<Data, Events>()` also takes the events the chart handles, as a
map from name to data (an interface or a type):

```ts
interface OrderEvents {
  'order.placed': { id: string; total: number };
  'order.cancelled': { id: string };
  tick: undefined;
}

statechart<{ total: number }, OrderEvents>({ family: 'orders', revision: 'v1' })
  .data('total', 0)
  .state('open', (s) => s
    .on('order.placed', (t) => t.assign('total', function ({ event }) {
      return this.total + (event?.data.total ?? 0); // event.data is { id, total }
    }))
    .on('order', (t) => t.raise('tick'))            // order.placed and order.cancelled
    .on('done.invoke.payment error.*')              // platform events: data unknown
    // .on('order.shipped')                         // type error: no such event
    // .raise('order.cancelled')                    // type error: its data is missing
  );
```

- `on()` takes an SCXML event descriptor: a space-separated list, each item
  a name that also matches the names it prefixes on a `.` boundary
  (`order` and `order.*` match `order.placed`), or `*`. An item that
  matches no event of the map is a type error. Descriptors of platform
  events (`done.state.<id>`, `done.invoke.<id>`, `error.*`, ...) are always
  accepted; list an invoked child's own events in the map to type them.
- In that transition's expressions, `context.event` is a union of the
  matched events, so `event.name` narrows `event.data`.
- `raise()` takes a name of the map, and requires its data unless the type
  admits `undefined`. `send()` accepts any name, since it may go to another
  actor, and checks `data()` for a name of the map.
- Without an event map every name is accepted and `event.data` is
  `unknown`. The map is types only: nothing changes at run time.

## Development builds

Builds of unreleased commits are published under the `next` dist-tag, so a
plain install never picks one up:

```text
npm install @tinyactors/node@next
```

The package is 0.x: its API may change in minor versions.

## License

MIT

Working on tinyactors itself? See [DEVELOPMENT.md](DEVELOPMENT.md).
