# Inspecting a host

A running host answers questions about itself: which state an actor is
in, what its variables hold, which actors exist, what the host has done
so far. These are the **typed methods**, one per question. Each takes its
params, a handler for the result and, optionally, one for an error. The
method reads the host as it is when you call it, and calls one of the
handlers in a later turn.

```js
import { compileCharts, Host } from '@tinyactors/node';

const compiled = compileCharts([{ name: 'order.scxml', text: `
<scxml xmlns="http://www.w3.org/2005/07/scxml" version="1.0"
       datamodel="ecmascript" name="order" initial="open">
  <datamodel>
    <data id="items" expr="[]"/>
  </datamodel>
  <state id="open" initial="browsing">
    <state id="browsing">
      <transition event="add"><script>items.push(_event.data)</script></transition>
    </state>
  </state>
</scxml>` }]);
const host = new Host({ systemID: 'shop' });
host.addImage(compiled.image);
compiled.close();

const order = host.spawn(host.chart('order'), { name: 'first' });
host.send(order, 'add', 'tea');
await host.idle(); // the actor has started and handled 'add'

host.actorState({ session: order }, (host, result) => console.log('state:', JSON.stringify(result.value)));
host.actorContext({ session: order }, (host, result) => console.log('variables:', JSON.stringify(result.variables)));
host.actorGet({ session: order }, (host, result) => console.log('actor:', result.chart, 'mailbox', result.mailbox));
console.log('asked');
await host.idle();
host.close();
```

```output
asked
state: {"open":"browsing"}
variables: {"items":["tea"]}
actor: order mailbox 0
```

The results came after `asked`: a typed method returns nothing, and its
handler runs in a later turn. Asked before the `await host.idle()`, the
same methods would have found an actor that had not started yet: no
active state, and its variables not yet set. An actor's state reads as nested objects,
one key per compound state: `{"open": "browsing"}` is the state
`browsing` inside `open`, and a parallel state is `{"p": {"r1": ...,
"r2": ...}}`, one key per region.

## The methods

