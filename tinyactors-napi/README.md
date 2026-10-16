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
SQLite, a delayed send pending for each resident one; its snapshot
written to a file and its store copied; the copy restored with its own
store and clock, whose own snapshot is the same bytes, then migrated to a
new version while events come, its clock run past the delayed sends,
while the live host goes on with events of its own.
