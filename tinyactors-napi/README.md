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

One run of it (`mise run scale`, 2026-10-17, x86_64 Linux 6.18, Intel
Xeon @ 2.10GHz, 4 CPUs, 16 GiB; node 22.23.3, bun 1.4.1), after core's
index of names, its count of each actor's requests and its wake queue
linked both ways. Each cell is the cost at N, then at 2N, in
microseconds, and their ratio.

Hibernated: 2,000,000 and 4,000,000 actors, at most 1000 counters
resident, 20,000 and 40,000 actors each running an invoked service.

| operation | unit | node, native | node, quickjs | bun, native | bun, quickjs |
|---|---|---|---|---|---|
| populate (spawn and hibernate) | µs per actor | 12.2 → 12.0 (0.98) | 7.65 → 7.47 (0.98) | 8.29 → 8.40 (1.01) | 6.43 → 6.43 (1.00) |
| spawn | µs per call | 6.10 → 5.89 (0.97) | 2.69 → 2.61 (0.97) | 4.66 → 5.07 (1.09) | 2.64 → 2.69 (1.02) |
| terminate | µs per call | 0.782 → 0.739 (0.94) | 0.610 → 0.580 (0.95) | 0.869 → 0.854 (0.98) | 0.786 → 0.720 (0.92) |
| send | µs per call | 4.61 → 4.60 (1.00) | 2.02 → 2.62 (1.30) | 3.93 → 3.48 (0.89) | 2.38 → 2.49 (1.05) |
| sendMany | µs per event | 4.67 → 4.78 (1.02) | 2.28 → 2.16 (0.95) | 3.53 → 3.33 (0.94) | 2.16 → 2.26 (1.05) |
| send (hibernated: woken from the store) | µs per call | 33.1 → 29.4 (0.89) | 40.7 → 42.3 (1.04) | 30.3 → 27.7 (0.92) | 33.5 → 48.6 (1.45) |
| hibernate | µs per call | 5.92 → 6.13 (1.04) | 4.81 → 4.70 (0.98) | 4.58 → 4.14 (0.91) | 4.20 → 4.02 (0.96) |
| wake | µs per call | 7.44 → 8.42 (1.13) | 4.34 → 4.17 (0.96) | 7.31 → 6.88 (0.94) | 4.83 → 4.43 (0.92) |
| wake (an event waiting) | µs per call | 8.53 → 8.49 (1.00) | 4.37 → 4.94 (1.13) | 8.35 → 8.06 (0.97) | 5.44 → 5.16 (0.95) |
| terminate (hibernated, an event waiting) | µs per call | 1.49 → 1.49 (1.00) | 1.34 → 1.52 (1.14) | 1.77 → 1.74 (0.98) | 1.79 → 1.68 (0.94) |
| terminate (hibernated) | µs per call | 17.9 → 17.2 (0.96) | 13.0 → 14.6 (1.12) | 15.0 → 15.3 (1.02) | 12.1 → 13.0 (1.08) |
| actorByName | µs per call | 0.528 → 0.511 (0.97) | 0.439 → 0.510 (1.16) | 0.405 → 0.415 (1.02) | 0.297 → 0.322 (1.08) |
| boot | µs per call | 4.24 → 4.12 (0.97) | 2.06 → 2.16 (1.05) | 4.53 → 4.70 (1.04) | 3.08 → 3.32 (1.08) |
| actorState | µs per call | 4.75 → 5.17 (1.09) | 4.30 → 4.63 (1.08) | 4.11 → 4.00 (0.97) | 3.90 → 4.41 (1.13) |
| actorContext | µs per call | 7.69 → 7.83 (1.02) | 5.53 → 6.01 (1.09) | 6.81 → 6.71 (0.99) | 4.44 → 4.45 (1.00) |
| actorGet | µs per call | 6.05 → 6.30 (1.04) | 6.28 → 6.48 (1.03) | 5.71 → 5.20 (0.91) | 5.39 → 5.17 (0.96) |
| hostStats | µs per call | 20.2 → 20.4 (1.01) | 21.3 → 20.4 (0.96) | 15.1 → 15.1 (1.00) | 15.0 → 20.6 (1.37) |
| hostConfig | µs per call | 17.2 → 17.5 (1.02) | 19.0 → 18.3 (0.96) | 12.8 → 12.4 (0.97) | 12.9 → 13.0 (1.00) |
| actorList (a page) | µs per call | 37.2 → 38.3 (1.03) | 47.7 → 40.3 (0.84) | 18.3 → 18.5 (1.01) | 20.2 → 19.2 (0.95) |
| traceConfigure | µs per call | 12.4 → 13.5 (1.09) | 13.2 → 15.0 (1.14) | 7.16 → 7.07 (0.99) | 7.25 → 6.91 (0.95) |
| traceWatch | µs per call | 14.1 → 15.7 (1.11) | 14.1 → 17.2 (1.22) | 8.67 → 8.72 (1.01) | 8.12 → 9.71 (1.20) |
| traceUnwatch | µs per call | 12.9 → 14.2 (1.10) | 13.3 → 13.5 (1.02) | 6.86 → 7.24 (1.05) | 6.63 → 7.17 (1.08) |
| traceEnd | µs per call | 3.84 → 4.24 (1.10) | 4.00 → 3.84 (0.96) | 3.84 → 3.97 (1.03) | 3.70 → 4.22 (1.14) |
| traceSpansDecode | µs per call | 4.89 → 5.03 (1.03) | 5.52 → 5.29 (0.96) | 3.94 → 3.84 (0.97) | 3.91 → 3.88 (0.99) |
| metrics | µs per call | 8.96 → 8.82 (0.99) | 8.55 → 9.48 (1.11) | 7.12 → 6.47 (0.91) | 6.26 → 6.87 (1.10) |
| I/O processor round trip | µs per call | 13.1 → 14.8 (1.12) | 9.25 → 8.60 (0.93) | 8.99 → 9.46 (1.05) | 7.32 → 7.62 (1.04) |
| transport round trip | µs per call | 35.4 → 36.1 (1.02) | 28.2 → 32.8 (1.16) | 27.8 → 29.2 (1.05) | 24.0 → 26.0 (1.08) |
| delayed send and its timer | µs per call | 7.68 → 7.91 (1.03) | 3.36 → 3.63 (1.08) | 5.75 → 5.79 (1.01) | 3.82 → 3.76 (0.98) |
| invoked service round trip | µs per call | 16.0 → 16.5 (1.03) | 11.4 → 12.9 (1.13) | 12.3 → 12.8 (1.04) | 12.7 → 11.5 (0.91) |
| actorList (every actor) | µs per actor | 0.153 → 0.155 (1.01) | 0.179 → 0.162 (0.90) | 0.101 → 0.103 (1.02) | 0.105 → 0.105 (1.00) |
| sweep (every actor) | µs per actor | 0.005 → 0.007 (1.31) | 0.008 → 0.007 (0.89) | 0.007 → 0.007 (1.07) | 0.008 → 0.008 (1.03) |
| migrate (one session) | µs per call | 28.7 → 27.9 (0.97) | 18.2 → 19.5 (1.07) | 22.5 → 22.5 (1.00) | 18.6 → 22.3 (1.20) |
| snapshot | µs per actor | 0.204 → 0.198 (0.97) | 0.172 → 0.209 (1.21) | 0.106 → 0.170 (1.60) **fails** | 0.107 → 0.095 (0.88) |
| restore (and its first turn) | µs per actor | 0.321 → 0.307 (0.96) | 0.214 → 0.206 (0.96) | 0.237 → 0.237 (1.00) | 0.190 → 0.193 (1.01) |
| close | µs per actor | 0.080 → 0.099 (1.23) | 0.094 → 0.107 (1.15) | 0.081 → 0.108 (1.32) | 0.092 → 0.114 (1.24) |