| Method | Params | Result |
|---|---|---|
| `actorState` | `{ session }` | `{ value }`: the active states, as above |
| `actorContext` | `{ session }` | `{ variables }`: each `<data>` and its value |
| `actorGet` | `{ session }` | `{ session, segment, chart, parent, mailbox, invocations }` |
| `actorList` | `{ cursor, limit }`, both optional | `{ sessions, cursor }`: a page of sessions; `cursor` is `null` after the last page |
| `hostStats` | none | `{ host, pools, charts }`: counters ([below](#the-hosts-counters)) |
| `hostConfig` | none | the config the host runs with, every default filled in |
| `traceConfigure`, `traceWatch`, `traceUnwatch`, `traceEnd`, `traceSpansDecode` | | [Telemetry](telemetry.md) |

A method without params takes `undefined`: `host.hostStats(undefined,
onResult)`. The host checks the params: one the method does not take, of
the wrong type or out of range is an error, as is a session that names no
actor (it ended, or never was). Sessions in a result are numbers, and the
inputs take them back as they are (`host.send(result.sessions[0], ...)`).

`actorContext` is refused for a hibernated actor: wake it first
([Hibernation](hibernation.md)).

## Errors

An error goes to the third argument, `onError(host, error)`, a
`TinyactorsError` with `code` (JSON-RPC's number), `reason` (a word such
as `stale`, `type`, `unknown`) and `path` (the param it is about), and a
message for people. Without an `onError`, the error rejects a waiting
`host.idle()`, or else is thrown as an uncaught exception, as a handler's
error is.

```js
import { Host } from '@tinyactors/node';

const host = new Host({ systemID: 'shop' });
host.actorState({ session: 99 }, () => {}, (host, error) => {
  console.log(error.reason, error.path, error.code);
});
host.actorGet({ name: 'first' }, () => {}, (host, error) => {
  console.log(error.reason, error.path);
});
await host.idle();
host.close();
```

```output
stale params.session -32001
unknown params.name
```

`host.close()` drops the handlers of methods still waiting, without
calling them.

## As promises

The methods take handlers so that nothing waits in the host on your
behalf. A promise is a few lines of your own:

```js
import { compileCharts, Host } from '@tinyactors/node';

const ask = (host, method, params) =>
  new Promise((resolve, reject) => host[method](params, (host, result) => resolve(result), (host, error) => reject(error)));

const compiled = compileCharts([{ name: 'idle.scxml', text: `
<scxml xmlns="http://www.w3.org/2005/07/scxml" version="1.0"
       datamodel="ecmascript" name="idle"><state id="waiting"/></scxml>` }]);
const host = new Host({ systemID: 'shop' });
host.addImage(compiled.image);
compiled.close();
for (let i = 0; i < 5; i++) host.spawn(host.chart('idle'));

const sessions = [];
let cursor = 0;
do {
  const page = await ask(host, 'actorList', { cursor, limit: 2 });
  sessions.push(...page.sessions);
  cursor = page.cursor;
} while (cursor !== null);
console.log('sessions:', sessions.join(' '));
host.close();
```

```output
sessions: 1 2 3 4 5
```

## The host's counters

`hostStats` answers three parts:

- `host`: `segments` (live), `actors` and `hibernated` (now),
  `runnable` (actors with work waiting), `migrations` (running),
  `regionBytes` (the memory of the live segments), `delivered` (events
  put in queues), `routed` (sends between the host's segments),
  `migrated` (actors moved), `retired` (segments freed after a
  migration), and `fuelClock` (the work the host has done, the clock its
  trace records are stamped with).
- `pools`: an array with one entry per live segment, in segment order.
  Each entry has its `segment` number and one object per pool, `actors`,
  `events`, `timers`, `workspaces`, `requests` and `slabBytes`, each
  `{ capacity, used, highWater }`: `stats.pools[0].actors.used`
  ([Capacity and memory](capacity.md)).
- `charts`: one entry per chart name, summed over its versions: `name`,
  `actors`, `hibernated`, `spawned`, `ended`, `turns`, `macrosteps`,
  `fuel`, `errorsExecution`, `errorsCommunication`, `errorsPlatform`,
  `faults`.

The words, as the counters use them:

- A **macrostep** is everything one event does to an actor: its
  transitions and those that follow on their own, until the actor waits
  for the next event. A **microstep** is one set of transitions taken
  together. Spans are named after both ([Telemetry](telemetry.md)).
- **Fuel** is the host's unit of work: an action, a transition, and a
  share of the JavaScript an expression runs. A **turn** is one time an
  actor is given the processor; it runs until it waits or has spent its
  fuel for the turn (`system.quantum`), so a busy actor cannot starve the
  others.
- A **workspace** is the room an actor needs while it runs; the host has
  a few and an actor waits for one.
- `errorsPlatform` counts `error.platform` events raised in the chart's
  actors; `faults` counts actors the host
  had to end because their run failed inside the host.

```js
import { compileCharts, Host } from '@tinyactors/node';

const compiled = compileCharts([{ name: 'job.scxml', text: `
<scxml xmlns="http://www.w3.org/2005/07/scxml" version="1.0"
       datamodel="ecmascript" name="job" initial="running">
  <state id="running">
    <transition event="finish" target="done"/>
    <transition event="break"><assign location="undeclared" expr="1"/></transition>
  </state>
  <final id="done"/>
</scxml>` }]);
const host = new Host({ systemID: 'shop' });
host.addImage(compiled.image);
compiled.close();

const jobs = [1, 2, 3].map(() => host.spawn(host.chart('job')));
host.send(jobs[0], 'finish');
host.send(jobs[1], 'break');
await host.idle();
host.hostStats(undefined, (host, stats) => {
  const [job] = stats.charts;
  console.log(`${job.name}: ${job.spawned} spawned, ${job.ended} ended, ${job.actors} running, ${job.errorsExecution} execution error`);
  console.log(`${stats.host.segments} segment, ${stats.pools[0].actors.used} actors in its pool`);
});
await host.idle();
host.close();
```

```output
job: 3 spawned, 1 ended, 2 running, 1 execution error
1 segment, 2 actors in its pool
```
