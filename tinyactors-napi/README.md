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
  spans and counted as metrics, with no telemetry dependency, and a Debug
  Adapter Protocol session lets a debugger watch a live actor without
  stopping it.

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
const { createSystem, statechart } = require('@tinyactors/node');

const light = statechart({ family: 'light', revision: 'v1' })
  .data('switches', 0)
  .state('off', (s) => s.on('toggle', (t) => t.target('on').assign('switches', function () { return this.switches + 1; })))
  .state('on', (s) => s.on('toggle', (t) => t.target('off')));

async function main() {
  const system = createSystem();
  const lamp = system.spawn(system.define(light), { name: 'lamp' });
  const result = await lamp.send('toggle'); // resolves once the step it started has ended
  console.log(result.status, lamp.inspect().activeStates, lamp.data().switches); // stable [ 'on' ] 1
  system.close();
}

main();
```

A **system** runs actors. `define()` compiles a chart once; `spawn()` starts
an actor of it. The first state is the initial one, `data()` declares the
actor's data, and functions in a chart run with `this` bound to that data.
A chart's `family` and `revision` name it, so a new revision can replace a
running actor's chart later.

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
document may be `In(state)` and data names. `system.registerDatamodel(name,
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
the process alive.

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
with `send(...).via(({ event }) => event.originType).to(({ event }) =>
event.origin)`.

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

## Debugging a live actor

`createDebugSession()` speaks the Debug Adapter Protocol, so a debugger
such as nvim-dap or Emacs dape (or VS Code, through a debug extension) can
attach to one actor of a running system. It shows the actor's
configuration, data, queues and timers, and breakpoints act as
tracepoints: the actor is watched, never stopped.

```js
const net = require('node:net');
const { createSystem, statechart, createDebugSession, DAPReader, encodeDAPMessage } = require('@tinyactors/node');

const system = createSystem();
const light = system.define(statechart({ family: 'light', revision: 'v1' })
  .state('off', (s) => s.on('toggle', (t) => t.target('on')))
  .state('on', (s) => s.on('toggle', (t) => t.target('off'))));
system.spawn(light, { name: 'lamp' });

// One Debug Adapter Protocol session per connection. Put this behind your own
// authentication: `allow` decides what the client may do to the live actor.
const server = net.createServer((socket) => {
  const session = createDebugSession(system, {
    allow: ['send'],                                   // watch, and send events; no code execution
    audit: (entry) => console.error('debug:', entry.type),
    send: (message) => socket.write(encodeDAPMessage(message)),
  });
  const reader = new DAPReader((message) => session.receive(message));
  socket.on('data', (chunk) => {
    try { reader.push(chunk); } catch { socket.destroy(); }
  });
  socket.on('close', () => session.close());
});
server.listen(4711, '127.0.0.1');
```

A client attaches with `{ "address": "light/lamp" }`. The session opens no
socket and checks no credentials itself. What a client may do is up to
`allow`: `send` events, `set` data, `goto` (operator transitions) and
`evaluate`. `evaluate` runs JavaScript in your process, so grant it only
to someone who may run code on the host. `audit` hears every action first,
and can refuse it by throwing.

## TypeScript

Types are included (`index.d.ts`). `statechart<Data>()` types an actor's
data, so `assign()`, `data()` and `inspect().data` are checked against it.

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
