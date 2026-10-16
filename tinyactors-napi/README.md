# @tinyactors/node

**Status: being rewritten** from core's redesigned API ([ROADMAP.md](ROADMAP.md)).
The last published version, 0.2.0, was built from an earlier binding.

## Before you use it

- **Chart code is trusted code.** On the native datamodel (the default)
  a chart's expressions and scripts run in `node:vm` contexts of your own
  engine, and a `vm` context is not a security boundary. Run charts you do
  not trust on quickjs, the sandbox: `new Host(config, { datamodel:
  'quickjs', quickjs: { maxOps, memoryLimit } })`.
- **An open host with a pending deadline keeps the process alive** (a
  delayed `<send>`, a timeout): the host arms one timer at its next
  deadline, as `setTimeout` does. `host.close()` clears it, and the process
  can exit.
- **A handler's error ends the process by default.** An error thrown by
  a handler (an I/O processor's, the log handler, a typed method's)
  rejects a waiting `host.idle()`; with none waiting it is thrown from the
  host's turn as an uncaught exception, and a handler's promise that
  rejects is an unhandled rejection. Either ends a Node.js or Bun process
  unless the application handles `uncaughtException` and
  `unhandledRejection`.
- **Telemetry is the application's to send.** With `telemetry` in the
  config and a `telemetry` handler in the options, the handler gets each
  OTLP traces or logs request as a `Buffer` (protobuf, for a collector's
  `/v1/traces` or `/v1/logs`); `host.metrics()` gives the metrics request
  when you ask. The handler runs in the host's turn, so post the bytes
  from a promise rather than wait for the collector there. Only a turn's
  first error is reported (to `idle()` or as uncaught); the others of that
  turn are lost. `close()` hands the last take (the spans still open,
  ended, and what is staged) to the handler before it frees the host, and
  `[Symbol.asyncDispose]` also awaits the promises the handler returned
  there. A `Host` the collector frees without `close()` skips the last
  take: close the hosts whose telemetry you want.
- **`using host = new Host(...)` needs a runtime that parses `using`:**
  Bun 1.4 does, Node.js 22 does not. `Symbol.dispose` and
  `Symbol.asyncDispose` call `close()`, which works everywhere.
- **A `<script>` that declares top-level names gives each session a
  context of its own.** A chart whose scripts declare `var`, `function`,
  `let`, `const` or `class` at their top level runs each of its sessions
  in a `vm` context of its own (about 143 KB a session on Node.js, 60 KB
  on Bun), so the script's functions close over its own variables. A
  chart that declares nothing shares one context per host: declare the
  function as a `<data>` and assign it in the script
  (`<data id="price"/>`, `price = (sku) => ...`).
  A function held in a variable keeps its session resident (below,
  "Hibernation"); a declaring chart's function declarations do not.

## Hibernation

Most actors spend most of their time waiting. Hibernation lets a host
keep millions of actors while only some of them are in memory: the rest
are blobs in your store, and the host asks for one back when it has work.
The host does no I/O, so storing and loading the blobs is your program's
job.

- **`host.sweep(maxScan, limit) → bigint[]`** returns at most `limit`
  actors that have not run since the previous sweep and can be hibernated
  now (a CLOCK sweep over at most `maxScan` slots). Calling it regularly
  is what makes "not run since the previous sweep" mean cold.
- **`host.hibernate(session) → Buffer`** takes the actor out of memory
  and returns its blob for you to store. It throws a `TinyactorsError`
  when the actor is not at rest (`TA_ERROR_STATE`: events waiting, in a
  macrostep, an invoked child), or when a variable holds what its
  datamodel cannot store (`TA_ERROR_UNSUPPORTED`, the message names the
  variable). The actor stays resident then. A pending delayed send stays
  in the host and wakes the actor when it falls due.
- **`wake: (host, session, migration) => ...`** is called when a
  hibernated actor has work: an event or a due delayed send (`migration`
  is `undefined`), or a migration that asks for it (`migration` is its
  id). The events wait in the mailbox while your handler loads the blob
  and calls **`host.wake(session, blob)`**, then or later (an `async`
  handler is fine; its promise is not awaited). The handler is called once
  per hibernation, however many events come. Without a `wake` handler the
  host does not ask: the actor stays hibernated, and its events wait,
  until you call `host.wake`.

