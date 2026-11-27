# Designing actors

Most of an application's design is deciding which actors exist, who
creates them, what they are called and how they end. The XML comes
after. These principles come from applications built on tinyactors
(granary, which closes GitHub issues from unknown users; the chat-rooms
and order-fulfillment examples; mcp.tinyactors.dev's own tracker), and
each says why.

Model real entities: an order, a payment, a subscription, a checkout, a
support ticket, a chat session. If a design needs a door or a traffic
light to explain it, it is not yet about your domain.

## 1. What becomes an actor

**An entity with a lifecycle is an actor; data without behaviour stays
inside one.** An issue that is reported, triaged, fixed and maybe
regresses is an actor. A user who is only a login in a list is not: the
list is a variable of the actor that answers "is this login allowed?".
The test is whether the thing has states something waits on.

**Shared policy is one singleton** that others ask: an allowlist, a
rate limiter, a "remediate one thing at a time" worker. Name it plainly
(`allowlist`, `tracker`), have it answer to `_event.origin`, and give the
asker a timeout.

**An occurrence is data, not an actor.** A second report of the same
problem is an entry in the issue's list of occurrences; a line of an
order is an entry in the order. Make an actor only for what has its own
lifecycle.

**Something you can never send to again is not an actor.** An agent's
session, an HTTP request that will be gone in a second: keep what you
need from it (its id, a link) as data in the actor that outlives it.

## 2. Names are stable domain keys

Name an actor by the key the domain will never change: a repository's
numeric id, not its `owner/name`, which can be renamed; an order number,
not a customer's email. Build names in one function, from the kind and
the ids joined by `.` (`issue.1002.17`, `room.acme.general`), using only
letters, digits, `-`, `.`, `_` and `~`, so an address holds the name as
it is: `tinyactors://<system>/@issue.1002.17`.

Name the actors that others must find; leave the rest unnamed and pass
their address (`_event.origin`, `spawned`'s `address`).

## 3. Ownership: who decides which actors exist

**A parent is the actor that decides which children exist**, not a
container. It is a *collection actor*: the only one that spawns those
names (through the `Spawner` I/O processor), one request at a time, and it
records the spawns in flight, keyed by the spawn's `sendid`, so it never
asks twice for a name. The host does not stop two actors from spawning
one name; one owner per name is your design's rule.

Hierarchies stay shallow: a reconciler or a singleton, then per-item
actors, sometimes one more level. Actors find each other by name, not by
holding references.

```
tracker                         one; owns which projects exist (from GitHub's repository list)
└── project.<repoId>            owns which issues exist, numbers them, finds duplicates
    └── issue.<repoId>.<n>      one issue's lifecycle
        └── upload.<...>        one attachment on its way; ends
auth                            owns logins and users
├── login.<state>               one sign-in; ends
└── user.<githubId>             roles, grants, news
    └── grant.<id>              one client's tokens
```

A collection actor that spawns tickets by name, refuses a duplicate, and
answers once the spawn is done:

```js
import { compileCharts, Host, Spawner } from '@tinyactors/node';

const desk = `
<scxml xmlns="http://www.w3.org/2005/07/scxml" version="1.0"
       datamodel="ecmascript" name="desk" initial="open">
  <datamodel>
    <data id="tickets" expr="({})"/>    <!-- number -> true, from the moment its spawn is sent -->
    <data id="creating" expr="({})"/>   <!-- spawn sendid -> {number, asker} -->
    <data id="sid"/>
  </datamodel>
  <state id="open">
    <!-- a macrostep is atomic: the number is taken in the step that asks for the spawn -->
    <transition event="open" cond="_event.data.number in tickets">
      <send targetexpr="_event.origin" event="refused"><param name="reason" expr="'exists'"/></send>
    </transition>
    <transition event="open">
      <send type="spawn" event="spawn" idlocation="sid">
        <content expr="({ chart: 'ticket', name: 'ticket.' + _event.data.number,
                          params: { number: _event.data.number } })"/>
      </send>
      <assign location="tickets[_event.data.number]" expr="true"/>
      <assign location="creating[sid]" expr="({ number: _event.data.number, asker: _event.origin })"/>
    </transition>
    <transition event="spawned" cond="_event.sendid in creating">
      <send targetexpr="creating[_event.sendid].asker" event="opened">
        <param name="address" expr="_event.data.address"/>
      </send>
      <script>delete creating[_event.sendid]</script>
    </transition>
    <transition event="error.communication" cond="_event.sendid in creating">
      <send targetexpr="creating[_event.sendid].asker" event="refused"><param name="reason" expr="'could not start'"/></send>
      <script>delete tickets[creating[_event.sendid].number]; delete creating[_event.sendid]</script>
    </transition>
  </state>
</scxml>`;
const ticket = `
<scxml xmlns="http://www.w3.org/2005/07/scxml" version="1.0"
       datamodel="ecmascript" name="ticket" initial="open">
  <datamodel><data id="number"/></datamodel>
  <state id="open"/>