All resident: 1,000,000 and 2,000,000 actors. Two million resident actors
take up to 8.8 GB, so four million do not fit on this machine.

| operation | unit | node, native | node, quickjs | bun, native | bun, quickjs |
|---|---|---|---|---|---|
| populate (spawn, the segment growing) | µs per actor | 6.60 → 7.41 (1.12) | 4.74 → 4.85 (1.02) | 5.92 → 5.65 (0.95) | 4.62 → 4.14 (0.89) |
| spawn | µs per call | 5.19 → 5.32 (1.02) | 2.79 → 2.66 (0.95) | 5.31 → 5.50 (1.04) | 2.56 → 2.62 (1.02) |
| send | µs per call | 6.89 → 7.58 (1.10) | 4.25 → 5.26 (1.24) | 6.86 → 6.35 (0.93) | 4.71 → 4.47 (0.95) |
| sendMany | µs per event | 7.36 → 7.22 (0.98) | 5.05 → 4.57 (0.91) | 5.38 → 5.93 (1.10) | 4.25 → 4.18 (0.98) |
| terminate | µs per call | 3.22 → 3.40 (1.06) | 3.83 → 3.62 (0.95) | 3.40 → 3.58 (1.05) | 4.03 → 3.88 (0.96) |
| terminate (runnable) | µs per call | 0.587 → 0.520 (0.89) | 0.729 → 0.375 (0.51) | 0.615 → 0.549 (0.89) | 0.520 → 0.538 (1.04) |
| actorByName | µs per call | 0.548 → 0.589 (1.08) | 0.428 → 0.480 (1.12) | 0.441 → 0.503 (1.14) | 0.258 → 0.274 (1.06) |
| hostStats | µs per call | 20.5 → 23.0 (1.12) | 20.6 → 21.0 (1.02) | 15.7 → 16.1 (1.03) | 14.2 → 14.1 (1.00) |
| actorList (every actor) | µs per actor | 0.167 → 0.162 (0.97) | 0.210 → 0.210 (1.00) | 0.112 → 0.121 (1.08) | 0.152 → 0.144 (0.95) |
| sweep (every actor) | µs per actor | 0.008 → 0.008 (1.04) | 0.008 → 0.008 (0.98) | 0.008 → 0.008 (1.04) | 0.008 → 0.008 (0.98) |
| hibernate | µs per call | 7.55 → 7.62 (1.01) | 8.48 → 8.39 (0.99) | 7.76 → 6.60 (0.85) | 7.69 → 7.69 (1.00) |
| wake | µs per call | 9.07 → 8.64 (0.95) | 3.51 → 3.14 (0.90) | 7.32 → 7.28 (1.00) | 4.63 → 3.73 (0.81) |
| snapshot | µs per actor | 7.23 → 9.98 (1.38) | 9.37 → 9.51 (1.02) | 5.61 → 6.64 (1.18) | 7.38 → 10.4 (1.41) |
| migrate (a chart, every actor) | µs per actor | 22.9 → 27.4 (1.20) | 15.1 → 15.1 (1.00) | 18.5 → 18.7 (1.01) | 11.9 → 14.2 (1.19) |
| restore (and its first turn) | µs per actor | 16.2 → 15.8 (0.97) | 4.96 → 6.34 (1.28) | 9.88 → 9.05 (0.92) | 4.68 → 4.88 (1.04) |
| close | µs per actor | 0.642 → 0.567 (0.88) | 1.82 → 1.47 (0.81) | 0.525 → 0.521 (0.99) | 1.64 → 1.46 (0.89) |

Peak resident memory, at N and 2N: hibernated node native 1635 / 3124 MB; node quickjs 1684 / 3210 MB; bun native 1325 / 2476 MB; bun quickjs 1288 / 2436 MB; all resident node native 3284 / 6349 MB; node quickjs 4127 / 7994 MB; bun native 4021 / 7933 MB; bun quickjs 4517 / 8835 MB.

**The operation that fails the gate.** The snapshot of the hibernated
population on bun with the native datamodel, 1.52 and 1.60 in two runs:
at 2,000,000 actors the third of its three samples is faster (0.18, 0.18,
then 0.11 µs an actor), at 4,000,000 none is (0.17 to 0.18), and a
whole-population operation keeps its fastest sample. The cost per actor
is flat otherwise, and the other three pairs pass. On node with quickjs
the sweep of every hibernated actor once failed the same way (0.007 µs
an actor, then 0.003 in the last sample at 2,000,000) and passed in the
next run.

Everything else costs per call what it costs at half the actors.