On the native datamodel a blob holds the session's variables and its
`_event` as CBOR, through the same value table as `encode`: numbers,
bigints, strings, `Uint8Array`s, arrays, plain objects, `true`, `false`,
`null` and `undefined`. A function, a `Map`, a `Date`, a class's instance
or an XML document in a variable keeps the session resident. A hibernated
native session leaves nothing in the engine's heap; the payloads of its
pending delayed sends and of the events waiting in its mailbox stay in the
host. A chart whose scripts declare top-level names gets a new context at
the wake: its `var`s come back from the blob and its `function`
declarations are made again from its scripts; one that declares with
`let`, `const` or `class` keeps its sessions resident. On quickjs the
sandbox stores what it can (its own rules, `let` and `const` included).

```js
import { DatabaseSync } from 'node:sqlite'; // bun: import { Database } from 'bun:sqlite'

const db = new DatabaseSync('actors.db');
db.exec('CREATE TABLE IF NOT EXISTS blobs (session INTEGER PRIMARY KEY, blob BLOB)');
const put = db.prepare('INSERT OR REPLACE INTO blobs VALUES (?, ?)');
const get = db.prepare('SELECT blob FROM blobs WHERE session = ?');

const host = new Host(config, { wake: (host, session) => host.wake(session, get.get(session).blob) });

function evict(host) {                     // after a turn: the cold actors leave
  for (const session of host.sweep(1 << 20, 1024)) {
    try {
      put.run(session, host.hibernate(session));
    } catch (error) {                      // not at rest, or a value outside the table
      if (!(error instanceof TinyactorsError)) throw error;
    }
  }
}
```

**Plan the capacity up front.** A hibernated actor keeps its slot and its
session's record in the host (about 60 B), so `system.maxActors` counts
every actor. Instance and string memory are needed for the resident ones
only. Left to grow, a segment doubles all its pools together and sizes
instance memory from `maxActors`. For `R` actors resident out of `N`:

```js
const config = { systemID: 'big', system: {
  maxActors: N + room, maxEvents: 4096,
  instanceBytes: 2048 * (R + room), stringBytes: 16384 + 64 * (R + room) } };
```

`tools/stress.mjs` (`mise run stress`) is this pattern at scale: two
million actors, at most 1000 resident, the rest in SQLite (`node:sqlite`
under Node.js, `bun:sqlite` under Bun).

One run of it (`mise run stress`, 2026-10-16, x86_64 Linux 6.18, Intel
Xeon @ 2.10GHz, 4 CPUs, 16 GiB; node 22.23.3, bun 1.4.1): two million
actors of a counter chart, at most 1000 resident, 200,000 events to actors
drawn at random, so nearly every event wakes one from the store and the
sweep sends another back (199,860 wakes in each run). Every sampled actor's
count matched the events it was sent, and after every eviction at most
1000 actors were resident.

| runtime, datamodel | populate (spawn and hibernate 2,000,000) | 200,000 events (woken, swept back) | peak resident | core's region | SQLite file |
|---|---|---|---|---|---|
| node, native | 26.1 s (76,700 a second) | 14.6 s (13,700 a second) | 253 MB (126 B per actor) | 115 MB (58 B per actor) | 378 MB |
| node, quickjs | 17.1 s (116,900 a second) | 11.9 s (16,900 a second) | 262 MB (131 B per actor) | 115 MB (58 B per actor) | 409 MB |
| bun, native | 19.5 s (102,600 a second) | 12.9 s (15,500 a second) | 236 MB (118 B per actor) | 115 MB (58 B per actor) | 378 MB |
| bun, quickjs | 14.0 s (143,000 a second) | 11.9 s (16,900 a second) | 221 MB (111 B per actor) | 115 MB (58 B per actor) | 409 MB |

## Migration

A new version of a chart (an image added with a chart of the same
`name`) runs the actors spawned from then on. `host.migrate` moves the
running actors to it, each at a stable point, during the next turns. A
moved actor keeps its session id, its state configuration (states are
matched by id) and its delayed sends with their data. It first gets
`error.platform.upgrade`, whose data is the old version's `<datamodel>`
bindings, and the new version takes what it keeps from there:

```xml
<transition event="error.platform.upgrade">
  <assign location="n" expr="_event.data.n"/>
</transition>
```

- **`host.migrate({chart, session, wake, timeout}?) → number`** starts a
  migration of every chart, of one `chart` by name, or of one `session`,
  and returns its id. `timeout` is in milliseconds of the host's clock,
  rounded up to its next tick; 0 or none means no timeout.
- **`migrationDone: (host, result) => ...`** gets the end of each
  migration, `{migration, migrated, failed, pending}`, `pending` the
  sessions (bigints) its timeout left behind. It is a handler of the
  host, not of the call.
