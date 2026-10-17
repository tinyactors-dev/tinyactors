# @tinyactors/node

**Status: being rewritten** from core's redesigned API ([ROADMAP.md](ROADMAP.md)).
The last published version, 0.2.0, was built from an earlier binding.

## Install

```sh
npm install @tinyactors/node     # or: pnpm add, bun add
```

It runs on Node.js 22.3 or later (`engines`) and on Bun (tested on 1.4), on macOS (arm64,
x64) and Linux (x64, arm64; glibc or musl). Nothing compiles on
install: the addon is prebuilt, one per platform, in
`@tinyactors/node-darwin-arm64`, `-darwin-x64`, `-linux-x64` and
`-linux-arm64`, which the main package lists as optional dependencies
at exactly its own version, so the package manager installs only the
one whose `os` and `cpu` match. A Linux addon links no C library, so
one file serves glibc and musl. The macOS addons are cross-built from
Linux and checked by their symbols; no Mac has run them yet. An install that left the optional
dependencies out (`--no-optional`, `--omit=optional`) fails at import,
naming the package it needs.

```js
import { compileCharts, Host } from '@tinyactors/node';
```

Development builds of `main` are published under the `next` dist-tag
(`npm install @tinyactors/node@next`), versioned
`<next minor>-dev.<date>.<n>.g<commit>`. Releases, their tarballs and
`SHA256SUMS` are on
[tinyactors-dev/tinyactors-napi](https://github.com/tinyactors-dev/tinyactors-napi/releases).

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
an operation over the factor is measured again, with another seed, before
it fails.

What it measures is the binding's and core's work, under the same
conditions at both sizes:

- **A sweep of every actor** reads each actor's slot, so its cost is the
  memory's: sweep after sweep, two million hibernated actors stay in this
  machine's 260 MB cache and four million do not. When one sweep takes 2
  ms or more, 768 MB are written before each timed sweep, so each starts
  from memory. With every actor resident, each sweep finds every actor
  idle and returns a bigint for each, which times the engine's collector,
  so that scenario times a sweep of a page of 256 per call instead.
- **A snapshot** is timed less the system time of the process during it:
  the kernel faulting in the pages of the Buffer it is written into. The
  faults per actor are the same at both sizes, but bun sometimes reuses a
  collected Buffer's pages, and on this virtual machine a fault costs two
  to five times as much in a process holding 6 GB as in one holding 3 GB.

One run of it (`mise run scale`, 2026-10-17, x86_64 Linux 6.18, Intel
Xeon @ 2.10GHz, 4 CPUs, 16 GiB, a virtual machine; node 22.23.3, bun
1.4.1). Each cell is the cost at N, then at 2N, in microseconds, and their
ratio. Every operation is within the factor on all four pairs; on node,
the hibernated scenario was measured twice ('terminate (hibernated)', 1.70
and 2.66 the first time, 1.08 and 1.16 the second).

Hibernated: 2,000,000 and 4,000,000 actors, at most 1000 counters
resident, 20,000 and 40,000 actors each running an invoked service.

| operation | unit | node, native | node, quickjs | bun, native | bun, quickjs |
|---|---|---|---|---|---|
| populate (spawn and hibernate) | µs per actor | 11.71 → 11.62 (0.99) | 7.16 → 7.36 (1.03) | 8.64 → 8.51 (0.99) | 6.74 → 6.58 (0.98) |
| spawn | µs per call | 5.47 → 5.43 (0.99) | 2.45 → 2.54 (1.04) | 4.82 → 5.59 (1.16) | 2.92 → 2.70 (0.92) |
| terminate | µs per call | 0.755 → 0.733 (0.97) | 0.565 → 0.560 (0.99) | 0.925 → 0.889 (0.96) | 0.758 → 0.742 (0.98) |
| send | µs per call | 4.44 → 4.41 (0.99) | 2.06 → 2.15 (1.04) | 3.71 → 3.61 (0.97) | 2.28 → 2.33 (1.02) |
| sendMany | µs per event | 4.84 → 4.79 (0.99) | 2.38 → 2.29 (0.96) | 3.50 → 3.40 (0.97) | 2.38 → 2.13 (0.89) |
| send (hibernated: woken from the store) | µs per call | 34.97 → 30.14 (0.86) | 33.20 → 40.71 (1.23) | 27.13 → 29.38 (1.08) | 34.26 → 35.97 (1.05) |
| hibernate | µs per call | 5.94 → 5.78 (0.97) | 4.50 → 4.55 (1.01) | 4.22 → 4.20 (0.99) | 4.37 → 4.32 (0.99) |
| wake | µs per call | 7.07 → 7.34 (1.04) | 3.84 → 4.02 (1.05) | 7.93 → 7.57 (0.95) | 4.85 → 4.75 (0.98) |
| wake (an event waiting) | µs per call | 8.45 → 8.11 (0.96) | 4.26 → 4.34 (1.02) | 8.00 → 8.32 (1.04) | 5.28 → 5.30 (1.00) |
| terminate (hibernated, an event waiting) | µs per call | 1.54 → 1.58 (1.02) | 1.27 → 1.21 (0.95) | 1.62 → 1.44 (0.89) | 1.52 → 1.44 (0.94) |
| terminate (hibernated) | µs per call | 16.24 → 17.53 (1.08) | 12.79 → 14.82 (1.16) | 14.23 → 17.83 (1.25) | 12.35 → 12.77 (1.03) |
| actorByName | µs per call | 0.532 → 0.507 (0.95) | 0.415 → 0.426 (1.03) | 0.426 → 0.415 (0.98) | 0.304 → 0.300 (0.99) |
| boot | µs per call | 3.86 → 4.03 (1.04) | 2.04 → 2.16 (1.06) | 4.80 → 4.91 (1.02) | 3.45 → 3.79 (1.10) |
| actorState | µs per call | 4.63 → 4.38 (0.95) | 4.34 → 4.80 (1.11) | 4.13 → 4.37 (1.06) | 3.95 → 4.20 (1.06) |
| actorContext | µs per call | 7.80 → 7.54 (0.97) | 4.95 → 4.96 (1.00) | 6.63 → 6.61 (1.00) | 4.57 → 4.35 (0.95) |
| actorGet | µs per call | 6.02 → 6.08 (1.01) | 5.90 → 5.81 (0.99) | 5.38 → 5.40 (1.00) | 4.76 → 4.81 (1.01) |
| hostStats | µs per call | 19.28 → 19.47 (1.01) | 19.32 → 19.76 (1.02) | 19.45 → 16.84 (0.87) | 14.51 → 14.14 (0.97) |
| hostConfig | µs per call | 17.54 → 17.65 (1.01) | 16.77 → 16.95 (1.01) | 13.96 → 14.44 (1.03) | 12.44 → 13.22 (1.06) |
| actorList (a page) | µs per call | 39.84 → 38.23 (0.96) | 37.99 → 37.11 (0.98) | 18.65 → 18.87 (1.01) | 20.30 → 20.31 (1.00) |
| traceConfigure | µs per call | 12.38 → 13.69 (1.11) | 12.61 → 12.46 (0.99) | 7.53 → 7.31 (0.97) | 7.71 → 7.49 (0.97) |
| traceWatch | µs per call | 14.26 → 14.27 (1.00) | 13.96 → 14.31 (1.03) | 8.06 → 8.31 (1.03) | 7.71 → 8.04 (1.04) |
| traceUnwatch | µs per call | 12.54 → 13.14 (1.05) | 12.43 → 12.89 (1.04) | 6.94 → 8.44 (1.22) | 6.85 → 6.75 (0.98) |
| traceEnd | µs per call | 3.80 → 3.85 (1.01) | 3.79 → 3.79 (1.00) | 3.88 → 4.10 (1.06) | 3.48 → 3.48 (1.00) |
| traceSpansDecode | µs per call | 4.82 → 4.79 (0.99) | 4.37 → 5.01 (1.14) | 4.10 → 4.34 (1.06) | 4.05 → 3.68 (0.91) |
| metrics | µs per call | 7.50 → 7.34 (0.98) | 6.85 → 8.05 (1.17) | 7.29 → 7.20 (0.99) | 7.21 → 6.58 (0.91) |
| I/O processor round trip | µs per call | 13.30 → 12.54 (0.94) | 8.47 → 8.33 (0.98) | 9.89 → 9.46 (0.96) | 7.88 → 6.48 (0.82) |
| transport round trip | µs per call | 34.01 → 34.13 (1.00) | 25.85 → 28.06 (1.09) | 27.99 → 31.32 (1.12) | 22.87 → 22.52 (0.98) |
| delayed send and its timer | µs per call | 7.19 → 7.48 (1.04) | 3.39 → 3.44 (1.02) | 5.93 → 5.97 (1.01) | 3.87 → 3.79 (0.98) |
| invoked service round trip | µs per call | 14.28 → 15.49 (1.08) | 10.86 → 11.78 (1.08) | 11.54 → 13.32 (1.15) | 9.84 → 10.34 (1.05) |
| actorList (every actor) | µs per actor | 0.160 → 0.154 (0.97) | 0.146 → 0.156 (1.07) | 0.101 → 0.104 (1.03) | 0.102 → 0.101 (1.00) |
| sweep (every actor) | µs per actor | 0.008 → 0.007 (0.97) | 0.007 → 0.007 (0.98) | 0.008 → 0.007 (0.94) | 0.007 → 0.008 (1.03) |
| migrate (one session) | µs per call | 32.02 → 28.74 (0.90) | 20.00 → 16.20 (0.81) | 24.48 → 24.95 (1.02) | 17.45 → 20.93 (1.20) |
| snapshot | µs per actor | 0.110 → 0.105 (0.96) | 0.075 → 0.077 (1.04) | 0.090 → 0.080 (0.89) | 0.084 → 0.082 (0.97) |
| restore (and its first turn) | µs per actor | 0.313 → 0.297 (0.95) | 0.200 → 0.200 (1.00) | 0.236 → 0.234 (0.99) | 0.193 → 0.199 (1.03) |
| close | µs per actor | 0.081 → 0.102 (1.26) | 0.078 → 0.087 (1.11) | 0.079 → 0.106 (1.35) | 0.087 → 0.104 (1.19) |

All resident: 1,000,000 and 2,000,000 actors. Two million resident actors
take up to 9.2 GB, so four million do not fit on this machine.

| operation | unit | node, native | node, quickjs | bun, native | bun, quickjs |
|---|---|---|---|---|---|
| populate (spawn, the segment growing) | µs per actor | 6.50 → 6.48 (1.00) | 4.49 → 4.17 (0.93) | 5.39 → 5.84 (1.08) | 4.55 → 4.37 (0.96) |
| spawn | µs per call | 5.36 → 5.53 (1.03) | 2.59 → 3.18 (1.23) | 4.84 → 4.93 (1.02) | 2.86 → 2.71 (0.95) |
| send | µs per call | 6.58 → 6.84 (1.04) | 4.35 → 4.88 (1.12) | 7.89 → 7.03 (0.89) | 4.55 → 4.64 (1.02) |
| sendMany | µs per event | 6.82 → 6.99 (1.02) | 4.22 → 4.68 (1.11) | 5.77 → 5.92 (1.03) | 4.11 → 4.23 (1.03) |
| terminate | µs per call | 3.00 → 3.27 (1.09) | 3.34 → 3.61 (1.08) | 3.03 → 3.40 (1.12) | 3.53 → 3.73 (1.06) |
| terminate (runnable) | µs per call | 0.958 → 0.498 (0.52) | 0.703 → 0.478 (0.68) | 0.638 → 0.882 (1.38) | 0.514 → 0.487 (0.95) |
| actorByName | µs per call | 0.538 → 0.527 (0.98) | 0.440 → 0.486 (1.11) | 0.458 → 0.443 (0.97) | 0.265 → 0.327 (1.23) |
| hostStats | µs per call | 20.07 → 27.24 (1.36) | 20.14 → 18.89 (0.94) | 15.87 → 15.60 (0.98) | 13.97 → 15.92 (1.14) |
| actorList (every actor) | µs per actor | 0.161 → 0.162 (1.01) | 0.198 → 0.192 (0.97) | 0.116 → 0.113 (0.98) | 0.144 → 0.144 (1.00) |
| sweep (a page of 256) | µs per call | 35.57 → 35.80 (1.01) | 46.83 → 46.36 (0.99) | 21.32 → 20.75 (0.97) | 30.09 → 30.97 (1.03) |
| hibernate | µs per call | 7.75 → 7.64 (0.99) | 7.68 → 7.42 (0.97) | 6.35 → 6.93 (1.09) | 7.48 → 7.82 (1.04) |
| wake | µs per call | 8.93 → 8.49 (0.95) | 3.05 → 3.27 (1.07) | 6.94 → 8.36 (1.20) | 3.93 → 3.57 (0.91) |
| snapshot | µs per actor | 6.62 → 6.46 (0.98) | 5.60 → 6.32 (1.13) | 4.59 → 2.94 (0.64) | 6.12 → 6.51 (1.06) |
| migrate (a chart, every actor) | µs per actor | 23.38 → 24.60 (1.05) | 12.26 → 14.03 (1.15) | 18.36 → 18.38 (1.00) | 12.01 → 13.27 (1.11) |
| restore (and its first turn) | µs per actor | 16.41 → 15.45 (0.94) | 4.45 → 5.46 (1.23) | 9.08 → 9.01 (0.99) | 4.75 → 4.90 (1.03) |
| close | µs per actor | 0.675 → 0.532 (0.79) | 1.78 → 1.47 (0.82) | 0.557 → 0.506 (0.91) | 1.70 → 1.40 (0.82) |

Peak resident memory, at N and 2N: hibernated node native 1718 / 3273 MB; node quickjs 1763 / 3368 MB; bun native 1396 / 2605 MB; bun quickjs 1358 / 2561 MB; all resident node native 3380 / 6493 MB; node quickjs 4236 / 8289 MB; bun native 4175 / 8176 MB; bun quickjs 4673 / 9239 MB.

Every operation costs per call what it costs at half the actors.
