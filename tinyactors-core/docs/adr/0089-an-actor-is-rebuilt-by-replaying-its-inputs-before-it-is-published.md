# ADR 0089: An actor is rebuilt by replaying its inputs before it is published

**Status:** Accepted and implemented (2026-09-29); see the implementation notes. Depends on ADR 0088. Amends:
- ADR 0037: a prepared actor needs no incumbent, and core supports replay without owning a log or a replay mode;
- ADR 0012: a replaying actor has its own clock;
- ADR 0081: an invoker's error report waits for the macrostep to end;
- the invoker contract: a start's failure is reported, not answered;
- the generation of invoke IDs, which moves from a counter per system to one per session.

It would extend ADR 0072: a resume says whether its event was ignored. It would replace ADR 0084 section 7 (withdrawal by due time). It serves `docs/proposals/replay-persistence.md`, and feedback from granary, an application built on `@tinyactors/node`. ADR 0090 moves the recording and the replay into core (a host hands `ta_actor_replay()` records instead of calling the input functions), and ADR 0091 lets a replay start from a checkpoint.
**Date:** 2026-09-29

## Context

Replay-based persistence rebuilds a named actor from its history instead
of from a snapshot. A session image can't do this job. It belongs to one
system (ADR 0074), and it can only be restored against the revision it
was captured under (ADR 0038). History has neither limit, and a new
revision can read old history, so an upgrade needs no migration. granary
tried to build replay persistence on napi. They gave up after a second
"scratch" system with dummy I/O processors, and the actor's configuration
lost on the way back.

### The requirement

Replaying an actor's history under the **same** revision must rebuild
exactly the state the actor had. Under a new revision, the same history
may lead elsewhere: that is how an upgrade works.

### The border

SCXML separates a session from the outside world with I/O processors and
invokers. Traffic across that border is either:
- **outward: effects.** Sends, invocation starts and cancels, forwarded
  events, logs;
- **inward: inputs.** External events, invoker reports, and the time.

If every inward crossing is logged, a replay only has to cross nothing
outward. The chart then reaches the same internally consistent state, and
resumes contact with the world when it is published.

A synchronous answer from an adapter is also an inward crossing, because
it enters the macrostep that made the call. In napi the synchronous part
of each adapter call is mostly "arrange for this to happen":
- an I/O processor's `send` queues an effect (`src/adapters/io.c`). A
  failure of the JavaScript processor comes back later, as an external
  `error.communication` event (`lib/system.js`);
- the JavaScript invoker's `start` queues an effect
  (`src/adapters/invoker.c`).

Two synchronous answers carry information:
- **The router's ERROR** for a target that cannot be reached. SCXML
  requires it to be raised in the macrostep that sends (C.1, IRP tests
  496 and 521). Under ADR 0088 a session ID names its family, so every
  destination has one, and for a family with a loader the router never answers ERROR: its loader
  decides, and a failure becomes an asynchronous dead letter (napi ADR
  0022). ERROR remains only for families with no loader, and for sessions
  that never existed.
- **An invoker's `start` failure.** napi's SCXML invoker compiles and
  spawns the child inside `start`, and a failure raises `error.execution`
  in the macrostep (`ta_invoke_start`). SCXML leaves the start of a service
  to the platform (6.4.3). Only an error while evaluating `<invoke>`'s own
  arguments must refuse it at once, and that is core's own, deterministic
  decision. A start failure can therefore be reported like any other
  service failure (ADR 0081).

### What core already has

Upgrading by replacement (ADR 0037) built an actor that runs off-route:
- **`ta_actor_prepare_replacement()`** makes an *unpublished* actor under
  the incumbent's address, one the directory does not know;
- **`ta_actor_deliver()` and `ta_actor_resume()`** accept it;
- **its effects are held** (`ta_activation_effects`):
  - its sends and logs are held;
  - its invocations are not started;
  - `<cancel>` reaches a held send;
- **`ta_actor_activate_replacement()`** swaps the directory entry and
  releases what was held.

What is missing:
- an actor with no incumbent;
- a clock of its own;
- crossing nothing, instead of holding effects for later;
- invoke IDs that a replay reproduces. `system->next_invoke_serial` is
  shared by the whole system;
