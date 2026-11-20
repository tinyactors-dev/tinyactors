# Snapshots

This guide is part of operating a host (the README's [Operating a
host](../../README.md#operating-a-host)).

`host.snapshot()` copies the whole host into one `Buffer`: every
segment's memory (actors, queues, timers, the hibernated actors'
records), the datamodel's sessions and values, the images, the system id
and the host's time, read when you call it. `Host.restore(snapshot,
charts, options)` makes a new host running it, in this process or
another.

Uses: rehearse a deploy on a copy (add the new image, migrate, run the
clock ahead, look); capture a host when something goes wrong and step
through it; start every test from one prepared world; or take a host out
of the process for a while.

```js
import { compileCharts, Host, SimulatedClock } from '@tinyactors/node';

const compiled = compileCharts([{ name: 'reminder.scxml', text: `
<scxml xmlns="http://www.w3.org/2005/07/scxml" version="1.0"
       datamodel="ecmascript" name="reminder" initial="waiting">
  <datamodel><data id="note" expr="''"/></datamodel>
  <state id="waiting">
    <transition event="set">
      <assign location="note" expr="_event.data"/>
      <send event="ring" delay="3600s"/>
    </transition>
    <transition event="ring" target="done"/>
  </state>
  <final id="done"><onentry><log expr="'reminder: ' + note"/></onentry></final>
</scxml>` }]);

const image = compiled.image;
compiled.close();
const log = (entry) => console.log(entry.text);

const clock = new SimulatedClock();
const host = new Host(image, { system: 'demo', clock, log });

const reminder = host.spawn('reminder', { name: 'plants' });
reminder.send('set', 'water the plants');
await host.idle();
clock.increment(30 * 60_000);      // half an hour passes
await host.idle();

const snapshot = host.snapshot();  // bytes: write them to a file, send them elsewhere
host.close();

const later = new SimulatedClock(); // a clock that reads 0
const copy = Host.restore(snapshot, image, { system: 'demo', clock: later, log });
console.log('same session:', copy.actor('plants').id === reminder.id);
await copy.idle();
console.log('due in', copy.nextDeadline / 60_000, 'minutes');
later.increment(30 * 60_000);
await copy.idle();
copy.close();
```

```output
same session: true
due in 30 minutes
reminder: water the plants
```

Every session id stays the same, and the host's clock goes on from the
snapshot's time, whatever clock reads it: a new `SimulatedClock`, as
here, or the runtime's clock in another process or on another machine.
The snapshot's time is the time of the `snapshot()` call, so a timer
with 6 seconds left then has 6 seconds left after the restore, however
long the host was idle before. `host.now()` reads that clock: a journal of
the inputs that came after a snapshot records it with each, and a replay
on a restored host on a `SimulatedClock` moves the clock to each reading
(`clock.increment(entry.at - host.now())`) before it gives the input.

## Restoring

Pass `Host.restore` the charts, and the options the snapshot was taken
with: its `system` is checked, and the I/O processors, the config's tick
rate and the snapshot's `datamodel` (`'quickjs'` for a quickjs host) are
not in the snapshot. The snapshot holds its images, so the charts are
only those it does not hold: an image whose bytes are one of the
snapshot's is not added again, and a new one (the next version of a
chart, for a rehearsed deploy) is added as `new Host` adds it. The
host's handlers are not in the snapshot either: give the restored host
its own.

- **A snapshot is valid only for the same build of the package.** It
  holds raw memory, with a version of its layout: a snapshot of a build
  whose layout differs is refused by `Host.restore` with
  `TA_ERROR_INVALID`, and you take a new one with the new build. It is
  not a backup: durability is your database's
  job, and [hibernation](hibernation.md) is how a running process keeps
  actors out of memory. A snapshot is a copy of the whole application,
  every chart version at once.
- **Invoked services are started again.** The services you run are not
  in the snapshot. The restored host's first turn announces each
  invocation that was running to the `services` handler you pass to
  `Host.restore`, with `invocation.restored` true and the same
  `invokeid`, `src` and params. Pass your real handlers to go on, or
  stand-ins for a copy that must not reach the real services. A type with
  no handler gives its invokers `error.communication`, and nothing is
  thrown.
- **Hibernated actors stay in your store.** The snapshot holds their
  records, not their blobs, and the restored host loads them from the
  `store` you pass it as before. A copy that hibernates actors needs its
  own copy of the store, since it writes and deletes blobs too.
- **Values are written through the value table**, so two variables of
  one actor that shared one object no longer share it after a restore.

## When a snapshot is refused

The host must be quiescent. `snapshot()` throws a `TinyactorsError` with
`TA_ERROR_STATE` when it is called from a handler, while an effect or a
typed method's reply has not been handled yet (`await host.idle()`
first), and while a migration runs. It throws `TA_ERROR_UNSUPPORTED` when
an actor holds what hibernation cannot store (on the native datamodel a
function, a `Map`, a `Date`, ...); the message names the variable.
The host goes on either way.

```js
import { compileCharts, Host } from '@tinyactors/node';