- **`migrated: (host, migrated) => ...`** gets each actor moved, as a thin
  `Migrated` view of `session`, `segment`, `chart` (its name) and
  `migration`, valid until the handler's batch is acknowledged.

**When a migration ends.** A migration ends when every actor it looks at
has moved, or at its `timeout`. A hibernated actor moves once it is
woken. With `wake: true` the migration asks for it through the `wake`
handler, with the migration's id, and asks again if it finds the actor
hibernated after a wake. Without `wake`, a hibernated actor that nobody
wakes holds the migration until its timeout, or forever without one, and
`migrationDone` is never called: give such a migration a `timeout`. A
migration still running when the host is closed never reaches
`migrationDone`.

**Actors that do not move.** An actor that cannot be written out stays
on its version and counts as failed: on the native datamodel, one whose
variables hold a value outside the value table (a function, a `Map`, a
`Date`, ...), or whose chart's scripts declare with `let`, `const` or
`class`. An actor that is running an invocation waits until the
invocation ends. On the native datamodel the old bindings are exactly the
old version's variables; a chart whose scripts declare top-level names
gets its new version's declarations (its scripts are instantiated, not
run), and the old names the new version does not declare are gone.

**With most actors hibernated, wake them a batch at a time.** A migration
with `wake: true` asks for every hibernated actor early. Wake them all and
they are all resident at once. Instead, queue the migration's requests
and wake a batch per round; the migration takes each woken actor in its
next runs. An event's request (`migration` is `undefined`) still wakes its
actor at once, and the migration takes that one too. Put actors back to
sleep once they have moved. One put back before it moved is asked for
again, which costs work but does not stall the migration.

```js
const asked = [];
const wake = (host, session, migration) => {
  if (migration !== undefined) asked.push(session);    // the migration's: a batch at a time
  else host.wake(session, get.get(session).blob);      // an event's: now
};
const host = new Host(config, { wake, migrationDone: (host, result) => (done = result) });
// ...
host.migrate({ wake: true });
while (done === undefined) {
  for (const session of asked.splice(0, 256)) host.wake(session, get.get(session).blob);
  await host.idle();
  evictMoved(host);                                    // the moved actors at rest go back to the store
}
```

`tools/migration.mjs` (`mise run migration`) is this pattern at scale:
two million actors, at most 1000 resident, the rest in SQLite.

One run of it (`mise run migration`, 2026-10-16, x86_64 Linux 6.18, Intel
Xeon @ 2.10GHz, 4 CPUs, 16 GiB; node 22.23.3, bun 1.4.1): two million
actors of version 1 of a counter, at most 1000 resident, 200,000 events
before the migration and 200,000 more during it, every actor moved to
version 2 with `wake: true` (2,218,265 wakes from the store in each run).
The migration ended with every actor migrated, none failed and none left;
all 25,000 sampled actors had their count and version 2; version 1's
segment was retired; and after every eviction at most 1000 actors were
resident (plus two batches while the migration moved them).

| runtime, datamodel | populate (2,000,000 and 200,000 events) | migrate (2,000,000 moved, 200,000 events) | peak resident | core's region, both versions / after | SQLite file |
|---|---|---|---|---|---|
| node, native | 54.5 s | 154.2 s (12,970 a second) | 656 MB (328 B per actor) | 231 MB / 115 MB (58 B per actor) | 576 MB |
| node, quickjs | 42.0 s | 108.7 s (18,390 a second) | 657 MB (329 B per actor) | 231 MB / 115 MB (58 B per actor) | 756 MB |
| bun, native | 50.5 s | 131.2 s (15,240 a second) | 680 MB (340 B per actor) | 231 MB / 115 MB (58 B per actor) | 576 MB |
| bun, quickjs | 41.0 s | 109.5 s (18,270 a second) | 605 MB (303 B per actor) | 231 MB / 115 MB (58 B per actor) | 756 MB |

## Snapshots

`host.snapshot()` copies the whole host into one `Buffer`: every
segment's memory (actors, queues, timers, the hibernated actors'
records), the datamodel's sessions and payloads, the images, the system
id and the host's time. `Host.restore(snapshot, config, options)` makes a
new host running it. Pass the config the snapshot was taken with (its
`systemID` is checked; its I/O processors and tick rate are not in the
snapshot), the snapshot's `datamodel`, and the handlers `new Host` takes.
Every session id stays the same, and the host's clock goes on from the
snapshot's time, whatever clock reads it: a new `SimulatedClock`, or the
runtime's clock in another process or on another machine.

```js
const blob = host.snapshot();             // quiescent: not from a handler
host.close();
// ...
host = Host.restore(blob, config, { log, wake });
```