- inputs that land only between macrosteps. An invoker's error report
  joins a macrostep paused by a budget (ADR 0081), at a point the log
  cannot record.

ADR 0037 says there is "no core migration language, replay mode,
event-log requirement, or universal effect/outbox subsystem". This ADR
keeps that: the log, its storage, and the order in which it is replayed
belong to the host.

## Decision

### 1. A prepared actor needs no incumbent

| Today | Becomes |
|---|---|
| `ta_actor_prepare_replacement()` | `ta_actor_prepare()` |
| `ta_actor_prepare_replacement_params { incumbent, definition, datamodel_binding, out }` | `ta_actor_prepare_params { incumbent, address, definition, datamodel_binding, at, trace, out }` |
| `ta_actor_activate_replacement()` | `ta_actor_publish()` |
| `ta_actor_activate_replacement_params { incumbent, replacement }` | `ta_actor_publish_params { actor, incumbent }` |

All of these change in place, and every caller migrates: napi's
`prepareReplacement` and `activateReplacement`, and the test host's
bindings and scenarios. "Published" is already core's word for an actor
the directory knows (`actor->published`).

- **With an incumbent** (`incumbent` ≠ 0), nothing changes: publishing swaps
  the incumbent's `RESIDENT` entry for the new actor.
- **Without one**, `address` names the address, in the definition's family.
  Publishing claims the entry if there is none, or replaces it if it is
  `UNLOADED`, in one atomic directory call. If the entry is `RESIDENT`, the
  answer is `TA_ERR_BUSY`.
- **An actor that finished before it was published** is refused with
  `TA_ERR_DONE`. The host destroys it and records that the address is done.

### 2. A replaying actor has its own clock

An actor prepared with `at` (non-null) is **replaying** until it is
published. Its clock starts at `at`:

```c
/* Moves a replaying actor's clock forward: t may not be earlier than its
 * clock or later than ta_system_time(). Only while it is idle. */
TA_API ta_result ta_actor_set_time(ta_system *system, ta_actor_id actor, ta_time t);
```

Everything in the actor that reads "now" reads its own clock instead:
delayed-send due times, trace records' `now`, and `ta_dm_host_time()`
(section 9). The host sets the clock to each input's logged time before
replaying that input. The call answers:
- `TA_ERR_INVALID` for an actor that is not replaying, or for a `t` that
  breaks the rule above;
- `TA_ERR_BUSY` while the actor is inside a macrostep.

**Moving the clock to t drops every held delayed send due at or before
t.** In the live run, time reached those sends, so they fired. One of two
things happened:
- it fired **before** the input at t. If it was sent to the actor itself,
  its delivery is in the log and will be replayed. If it was sent to
  another actor, it has left;
- the host delivered the input first and dispatched the send afterwards.
  napi runs deliveries before `ta_system_dispatch_due()` in a turn, and a
  dispatch budget can postpone a send to a later turn. A `<cancel>` in the
  meantime could still remove it. Either way the send is gone afterwards:
  removed, or fired, and logged if it went to the actor itself. So
  dropping it before the macrostep leaves the same pending sends.

**The last clock move is to the moment the actor stopped.** Before
publishing, the host moves the clock to the last time it knows the
actor's due sends had been dispatched:
- **after an eviction:** the eviction time. The host dispatches the actor's
  due sends, evicts it, and logs the time;
- **after a crash:** the last committed **dispatch horizon**. After each
  `ta_system_dispatch_due()` the host takes `min(now, next deadline − 1)`
  (`ta_system_next_deadline()`), so a turn cut short by its dispatch budget
  does not pass a send it did not dispatch. It commits the horizon with the
  turn's inputs, once per system per turn.

Sends due by then fired and are dropped. Sends due later are still held
at publish and fire at the next dispatch, late, as after an unload and a
load (ADR 0074). A logical-time timer that is due is delivered as soon as
its actor exists again. The clock never moves past a send that was not
dispatched, so nothing is lost. The one imprecision runs the other way.
A send dispatched after the last committed horizon, just before a crash,
fires again at publish. That is the at-least-once window of any effect
released before its turn was committed (see Scope), and there is nothing
special about timers.