</scxml>`;

const compiled = compileCharts([{ name: 'desk.scxml', text: desk }, { name: 'ticket.scxml', text: ticket }],
  { warningsAsErrors: true });
const host = new Host(compiled, { system: 'support', ioProcessors: { spawn: new Spawner() } });
compiled.close();
const support = host.spawn('desk', { name: 'desk' });
console.log(await support.ask('open', { number: 4711 }));
console.log(await support.ask('open', { number: 4711 }));
console.log(host.actor('ticket.4711').address.text);
host.close();
```

```output
[ 'opened', { address: 'tinyactors://support/#_scxml_2' } ]
[ 'refused', { reason: 'exists' } ]
tinyactors://support/#_scxml_2
```

A request that comes while a spawn is in flight must not be lost: either
refuse it (as above, the name is taken in the same macrostep) or keep it
with the spawn's record and forward it on `spawned`.

## 4. Two lifetimes

- **Long-lived actors never end.** An account, a project, a room, an
  issue that can come back: give the chart no top-level `<final>`
  (closed, declined, deleted are states). Such an actor is always there to
  send to by name, and with a store and a `resident` bound an idle one
  costs only its blob ([choices.md](choices.md)).
- **Per-occurrence actors end with donedata.** A login, a payment
  attempt, an upload, a delivery: they reach a `<final>` whose
  `<donedata>` says the outcome, which `onDone` or `actor.done` gives
  your program. The host does not give you the actor's name at the end,
  so put the key you need in the donedata.

## 5. A send to an actor that is not there

Choose per kind of actor (the guide `docs/guide/missing-actors.md` shows
each):

| The actor | The way |
|---|---|
| long-lived, a known set | it never ends and hibernates: a send to its name wakes it |
| made on demand, or not an actor (a user who may be offline) | your I/O processor routes the send, or the collection actor makes it |
| short-lived, ends on its own | the sender handles `error.communication`, which a send to a name nobody holds gets at once |

## 6. Charts are pure; effects go through I/O processors

A chart never touches a database, a file or the network. Every effect is
a `<send type="...">` to an I/O processor (or an `<invoke>` of a service),
so it shows in traces, is replaced by a stand-in in tests, and is the one
place to look for "who wrote this row".

- **Write before the effect.** Persist the intent (an outbox row, a
  ledger entry) before the side effect, then answer: a crash between the
  two is then repaired by replaying the row.
- **Make effects idempotent.** Key them (`close:<repo>:<number>`), so
  a retry finds the work already done. Delivery into an actor is at least
  once; an effect outward should happen at most once per key.
- **Answer to `_event.origin`** (or a `replyTo` the request carries when
  it was forwarded), so one chart serves another actor, `ask` and another
  system alike.

## 7. The truth, per entity, and a crash table

Decide in writing where each entity's truth lives, and what each state
does after a restart:

- **The host is the truth** (a snapshot on a timer and at shutdown, the
  store's blobs beside it, restored at start): the simplest, and it keeps
  timers and services.
- **Your database is the truth** and actors are rebuilt from rows at
  start (spawn params from the row, a first state that routes on a
  `phase`): what granary chose, and the choice when other programs read
  the data.

For each state that does I/O, write one row: "crash while charging: on
restart, the outbox row exists, so retry with the same key".

## 8. Time

- **Every wait has a timeout**, as a delayed send cancelled by the answer.
- **Schedules are derived from facts, not trusted to survive.** The next
  run is due at the last run's start plus the interval; missed windows
  collapse into one catch-up run. A delayed send is a convenience; the
  fact it is computed from is the truth.
- **Pull complements push.** Webhooks get lost; a periodic pull with a
  checkpoint (an ETag, the last commit seen) catches what was missed.

## 9. Duplicates and late answers are states

A second "opened" for an issue being closed, an answer after its
timeout, a report of a problem already fixed: give each a transition (or
a state that ignores it) instead of treating it as an error. Write
answers so that a late one is recognized (`_event.sendid`).

## 10. The protocol is one catalogue

List every event in one table: its name, its data, who sends it to whom.
Validate data where it enters (your I/O processors, your HTTP handlers);
charts may trust data from a validated sender. Name answers so that
they do not extend the request's name (`listed` for `list`), because
event names match by prefix ([pitfalls.md](pitfalls.md)).

## 11. Every I/O processor gets a stand-in

The processor boundary is the test boundary: each I/O processor is an
interface, and its tests use a stand-in that records what it was asked,
answers as the test decides, and can inject faults (a 503, silence, a
refusal). [testing.md](testing.md).

## Examples to read

- `tinyactors-napi/examples/chat-rooms/`: workspaces (collection actors)
  and rooms that never end and hibernate, connections that end, the
  Spawner, all three ways of a missing actor.
- `tinyactors-napi/examples/order-fulfillment/`: payment and shipping as
  invoked services, timeouts, retries, cancels.
- `tinyactors-napi/examples/subscription-billing/`: a live population
  migrated to a new chart version.
- `tinyactors-support/charts/` in the monorepo: the tracker above, with
  the Spawner, `ask`, a decision model as an I/O processor, uploads.
