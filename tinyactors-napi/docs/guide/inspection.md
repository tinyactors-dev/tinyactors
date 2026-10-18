# Inspecting a host

A running host answers questions about itself: which state an actor is
in, what its variables hold, which actors exist, what the host has done
so far. These are the **typed methods**, one per question. Each reads
the host as it is when you call it and answers in a later turn, so each
returns a promise: `await host.actorStateAsync({ session })`.

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

const { value } = await host.actorStateAsync({ session: order });
console.log('state:', JSON.stringify(value));
const { variables } = await host.actorContextAsync({ session: order });
console.log('variables:', JSON.stringify(variables));
const actor = await host.actorGetAsync({ session: order });
console.log('actor:', actor.chart, 'mailbox', actor.mailbox);
host.close();
```

```output
state: {"open":"browsing"}
variables: {"items":["tea"]}
actor: order mailbox 0
```

Asked before the `await host.idle()`, the same methods would have found
an actor that had not started yet: no active state, and every variable
`undefined`, the spawn's `params` included. An actor's `<data>` are
given their values, from `expr` or from `params`, when it first runs, in
the host's next turn after the spawn. A variable that holds `undefined`
reads `undefined` and one that holds `null` reads `null`, on both
datamodels. An actor's state reads as nested objects, one key per
compound state: `{"open": "browsing"}` is the state `browsing` inside
`open`, and a parallel state is `{"p": {"r1": ..., "r2": ...}}`, one
key per region.

## The methods

| Method | Params | Result |
|---|---|---|
| `actorStateAsync` | `{ session }` | `{ value }`: the active states, as above |
| `actorContextAsync` | `{ session }` | `{ variables }`: each `<data>` and its value |
| `actorGetAsync` | `{ session }` | `{ session, segment, chart, parent, mailbox, invocations }` |
| `actorListAsync` | `{ cursor, limit }`, both optional | `{ sessions, cursor }`: a page of sessions; `cursor` is `null` after the last page |
| `hostStatsAsync` | none | `{ host, pools, charts }`: counters ([below](#the-hosts-counters)) |
| `hostConfigAsync` | none | the config the host runs with, every default filled in |
| `traceConfigureAsync`, `traceWatchAsync`, `traceUnwatchAsync`, `traceEndAsync`, `traceSpansDecodeAsync` | | [Telemetry](telemetry.md) |

A method without params is called with none: `await
host.hostStatsAsync()`. The host checks the params: one the method does
not take, of the wrong type or out of range is an error, as is a session
that names no actor (it ended, or never was). Sessions in a result are
numbers, and the inputs take them back as they are
(`host.send(result.sessions[0], ...)`).

Each method's params and result are typed in the package's `index.d.ts`
(`SessionParams`, `ActorStateResult`, `ActorListResult`,
`HostStatsResult`, `EffectiveHostConfig`, ...), for the promise forms and
the handlers' `result` alike, and so is the host's config (`HostConfig`).
An editor completes `stats.pools[0].actors.highWater`, and TypeScript, or
`tsc` checking JavaScript, refuses a field a result does not have or
a param a method does not take. Every example of these guides is checked
that way against the package's types, the one below included.

`actorContextAsync` is refused for a hibernated actor: wake it first
([Hibernation](hibernation.md)).

A page at a time, `actorListAsync` lists every actor:

```js
import { compileCharts, Host } from '@tinyactors/node';

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
  const page = await host.actorListAsync({ cursor, limit: 2 });
  sessions.push(...page.sessions);
  cursor = page.cursor;
} while (cursor !== null);
console.log('sessions:', sessions.join(' '));
host.close();
```

```output
sessions: 1 2 3 4 5
```

## Errors

An error rejects the promise with a `TinyactorsError` with `code`
(JSON-RPC's number), `reason` (a word such as `stale`, `type`,
`unknown`) and `path` (the param it is about), and a message for people.
A call the host refuses at once (a closed host) rejects too.

```js
import { Host } from '@tinyactors/node';

const host = new Host({ systemID: 'shop' });
try {
  await host.actorStateAsync({ session: 99 });
} catch (error) {
  console.log(error.reason, error.path, error.code);
}
// @ts-expect-error: a param actorGet does not take, which the types refuse too
await host.actorGetAsync({ name: 'first' }).catch((error) => console.log(error.reason, error.path));
host.close();
```

```output
stale params.session -32001
unknown params.name
```

The rejection is yours to handle: `host.idle()` does not see it, and one
you do not catch is an unhandled rejection, which ends a Node.js or Bun
process by default. `host.close()` drops the methods still waiting:
their promises never settle.

## With handlers

Each method also has a form that takes handlers instead of returning a
promise: `host.actorState(params, onResult, onError)`, and so on, without
`Async`. It returns nothing; `onResult(host, result)` is called in a
later turn, or `onError(host, error)` with the same `TinyactorsError`.
The promise forms are written over these, and a caller that passes
handlers pays for no promise. A method without params takes `undefined`:
`host.hostStats(undefined, onResult)`.

```js
import { compileCharts, Host } from '@tinyactors/node';

const compiled = compileCharts([{ name: 'idle.scxml', text: `
<scxml xmlns="http://www.w3.org/2005/07/scxml" version="1.0"
       datamodel="ecmascript" name="idle"><state id="waiting"/></scxml>` }]);
const host = new Host({ systemID: 'shop' });
host.addImage(compiled.image);
compiled.close();
const session = host.spawn(host.chart('idle'));
await host.idle();

host.actorState({ session }, (host, result) => console.log('state:', JSON.stringify(result.value)));
host.hostStats(undefined, (host, stats) => console.log('actors:', stats.host.actors));
host.actorGet({ session: 99 }, () => {}, (host, error) => console.log('error:', error.reason));
console.log('asked');
await host.idle();
host.close();
```

```output
asked
state: "waiting"
actors: 1
error: stale
```

The handlers ran after `asked`, in a later turn. Without an `onError`,
an error rejects a waiting `host.idle()`, or else is thrown as an
uncaught exception, as a handler's error is.

## The host's counters

`hostStatsAsync` answers three parts:

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
const stats = await host.hostStatsAsync();
const [job] = stats.charts;
console.log(`${job.name}: ${job.spawned} spawned, ${job.ended} ended, ${job.actors} running, ${job.errorsExecution} execution error`);
console.log(`${stats.host.segments} segment, ${stats.pools[0].actors.used} actors in its pool`);
host.close();
```

```output
job: 3 spawned, 1 ended, 2 running, 1 execution error
1 segment, 2 actors in its pool
```