- **A snapshot is valid only for the same build of the addon.** It holds
  raw memory, so it is not a backup; durability is your database's job,
  and hibernation is how a running process keeps actors out of memory. A
  snapshot is a copy of the whole application, every chart version at
  once. Use one to rehearse a deploy (add the new image, migrate, run the
  clock ahead, look), to capture a host when something goes wrong and
  step through it on the same build, to start every test from one
  prepared world, or to take a host out of the process for a while.
- **Invoked services are started again.** The services you run are not
  in the snapshot. The restored host's first turn announces each
  invocation that was running to the `services` handler you pass to
  `Host.restore`, with `invocation.restored` true and the same
  `invokeid`, `src` and params. Pass your real handlers to go on, or
  stand-ins for a copy that must not reach the real services. A type with
  no handler gives its invokers `error.communication`, and nothing is
  thrown.
- **Hibernated actors stay in your store.** The snapshot holds their
  records, not their blobs, and the restored host asks for them through
  `wake` as before. A copy that hibernates actors needs its own copy of
  the store, since it writes blobs too.
- **The host must be quiescent.** A snapshot is refused with
  `TA_ERROR_STATE` from a handler, while an effect or a typed method's
  reply has not been handled (`await host.idle()` first), and while a
  migration runs. It is refused with `TA_ERROR_UNSUPPORTED` when a
  session or payload holds what hibernation cannot store (on the native
  datamodel a function, a `Map`, a `Date`, ..., or a session of a chart
  whose scripts declare with `let`, `const` or `class`); the message names
  the variable. The host goes on either way.
- **Values are written through the value table**, so two variables of
  one session that share an object no longer share it after a restore.

`tools/snapshot.mjs` (`mise run snapshot`) rehearses a deploy on a copy:
a live host of two million actors, at most 1000 resident and the rest in
SQLite, a delayed send pending for each resident one, and half of those
paying through an invoked payment service that has not answered; its
snapshot written to a file and its store copied; the copy restored with
its own store and clock and a stand-in payment service, which gets every
payment in flight again (`restored`, with its amount) and declines it,
and whose own snapshot is the same bytes, then migrated to a new version
while events come, its clock run past the delayed sends, while the live
host goes on with events of its own and its payments still in flight. The
copy never reaches the real service.

One run of it (`mise run snapshot`, and again with `--actors 100000
--resident 100000 --events 100000`, 2026-10-16, x86_64 Linux 6.18, Intel
Xeon @ 2.10GHz, 4 CPUs, 16 GiB; node 22.23.3, bun 1.4.1). Two million
actors of an order chart, at most 1000 resident, 200,000 events before the
snapshot and 200,000 more on each host after it; then 100,000 actors all
resident, so the datamodel's own state is most of the snapshot. In every
run the copy's snapshot was the same bytes, the copy migrated every actor
to version 2 (2,018,400 wakes from its store at two million), all 25,000
sampled actors had their counts, delayed sends and versions on both hosts,
the live store held only the live host's blobs, and after every eviction
at most R actors were resident.

| runtime, datamodel | actors, resident | snapshot | taken | restored | migrate on the copy | peak resident (both hosts) |
|---|---|---|---|---|---|---|
| node, native | 2,000,000, 1000 | 116.2 MB (58 B per actor) | 0.11 s | 0.55 s | 172.4 s | 934 MB |
| node, quickjs | 2,000,000, 1000 | 116.1 MB (58 B per actor) | 0.09 s | 0.09 s | 125.7 s | 851 MB |
| bun, native | 2,000,000, 1000 | 116.2 MB (58 B per actor) | 0.10 s | 0.08 s | 144.9 s | 1018 MB |
| bun, quickjs | 2,000,000, 1000 | 116.1 MB (58 B per actor) | 0.09 s | 0.07 s | 111.8 s | 937 MB |
| node, native | 100,000, 100,000 | 65.9 MB (659 B per actor) | 0.76 s | 3.42 s | 5.0 s | 817 MB |
| node, quickjs | 100,000, 100,000 | 60.9 MB (609 B per actor) | 0.90 s | 0.74 s | 3.6 s | 638 MB |
| bun, native | 100,000, 100,000 | 65.9 MB (659 B per actor) | 0.62 s | 1.13 s | 4.6 s | 1106 MB |
| bun, quickjs | 100,000, 100,000 | 60.9 MB (609 B per actor) | 0.86 s | 0.61 s | 3.3 s | 742 MB |