#### Example timelines

A timeout sent to the actor itself. Its firing comes back as an input:

```
Live                                          Log
t0   deliver e1: arms #_send.1 to self, due d  deliver e1 @ t0
d    dispatch #_send.1: posted to self
d'   deliver timeout (sendid #_send.1), d' ≥ d  deliver timeout @ d'
E    evicted, due sends dispatched               evicted @ E

Replay, a week later
set_time(t0); deliver e1    #_send.1 held, due d
set_time(d')                d ≤ d': #_send.1 dropped (it fired)
deliver timeout             the chart takes its timeout, as it did live
set_time(E); publish        nothing held, nothing fires twice
```

A send to another actor B, after the last input. B never answers, so
the sender's log holds nothing after it:

```
Live, case 1: it fired before the actor stopped
t0   deliver e1: arms #_send.2 to B, due d      deliver e1 @ t0
d    dispatch #_send.2: posted to B
E    evicted, E ≥ d                              evicted @ E

Replay
set_time(t0); deliver e1    #_send.2 held, due d
set_time(E)                 d ≤ E: dropped (B already has it)
publish                     nothing fires

Live, case 2: the actor stopped first
t0   deliver e1: arms #_send.2 to B, due d      deliver e1 @ t0
E    evicted, E < d                              evicted @ E

Replay, a week later (now ≫ d)
set_time(t0); deliver e1    #_send.2 held, due d
set_time(E)                 d > E: still held
publish                     #_send.2 scheduled; past due, it fires at the
                            next dispatch, once, a week late
```

A crash between two commits:

```
Live
t0   deliver e1: arms #_send.3 to B, due d      deliver e1 @ t0
H    turn committed, horizon H < d               horizon H
d    dispatch #_send.3: posted to B
     crash, before the next commit

Replay
set_time(t0); deliver e1    #_send.3 held, due d
set_time(H)                 d > H: still held
publish                     #_send.3 fires again: at least once, as for
                            any effect released after the last commit
```

### 3. A replaying actor crosses nothing

Core calls no adapter for a replaying actor:
- **Immediate sends are dropped.** Their `SENT` record is written, and the
  chart sees them as sent. A send to a family with a loader was never
  refused in the live run (ADR 0088), so "sent" is what happened.
- **Delayed sends are held** with their due time on the actor's clock, and
  `<cancel>` still removes them.
- **Invocations become active in core** under their generated IDs (section
  5), without calling the invoker. Events the original service sent carry
  the same IDs, so when they are replayed as deliveries, `<finalize>` runs
  and `done.invoke` retires the invocation. Core calls neither `cancel` nor
  `forward` for these invocations. Autoforwarding to an invoked SCXML child
  is a send, and is dropped like one.
- **`<log>` records are dropped.** The live run wrote them.

### 4. Inputs arrive only between macrosteps

Every input is a call the host makes on an idle actor, and the host
queues an input until the actor is idle, as it already does for mail.
One mechanism serves all inputs, and it is the host's mailbox:
- `ta_actor_deliver()` answers `TA_ERR_BUSY` inside a macrostep, as today.
- **`ta_actor_report_invoke()`** answers `TA_ERR_BUSY` inside a macrostep,
  whatever the report's kind. This reverses ADR 0081's "never answers
  TA_ERR_BUSY".
- **An invoker's `start` answers `TA_OP_OK` or `TA_OP_NOMEM`.** A start
  that fails is reported with an ERROR report, which raises
  `error.communication.invoke.<id>.<reason>` (ADR 0081). The rest of
  `<invoke>` stays with core:
  - an error in evaluating its arguments still refuses it with
    `error.execution` (SCXML 6.4);
  - so does an unregistered type, which core decides from its registry.
- The router keeps ERROR. Under ADR 0088, it is raised only for families
  with no loader and for sessions that never existed.

The log is then the list of input calls core accepted, each with its
time: deliveries, reports, and the host's operations (section 7).

### 5. Generated IDs come from one counter per session