const compiled = compileCharts([{ name: 'c.scxml', text: `
<scxml xmlns="http://www.w3.org/2005/07/scxml" version="1.0"
       datamodel="ecmascript" name="c" initial="s">
  <state id="s"><onentry><log expr="'hello'"/></onentry></state>
</scxml>` }]);
const host = new Host(compiled, {
  system: 'demo',
  log: (entry) => {
    try {
      host.snapshot();
    } catch (error) {
      console.log('from a handler:', error.status);
    }
  },
});
compiled.close();

host.spawn('c');
await host.idle();
console.log('after idle():', host.snapshot().length > 0);
host.close();
```

```output
from a handler: TA_ERROR_STATE
after idle(): true
```

## At scale

`tools/snapshot.mjs` (`mise run snapshot` in the repository) rehearses a
deploy on a copy. The live host has two million actors, `resident: 1000`
and the rest in a store over SQLite. Each resident actor has a delayed send
pending, and half of them are paying through an invoked payment service
that has not answered. The tool writes the host's snapshot to a file and
copies the store. It restores the copy with its own store and clock and
a stand-in payment service, which gets every payment in flight again
(`restored`, with its amount) and declines it; the copy's own snapshot
must be the same bytes. Then it migrates the copy to a new version while
events come and runs its clock past the delayed sends, while the live
host goes on with events of its own and its payments still in flight. The
copy never reaches the real service.

Two runs of it, each alone on the machine (2026-11-20; Intel Xeon @
2.10GHz, 4 CPUs, 16 GiB, Linux; node 22.23.5, bun 1.4.1): two million
actors, at most 1000 resident, 200,000 events before the snapshot and
200,000 more on each host after it; then 100,000 actors all resident, so
the datamodel's own state is a large part of the snapshot. Every run
passed its checks: the copy's snapshot was the same bytes, and the copy
migrated every actor to version 2 (3,688,631 loads from its store at two
million). Every sampled actor had its count, delayed send and version on
both hosts, the live store held only the live host's blobs, and after
every turn at most R actors were resident.

| runtime, datamodel | actors, resident | snapshot | taken | restored | migrate on the copy | peak resident (both hosts) |
|---|---|---|---|---|---|---|
| node, native | 2,000,000, 1000 | 165.0 MB (82 B per actor) | 0.14 s | 0.14 s | 244.9 s | 1261 MB |
| node, quickjs | 2,000,000, 1000 | 164.9 MB (82 B per actor) | 0.14 s | 0.14 s | 205.7 s | 1177 MB |
| bun, native | 2,000,000, 1000 | 165.0 MB (82 B per actor) | 0.12 s | 0.21 s | 208.1 s | 1534 MB |
| bun, quickjs | 2,000,000, 1000 | 164.9 MB (82 B per actor) | 0.11 s | 0.19 s | 185.9 s | 1559 MB |
| node, native | 100,000, 100,000 | 69.5 MB (695 B per actor) | 0.65 s | 2.00 s | 5.0 s | 522 MB |
| node, quickjs | 100,000, 100,000 | 63.7 MB (637 B per actor) | 1.01 s | 0.67 s | 3.4 s | 724 MB |
| bun, native | 100,000, 100,000 | 69.5 MB (695 B per actor) | 0.51 s | 1.05 s | 4.2 s | 941 MB |
| bun, quickjs | 100,000, 100,000 | 63.7 MB (637 B per actor) | 1.06 s | 0.71 s | 3.5 s | 800 MB |

Core's region is 82 B an actor with most of them hibernated and 490 B
all resident; the native datamodel's rows take about 58 B an actor more
than quickjs's state in a snapshot of resident actors. The logs are in
`tools/results/2026-11-20/` in the repository (`snapshot.log`,
`snapshot-resident.log`), and the root `AGENTS.md` ("Published
numbers") says when to run it again.