Core's region is 58 B an actor with most of them hibernated and 469 B
all resident; the native datamodel's rows take about 50 B an actor more
than quickjs's state in a snapshot of resident actors.

## Invoked services

A chart can `<invoke>` a service that your program runs, such as a
connection, a subprocess or a long computation. Register a handler per
type: `new Host(config, { services: { [type]: handler } })`. When an
invocation starts, the handler receives the host and an `Invocation` and
returns the service, any object. The host keeps the service until the
invocation ends.

```xml
<state id="talking">
  <invoke id="counter" type="urn:example:counter" src="counter://local">
    <param name="seed" expr="seed"/>
  </invoke>
  <transition event="go"><send target="#_counter" event="ping"/></transition>
  <transition event="pong">...</transition>
  <transition event="done.invoke.counter" target="finished"/>
</state>
```

```js
class Counter {
  constructor(invocation) {
    this.invocation = invocation;
    this.count = 0;
  }
  receive(host, send) {                 // a <send target="#_counter">, or autoforwarded
    this.count++;
    if (send.event === 'finish') this.invocation.done({ total: this.count }); // done.invoke.counter
    else this.invocation.sendBack('pong', { n: this.count });
  }
  stop(host) {}                         // the invoker cancelled it (left the state, ended)
}

const host = new Host(config, {
  services: { 'urn:example:counter': (host, invocation) => new Counter(invocation) },
});
```

- **The invocation** has `session` (the invoker, a `bigint`),
  `invokeid`, `type`, `src`, `data` (the params, or the content),
  `traceparent`, `running` and `restored` ("Snapshots"). `sendBack(event,
  data?)` gives the invoker an event with its `invokeid` set.
  `done(data?)` ends the service with `done.invoke.<invokeid>`. Both keep
  the invoke's trace unless given `{traceparent}` (`null` for a new
  one). The invocation is a record, so you can keep it and answer while
  the service runs, from a timer or a promise. After `done`, a cancel or
  `close()`, these calls throw `TinyactorsError` with `TA_ERROR_STALE`.
- **The service** may have `receive(host, send)`, which gets each send to
  it as a thin view (`type` is `undefined`), and `stop(host)`, which gets
  the cancel; both are called on the service, and both are optional. A
  `receive` that returns a promise has its send filled before the batch
  ends, so it may read it after an `await`. A service that ended with
  `done` is neither stopped nor kept, so a handler may call `done` and
  return at once.
- **The handler returns the service, not a promise.** An `async` handler
  is refused with a `TypeError` and the invoker gets
  `error.communication`: start the work in a plain handler, return the
  service at once, and answer with `sendBack` and `done` when the work is
  ready.
- **A failure** of any kind gives the invoker `error.communication`: no
  handler for the type, a handler that throws, a `receive` that throws or
  whose promise rejects (the service crashed and is dropped, not
  stopped), or no handler for a restored invocation. Each later send to
  that invocation gets one too, until the invoker leaves the state. The
  invoker gets none when it cancelled the invocation in the same run. The
  error is reported after the turn, as any handler's ("Before you use
  it"); a restored invocation with no handler throws nothing, since
  declining to start it again was your choice.
- **`close()`** drops the running services without calling `stop`. The
  host keeps them on its own object, so a dropped host whose services
  close over it is still collected.

An invocation uses one of core's requests, so `system.maxRequests` must
cover the invocations that run at once.

`tools/services.mjs` (`mise run services`) runs a population of actors
that each invoke a counter service: pings and pongs to actors drawn at
random, then a third finish with done data, a third leave the invoking
state (the service is stopped), the rest keep theirs; every pong is
counted once, every done datum is right, every cancelled service was
stopped once and no other, and the ended services are collected.

One run of it (`mise run services`, 2026-10-16, x86_64 Linux 6.18, Intel
Xeon @ 2.10GHz, 4 CPUs, 16 GiB; node 22.23.3, bun 1.4.1). 200,000 actors,
each with a counter service, made 1,000,000 round trips (actor, service,
actor); then a third finished with done data and a third were cancelled.
In every run every pong and done datum was right, every cancelled service
was stopped exactly once and no other, and the host held only the 66,666
services still running.