A generated invoke ID's `platformid` comes from the session's
generated-ID counter (`next_send_id`), which generated send IDs already
use. SCXML requires it to be unique only "within the current session"
(6.4). Session images already carry that counter. `next_invoke_serial` is
removed.

### 6. Publishing ends a replay

`ta_actor_publish()` of a replaying actor:
- schedules its held delayed sends under their due times. One already past
  due fires at the next `ta_system_dispatch_due()`, late, as after a load
  (ADR 0074);
- retires every invocation that is still active, appending
  `error.communication.invoke.<id>.unavailable` to the internal queue, as
  restoring an image does (ADR 0038). If anything was appended, the actor
  is left with a macrostep to run. The world has moved on since the
  service started, and the chart handles that the same way it does after
  a load.

An actor prepared without `at` publishes as a replacement activates today:
its held effects are released, and its pending invocations start.

### 7. The host's operations can be replayed

The host logs its own operations on an actor, and the replay driver
performs them again at the same place among the inputs:
- `ta_actor_withdraw_send()` also removes a replaying actor's held sends
  with that ID. It keeps ADR 0084's scope and results.
- An operator transition is a delivery (ADR 0082).
- Abandoning a macrostep, and resuming with `no_microstep_limit` after a
  runaway, work on an unpublished actor as on a published one.

### 8. A resume says whether its event was ignored

`ta_actor_resume_params` gains `TA_OUT TA_OPTIONAL bool *ignored`. It is
set when the call ends a macrostep (`TA_RUN_STABLE` or `TA_RUN_DONE`):
true when that macrostep's external event enabled no transition, which
is what `ta_after_selection` counts as `events_ignored`. It is false for:
- a bootstrap;
- an operator transition;
- a macrostep with no external event;
- an event that enabled a transition.

Live actors get it too (napi's `SendResult.consumed`). A replay driver
uses it to report history that the current revision ignores.

### 9. A data model reads the macrostep's time

```c
/* The logical time of the running macrostep: a replaying actor's clock,
 * otherwise the system's time when the macrostep began. */
TA_API ta_time ta_dm_host_time(const ta_dm_host *host);
```

A host data model uses it to give expressions a deterministic clock
(napi: `context.time`). In the live run, it is the time the host logs with
the input.

## Scope: when a replay is exact

A replay under the same revision rebuilds the actor's state (its
configuration, data, history states, active invocations and pending
sends) when:
- every family the actor sends to has a loader. Otherwise a synchronous
  ERROR from the live run is not in the log;
- expressions read nothing but the data, the event and
  `ta_dm_host_time()`. That rules out random numbers, outside state, and a
  stored `_sessionid`;
- every input was committed before its effects were released. An effect
  released but not committed before a crash happens again after the
  replay. Delivery is at least once in that window unless the host keeps
  an outbox.

## What stays with the host

- The log: the spawn with its time and binding, each input call with its
  time, and the host's operations. Also when to commit it, where to store
  it, how to encode values, and upcasters for events whose shape changed.
- Replay order, and queueing live mail behind the history.
- The moment an actor stopped: on eviction, dispatching its due sends,
  then logging the eviction time; on every turn, committing the dispatch
  horizon (section 2) with the turn's inputs.
- Loaders for the families that persisted actors address (ADR 0088).
- Whether a replay is traced, and how it is marked.

## Considered and rejected

- **Discarding a replayed macrostep's effects, and withdrawing held sends
  due before an eviction time** (an earlier draft of this ADR). An effect's
  synchronous answer was treated as if it carried no information, and the
  eviction time was a guess.
- **Having the host answer replayed effects from its log**, through a
  `replayed` flag on the adapters, a list of answers on each delivery, or
  a replay adapter. Each of these leaks replay out of core to work around
  synchronous answers. The two that matter are removed by the invoker
  contract (section 4) and by the family loader (ADR 0088).
- **A new synchronous answer, INACCESSIBLE, carried in the log.** It is not
  needed: with a loader, the router never refuses mail to a family
  synchronously.
- **A log record for each delayed send that fires.** The log already
  holds what a timer's firing tells the actor: a timeout is a delivery,
  and a send to another actor tells the sender nothing. The moment the
  actor stopped settles the rest (section 2) with one record per turn,
  not one per timer.