| runtime, datamodel | start 200,000 invocations | 1,000,000 round trips | end (done, cancel) | peak resident | core's region |
|---|---|---|---|---|---|
| node, native | 5.0 s (40,000 a second) | 27.2 s (37,000 a second) | 2.1 s | 575 MB (2875 B per actor and service) | 50.3 MB (251 B per actor) |
| node, quickjs | 4.0 s (50,000 a second) | 22.4 s (45,000 a second) | 2.3 s | 698 MB (3491 B per actor and service) | 61.8 MB (309 B per actor) |
| bun, native | 3.9 s (52,000 a second) | 20.7 s (48,000 a second) | 1.9 s | 843 MB (4214 B per actor and service) | 50.3 MB (251 B per actor) |
| bun, quickjs | 3.1 s (64,000 a second) | 19.0 s (53,000 a second) | 1.6 s | 766 MB (3830 B per actor and service) | 61.8 MB (309 B per actor) |

A round trip costs about what it costs with 10,000 actors: a run's work
does not grow with the number of running invocations.

## Scale

`tools/scale.mjs` (`mise run scale`) is a gate against work that grows
with the population: it times every operation of the API on a host of N
actors and on one of 2N, each in a process of its own, and fails when an
operation costs more than 1.5 times as much at 2N, per call for an
operation on one actor and per actor for one on the whole population.
Most of the actors are hibernated in SQLite, and one in a hundred runs an
invoked service, so core's requests grow with the population too; then
the same with every actor resident. A per-call cost is the median of five
samples of at least 200 ms; a whole-population cost is the least of three;
an operation over the factor is measured again before it fails.

One run of it (`mise run scale`, 2026-10-16, x86_64 Linux 6.18, Intel
Xeon @ 2.10GHz, 4 CPUs, 16 GiB; node 22.23.3, bun 1.4.1). Each cell is the
cost at N, then at 2N, in microseconds, and their ratio.

Hibernated: 2,000,000 and 4,000,000 actors, at most 1000 counters
resident, 20,000 and 40,000 actors each running an invoked service.

| operation | unit | node, native | node, quickjs | bun, native | bun, quickjs |
|---|---|---|---|---|---|
| populate (spawn and hibernate) | µs per actor | 12.1 → 12.1 (0.99) | 7.80 → 7.95 (1.02) | 9.02 → 8.81 (0.98) | 6.65 → 6.57 (0.99) |
| spawn | µs per call | 5.77 → 6.07 (1.05) | 2.79 → 2.51 (0.90) | 5.29 → 5.04 (0.95) | 2.37 → 2.64 (1.11) |
| terminate | µs per call | 0.897 → 0.743 (0.83) | 0.622 → 0.579 (0.93) | 0.886 → 0.843 (0.95) | 0.718 → 0.642 (0.89) |
| send | µs per call | 4.62 → 4.19 (0.91) | 2.27 → 2.12 (0.94) | 4.49 → 3.56 (0.79) | 3.89 → 2.02 (0.52) |
| sendMany | µs per event | 5.11 → 4.50 (0.88) | 2.43 → 2.44 (1.00) | 3.50 → 3.46 (0.99) | 2.13 → 1.99 (0.93) |
| send (hibernated: woken from the store) | µs per call | 35.1 → 33.5 (0.95) | 41.5 → 45.0 (1.09) | 31.8 → 33.1 (1.04) | 37.7 → 42.1 (1.12) |
| hibernate | µs per call | 23.8 → 54.2 (2.28) **fails** | 23.6 → 51.0 (2.16) **fails** | 26.0 → 49.3 (1.89) **fails** | 22.1 → 45.2 (2.04) **fails** |
| wake | µs per call | 8.30 → 8.06 (0.97) | 4.42 → 4.69 (1.06) | 8.07 → 9.16 (1.13) | 5.11 → 5.23 (1.02) |
| wake (an event waiting) | µs per call | 1086 → 3283 (3.02) **fails** | 1169 → 3293 (2.82) **fails** | 1156 → 3172 (2.74) **fails** | 1214 → 3012 (2.48) **fails** |
| terminate (hibernated, an event waiting) | µs per call | 1.69 → 1.68 (0.99) | 1.52 → 1.44 (0.95) | 1.76 → 1.61 (0.91) | 1.62 → 1.48 (0.91) |
| terminate (hibernated) | µs per call | 18.8 → 19.3 (1.03) | 11.8 → 13.9 (1.18) | 14.0 → 16.4 (1.17) | 11.8 → 13.1 (1.10) |
| actorByName | µs per call | 4935 → 28722 (5.82) **fails** | 6058 → 27541 (4.55) **fails** | 5203 → 29825 (5.73) **fails** | 5536 → 28725 (5.19) **fails** |
| boot | µs per call | 3.83 → 4.28 (1.12) | 2.10 → 2.18 (1.04) | 4.83 → 4.97 (1.03) | 2.84 → 3.24 (1.14) |
| actorState | µs per call | 4.34 → 4.69 (1.08) | 4.89 → 4.75 (0.97) | 4.47 → 3.90 (0.87) | 3.97 → 3.65 (0.92) |
| actorContext | µs per call | 7.28 → 8.13 (1.12) | 5.55 → 5.42 (0.98) | 6.60 → 7.03 (1.07) | 4.18 → 4.23 (1.01) |
| actorGet | µs per call | 5.97 → 6.58 (1.10) | 7.16 → 6.53 (0.91) | 5.00 → 5.14 (1.03) | 5.43 → 4.61 (0.85) |
| hostStats | µs per call | 20.3 → 19.2 (0.95) | 19.9 → 21.7 (1.09) | 15.0 → 15.7 (1.05) | 14.7 → 15.9 (1.08) |
| hostConfig | µs per call | 18.1 → 16.9 (0.93) | 18.3 → 21.6 (1.18) | 12.8 → 13.0 (1.02) | 13.1 → 13.0 (0.99) |
| actorList (a page) | µs per call | 41.3 → 38.7 (0.94) | 41.4 → 40.1 (0.97) | 18.2 → 18.7 (1.03) | 20.1 → 18.6 (0.93) |
| traceConfigure | µs per call | 18.6 → 12.5 (0.67) | 15.3 → 13.4 (0.88) | 7.00 → 7.85 (1.12) | 6.89 → 7.00 (1.02) |
| traceWatch | µs per call | 15.7 → 13.6 (0.87) | 14.2 → 15.0 (1.06) | 8.79 → 8.62 (0.98) | 8.37 → 7.95 (0.95) |
| traceUnwatch | µs per call | 12.7 → 12.8 (1.01) | 12.4 → 13.1 (1.06) | 6.93 → 7.02 (1.01) | 6.58 → 6.57 (1.00) |
| traceEnd | µs per call | 4.17 → 4.62 (1.11) | 4.02 → 4.71 (1.17) | 3.61 → 3.69 (1.02) | 3.20 → 3.60 (1.12) |
| traceSpansDecode | µs per call | 6.71 → 5.51 (0.82) | 4.93 → 5.13 (1.04) | 3.82 → 4.29 (1.12) | 3.73 → 3.80 (1.02) |
| metrics | µs per call | 8.01 → 7.13 (0.89) | 8.55 → 9.81 (1.15) | 7.30 → 6.82 (0.93) | 6.13 → 7.18 (1.17) |
| I/O processor round trip | µs per call | 12.2 → 13.3 (1.09) | 9.27 → 9.25 (1.00) | 9.36 → 9.94 (1.06) | 6.58 → 7.03 (1.07) |
| transport round trip | µs per call | 34.3 → 35.6 (1.04) | 29.9 → 31.6 (1.06) | 29.0 → 27.9 (0.96) | 21.5 → 23.2 (1.08) |
| delayed send and its timer | µs per call | 7.84 → 8.25 (1.05) | 3.56 → 3.77 (1.06) | 8.27 → 5.75 (0.70) | 3.34 → 3.47 (1.04) |
| invoked service round trip | µs per call | 17.0 → 16.6 (0.98) | 12.3 → 13.5 (1.10) | 13.0 → 15.7 (1.21) | 10.1 → 10.9 (1.07) |
| actorList (every actor) | µs per actor | 0.147 → 0.158 (1.07) | 0.150 → 0.160 (1.06) | 0.108 → 0.099 (0.92) | 0.099 → 0.096 (0.97) |
| sweep (every actor) | µs per actor | 0.008 → 0.008 (1.02) | 0.008 → 0.008 (1.04) | 0.008 → 0.008 (0.92) | 0.008 → 0.007 (0.93) |
| migrate (one session) | µs per call | 50.5 → 86.4 (1.71) **fails** | 36.7 → 73.3 (2.00) **fails** | 74.2 → 73.6 (0.99) | 41.8 → 70.6 (1.69) **fails** |
| snapshot | µs per actor | 0.174 → 0.158 (0.91) | 0.143 → 0.148 (1.04) | 0.099 → 0.147 (1.48) | 0.083 → 0.087 (1.05) |
| restore (and its first turn) | µs per actor | 0.306 → 0.271 (0.89) | 0.188 → 0.191 (1.01) | 0.229 → 0.214 (0.93) | 0.159 → 0.161 (1.02) |
| close | µs per actor | 0.057 → 0.054 (0.95) | 0.065 → 0.068 (1.04) | 0.055 → 0.067 (1.23) | 0.063 → 0.081 (1.28) |