- **A queue of arrivals inside core.** The host already keeps mail until an
  actor is idle, and one mechanism is enough.
- **An `at` field on each input call** instead of `ta_actor_set_time()`
  (decided in review, 2026-09-29):
  - it would add a field to `ta_actor_deliver_params` and
    `ta_invoke_report_params`;
  - `ta_actor_withdraw_send()` and `ta_actor_abandon()`, which take plain
    arguments, would need parameter structs, and every caller would
    change;
  - publish would still need a time for the last clock move;
  - every live caller would pass null, and every input function would
    carry the replay rules.

  Its one advantage is that a missing time is refused, where a forgotten
  `ta_actor_set_time()` goes unnoticed. That is a bug in the replay
  driver, and the driver's tests catch it. The runtime cost is
  negligible either way: one extra call per replayed input, against a
  null check per live input.
- **A time override on each resume** (the proposal). A macrostep spans
  several resumes, and its time must not change within it.
- **A scratch system, then moving the image** (granary's plan). Images are
  valid only in the system that made them (ADR 0074).

## Consequences

- A host can rebuild a named actor from its log inside the live system,
  and upgrade a live actor by replaying its log into a replacement under
  the new revision. It needs no second system and no `restore` state in
  the chart.
- **Behaviour changes for live actors:**
  - an invoker's start failure arrives as
    `error.communication.invoke.<id>.<reason>` in a later macrostep,
    instead of as `error.execution` in the same one. napi's SCXML invoker
    reports its compile and spawn failures, and its tests change;
  - `ta_actor_report_invoke()` can answer `TA_ERR_BUSY`, so napi's invoker
    path queues reports;
  - generated invoke IDs are numbered per session, and share numbers with
    generated send IDs. Tests that assert those numbers change.
- **The public API changes:**
  - two functions are renamed, with new parameter structs;
  - `ta_actor_set_time()` and `ta_dm_host_time()` are added;
  - `ta_actor_resume_params` gains `ignored`;
  - the invoker `start` contract narrows.

  The napi addon and the test host migrate in the same change (AGENTS.md).
  The IRP suite must still pass: none of its `<invoke>` tests expects a
  synchronous error from a start. The only error it checks around an
  invocation is test 347's catch-all `error`. napi's documented "a child
  chart that cannot be compiled at invoke time raises error.execution in
  the parent" (`docs/scxml.md`) changes to the report.
- **Costs,** to be measured when this is implemented:
  - a published actor: 0 bytes. The replay state lives with the held
    effects, which only an unpublished actor has;
  - a replaying actor: its held effects, as a replacement has today, plus
    its clock (8 bytes) and a flag;
  - an event block: one flag for `ignored`;
  - a system: `next_invoke_serial` (8 bytes) is removed;
  - the image format is unchanged, and published actors deliver, resume
    and dispatch at the same cost.

## Implementation notes

### Sections 4, 5, 8 and 9 (2026-09-29)

- **Where:** `src/invoke.c` (the generated ID's serial, the BUSY check on
  an ERROR report), `src/system.h` (`next_invoke_serial` removed),
  `src/actor.h` (`ta_event_slots.external_ignored`, `timed`, `time`),
  `src/interpreter.{c,h}` (`ta_note_ignored`, `ta_take_macrostep_time`,
  `ta_interpreter_macrostep_time`, `ta_block_settle`), `src/datamodel.c`
  (`ta_dm_host_time`), `include/tinyactors.h` (the start contract, the
  report's BUSY, `ta_actor_resume_params.ignored`, `ta_dm_host_time`).
  Commit `1c0991b`.
- **A start that still answers `TA_OP_ERROR`** (section 4 left the choice to
  the implementation) raises `error.execution` in the invoking macrostep,
  as before. It is the simplest safe behaviour: a host that has not moved
  its failures to reports keeps working, and only loses replay exactness
  for that failure, which the header says. The test host's SCXML invoker
  (`tinyactors-test/src/adapters.c`) still answers `TA_OP_ERROR`: it does
  not replay, and its scenarios keep their `error.execution`. napi reports.
- **BUSY covers every report kind,** ERROR newly: EVENT and DONE already went
  through `ta_actor_deliver()`, which answered BUSY. "Inside a macrostep" is
  `ta_actor_deliver()`'s test: the macrostep flag, or a delivered event not
  yet taken.
- **`ignored`** is a flag in the event block's slots, set where
  `events_ignored` is counted and read when the macrostep ends, stable or
  finished; it is cleared with the external event. A macrostep that takes
  an external event keeps its block until it ends, so the flag survives any
  number of resumes. A call that ends no macrostep (the actor was idle or
  already finished) leaves `*ignored` as it was.
- **The macrostep's time is taken by its first resume,** not when the event
  is delivered: a macrostep can start without a delivery (a bootstrap, an
  error report, a load), and every one is run by resumes. It is kept in the
  event block, which an actor now holds until its macrostep ends
  (`ta_block_settle` keeps a block while the macrostep flag is set, where it
  kept one only while a trace parent waited). Freeing an actor releases the
  block even mid-macrostep. Section 2 substitutes the replaying actor's
  clock at `ta_take_macrostep_time`. Delayed-send due times and trace
  records' `now` still read the system clock; section 2 moves them to the
  macrostep's time for replaying actors.
- **Costs:** `ta_event_slots` grew from 192 to 200 bytes (the time; the two
  flags fill padding), so an event block is 8 bytes larger. Blocks exist
  only while an actor holds an event or runs a macrostep, and come from the
  system's cache. `ta_actor` stays 232 bytes, and the system is 8 bytes
  smaller. A macrostep without a delivery or a traced spawn (an untraced
  bootstrap, for one) now takes a block at its first resume: a cache hit
  after warm-up. `scale_test` measured 332 bytes per actor, flat from 50,000
  to 4,000,000 actors; napi's `scale.js` is unchanged at 328 native bytes
  per actor.
- **Tests:** `tinyactors-test/ruby/scenarios/session_inputs.rb` (a resume
  says whether its event was ignored, and leaves `ignored` alone when it
  ends no macrostep; two sessions number their invocations alike, after
  their generated send IDs, which failed with the system counter);
  `invoke_errors.rb` (a report into a paused macrostep answers `err_busy`,
  changes nothing, and is taken once the session is idle);
  `tinyactors-napi/test/session-inputs.js` (`consumed`, `context.time` on
  the manual clock, and an `unavailable()` report taken after the mail
  queued before it). The time staying fixed across the resumes of one
  macrostep has no test of its own: napi's pump cannot move the manual
  clock mid-macrostep. `mise run test:full` passed, the test host's 527
  scenarios included (its fast pass runs 427); IRP 176/176 on Node and Bun.

### Sections 1, 2, 3, 6 and 7 (2026-09-29)

- **Where:** `include/tinyactors.h` (`ta_actor_prepare_params`,
  `ta_actor_publish_params`, `ta_actor_prepare()`, `ta_actor_publish()`,
  `ta_actor_set_time()`, the withdrawal's note), `src/actor.{c,h}`
  (`ta_actor_replaying`, `ta_actor_now`, prepare, publish and its route,
  the clock, withdrawal of held sends), `src/interpreter.{c,h}` (a
  replaying actor's immediate sends and logs dropped in `ta_send_commit`
  and `<log>`, its pending invocations started at the end of a macrostep,
  `ta_interpreter_queue_reserve`, every "now" read through
  `ta_actor_now`), `src/invoke.{c,h}` (`ta_invoke_meta.replayed`, no
  invoker call for a replayed start, no cancel or forward for a replayed
  invocation, `ta_invoke_prepare_retire` and `ta_invoke_retire_all`),
  `src/trace.c` (a lifecycle record's `now`). Commit `fb908fd`.
- **The replay state lives in `ta_activation_effects`,** the record an
  unpublished actor's held effects already used: `replaying` and `clock`.
  A replaying actor gets it at prepare, for its clock. `ta_actor_now()`
  answers the clock for a replaying actor and the system's time otherwise,
  and is what the macrostep's time (section 9), a delayed send's due time
  and a trace record's `now` read, so a live actor's times are unchanged.
- **Held sends are dropped by due time only.** A replaying actor's held
  list holds nothing else: its immediate sends and logs never enter it
  (section 3). `ta_actor_set_time()` compacts the list in place.
- **A send to `#_<invokeid>` of a replayed invocation** (not in the
  decision). A replayed invocation has no child, so the target resolves
  to nothing; the run being replayed reached a child there. While the
  invocation is active, a replaying actor's send to it counts as sent: an
  immediate one is dropped, and a delayed one is held with no destination
  (runtime ID 0), so if it is still held at publish it fails at dispatch as
  the invocation is gone by then. A send to an invocation that is not
  active raises `error.communication`, as in a live run.
- **`done.invoke` does not retire an invocation** (section 3 said it would).
  Core never did: an invocation ends when its state exits, when the host
  reports it lost, or when the session finishes. A replayed `done.invoke`
  runs `<finalize>` and the chart's transitions, as the live one did, so the
  replay matches the run; the text was wrong, not the code.
- **Publishing a replay makes room first.** It reserves the timer room for
  its held sends, names each `error.communication.invoke.<id>.unavailable`
  and reserves queue room for them (`ta_invoke_prepare_retire`), and only
  then changes the directory; so a refusal changes nothing, and the rest
  (scheduling, queueing, forgetting the invocations) cannot fail. The
  out-of-memory sweep in `tests/replay_test.c` checks it at every
  allocation publish makes.
- **An UNLOADED entry's parked session ends** when publish replaces it
  (ADR 0066), as `ta_actor_remove_unloaded()` ends it; the image or bytes
  the host kept describe nothing after that.
- **The bootstrap's trace parent:** `ta_actor_prepare_params.trace` puts
  the bootstrap under a parent as a spawn's does. A prepared actor writes no
  SPAWNED record, as a prepared replacement wrote none.
- **Operator actions** needed nothing: `ta_actor_deliver()` with an operator
  transition, `ta_actor_abandon()` and a resume with `no_microstep_limit`
  never looked at `published`.
- **Callers migrated in the same commit:** napi (`System.prepare()` and
  `System.publish()`, `PreparedActor.replaying` and `setTime()`, the
  `published` hook that moves an incumbent's mail and wakes an actor left a
  macrostep; `Actor.prepareReplacement` and `activate()` removed; the debug
  adapter's upgrade, `docs/design.md`, `test/persist.js`, the API status
  probe) and the test host (`prepare`, `publish` and `set_actor_time`
  helpers; `definition_upgrade`, `named_actors`, the workbench, the runner
  self-test, `scale_test.c`).
- **Costs:** a published actor stores nothing more. `ta_activation_effects`,
  which only an unpublished actor has, grew from 48 to 64 bytes (the flag
  and the clock). `ta_invoke_meta` stays 40 bytes (`replayed` fills
  padding). Delivery, resume and dispatch cost the same for published
  actors: the replay checks are one test of `published` first.
  `scale_test` measured 332 bytes per actor, flat to 4,000,000 actors.
- **Tests:** `tests/replay_test.c`: the four timelines of section 2 (a
  timeout to itself dropped when the clock passes it and taken from the
  log; a send to another actor dropped at the stop; one still pending at
  the stop, fired once, late, at publish; a crash between commits firing
  again); a replay under the same revision matching the live run's
  configuration, deep history and pending sends; a replayed invocation
  whose reply reaches `<finalize>` while no invoker, router or log sink is
  called; publish retiring an invocation as unavailable; result codes of
  prepare, publish (resident address, UNLOADED entry, finished, published)
  and set_time (not replaying, backwards, the future, busy); withdrawal of
  a held send; the out-of-memory sweep of publish.
  `tinyactors-test/ruby/scenarios/replay.rb` (an evicted actor rebuilt from
  its inputs a week later, and a send pending at the stop firing after
  publish). `tinyactors-napi/test/prepare.js` (the same through
  `System.prepare/publish`, and prepare's refusals). `mise run test:full`
  passed (529 test host scenarios); IRP 176/176 on Node and Bun.