All resident: 1,000,000 and 2,000,000 actors. Two million resident actors
take up to 11.6 GB, so four million do not fit on this machine.

| operation | unit | node, native | node, quickjs | bun, native | bun, quickjs |
|---|---|---|---|---|---|
| populate (spawn, the segment growing) | µs per actor | 5.99 → 6.89 (1.15) | 4.42 → 4.56 (1.03) | 5.51 → 5.81 (1.05) | 4.33 → 4.33 (1.00) |
| spawn | µs per call | 5.48 → 5.08 (0.93) | 2.68 → 2.60 (0.97) | 4.94 → 4.38 (0.89) | 2.51 → 2.50 (1.00) |
| send | µs per call | 7.35 → 6.90 (0.94) | 3.97 → 4.81 (1.21) | 6.04 → 6.86 (1.13) | 4.26 → 5.05 (1.19) |
| sendMany | µs per event | 7.07 → 7.41 (1.05) | 4.13 → 4.41 (1.07) | 6.69 → 6.08 (0.91) | 4.58 → 4.23 (0.93) |
| terminate | µs per call | 2.58 → 3.50 (1.36) | 3.65 → 3.59 (0.99) | 3.37 → 3.64 (1.08) | 3.70 → 3.73 (1.01) |
| terminate (runnable) | µs per call | 0.520 → 0.579 (1.11) | 0.509 → 0.700 (1.37) | 0.593 → 0.692 (1.17) | 0.708 → 0.515 (0.73) |
| actorByName | µs per call | 2169 → 4695 (2.16) **fails** | 1998 → 4708 (2.36) **fails** | 2462 → 12553 (5.10) **fails** | 2288 → 14369 (6.28) **fails** |
| hostStats | µs per call | 20.2 → 20.8 (1.03) | 20.4 → 20.9 (1.03) | 17.3 → 17.0 (0.99) | 13.5 → 14.0 (1.04) |
| actorList (every actor) | µs per actor | 0.170 → 0.157 (0.93) | 0.205 → 0.194 (0.95) | 0.117 → 0.106 (0.91) | 0.138 → 0.134 (0.97) |
| sweep (every actor) | µs per actor | 0.008 → 0.008 (0.98) | 0.008 → 0.007 (0.94) | 0.011 → 0.008 (0.77) | 0.008 → 0.008 (0.97) |
| hibernate | µs per call | 8.82 → 7.04 (0.80) | 8.34 → 8.25 (0.99) | 6.75 → 7.07 (1.05) | 8.80 → 7.36 (0.84) |
| wake | µs per call | 9.14 → 8.73 (0.96) | 3.46 → 3.48 (1.00) | 6.82 → 8.28 (1.21) | 4.00 → 3.42 (0.86) |
| migrate (a chart, every actor) | µs per actor | 23.3 → 24.4 (1.05) | 13.5 → 13.6 (1.01) | 19.2 → 18.4 (0.95) | 11.8 → 12.5 (1.06) |
| snapshot | µs per actor | 11.6 → 15.5 (1.34) | 7.43 → 9.25 (1.24) | 7.55 → 5.33 (0.71) | 7.31 → 10.4 (1.42) |
| restore (and its first turn) | µs per actor | 20.0 → 20.4 (1.02) | 4.78 → 5.16 (1.08) | 10.8 → 9.06 (0.84) | 4.57 → 4.72 (1.03) |
| close | µs per actor | 0.628 → 0.477 (0.76) | 1.76 → 1.38 (0.78) | 0.598 → 0.501 (0.84) | 1.63 → 1.35 (0.83) |

Peak resident memory, at N and 2N: hibernated node native 1380 / 2652 MB; node quickjs 1427 / 2707 MB; bun native 1126 / 2101 MB; bun quickjs 1102 / 2057 MB; all resident node native 5944 / 11620 MB; node quickjs 4359 / 8259 MB; bun native 8604 / 8002 MB; bun quickjs 4659 / 9774 MB.

**Operations that fail the gate.** Each is a walk in core, filed as a
request to core (`ROADMAP.md`, "Requests to core"):

- `host.actorByName` walks every actor, hibernated ones included: about
  5 to 30 ms a lookup at these sizes.
- `host.hibernate`, and a migration of one session (over the factor on
  three of the four pairs), walk every request
  of the segment to check the actor holds none, so they cost more the more
  invocations are running (none running: about 7 to 9 us at every size).
- `host.wake` of a hibernated actor that has an event waiting, before
  core asks for it, walks core's queue of wakes from its head.

Everything else costs per call what it costs at half the actors.
