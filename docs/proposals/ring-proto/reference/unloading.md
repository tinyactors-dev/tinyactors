# Unloading and loading sessions

**Status:** Sketch (2026-10-01), part of [the ring protocol proposal](../README.md)

A session that is not in memory is **unloaded**. Its image is the host's
to keep. Core only asks for it back. This document covers the
records that move a session out of memory and back in, and the one index
the host keeps for unloaded sessions.

## Unloading bounds memory; it is not persistence

Unloading exists for one reason: **to bound RAM while the process runs.**
One process can then hold, say, 100 million actors, as long as the actors
that are active at the same time fit in memory. The rest are images,
paged back in when mail or a timer needs them.

- **An image is valid only in the process that wrote it** (core ADR 0074).
  It ends with the process. Nothing may read it after a restart.
- **Where the host keeps images is up to the host:** memory, a temporary
  file, a table it empties at startup. The choice is about how much RAM to
  save, not about surviving a crash.
- **Starting again after a restart is a different mechanism:** a recorded
  session's [`c2h.input`](../../../../tinyactors-core/src/generated/MESSAGES.md#c2hinput) log, and its
  checkpoints, [`c2h.state`](../../../../tinyactors-core/src/generated/MESSAGES.md#c2hstate),
  seed new execution through [`h2c.spawn`](../../../../tinyactors-core/src/generated/MESSAGES.md#h2cspawn)
  and its `state` field (core ADR 0105,
  [recording-and-replay.md](recording-and-replay.md)). They do not restore
  the old execution.
  [`h2c.load`](../../../../tinyactors-core/src/generated/MESSAGES.md#h2cload) never brings
  anything back across a restart.

So after a crash there are no unloaded sessions. The host's index of them
is empty, and every session a restarted system needs is seeded again
from replay or a checkpoint.

## The records

| Record | Direction | Meaning |
|---|---|---|
| [`h2c.unload`](../../../../tinyactors-core/src/generated/MESSAGES.md#h2cunload) | host → core | page this stable session out |
| [`c2h.unloaded`](../../../../tinyactors-core/src/generated/MESSAGES.md#c2hunloaded) | core → host | here is its image, and when it next needs to run (`wake_at`) |
| [`c2h.load_needed`](../../../../tinyactors-core/src/generated/MESSAGES.md#c2hload_needed) | core → host | envelopes wait for this unloaded address |
| [`h2c.load`](../../../../tinyactors-core/src/generated/MESSAGES.md#h2cload) | host → core | here is the image |
| [`c2h.loaded`](../../../../tinyactors-core/src/generated/MESSAGES.md#c2hloaded) | core → host | it is resident again |
| [`h2c.load_failed`](../../../../tinyactors-core/src/generated/MESSAGES.md#h2cload_failed) | host → core | there is no image, or it cannot be read |
| [`c2h.load_result`](../../../../tinyactors-core/src/generated/MESSAGES.md#c2hload_result) | core → host | all consequences of a load failure have completed |

## Unloading

The host decides when to unload, for example when a session has been idle
for a while, or when the number of resident actors passes its own cap.
That is policy, and it stays the host's.

```
h2c.unload   {address: "campaign:42", image: <writable blob>}
c2h.unloaded {cause_seq, address, image, image_bytes, wake_at: t0+30s, flags: has_wake}
```

- Core writes the image through a byte sink the binding provides (core
  ADR 0090's `ta_byte_sink`) and names the result by a `blob` handle. The
  image holds the configuration, the data model root through
  `value_write`, and **the session's delayed sends**: they leave the pool
  with their owner, as they do today under core ADR 0054, whose storage the
  pool replaces ([envelope.md](envelope.md#delayed-sends-envelopes-with-a-due-time)).
- `wake_at` is the earliest of those delayed sends. Without the `has_wake`
  flag, the session has no timer and needs no wakeup.
- Core frees the actor slot and the envelopes the delayed sends used.
- [`c2h.bounced`](../../../../tinyactors-core/src/generated/MESSAGES.md#c2hbounced) with
  reason `has_mail` answers an `h2c.unload` for a session that has
  envelopes waiting. The host tries again later.
- An actor that owns an active local invocation, or is an active local
  invoked child, cannot unload. Core answers with `c2h.bounced` reason
  `active_local_invoke`, without changing the actor or its timers. The
  invocation's lifetime envelope and parent/child relationship remain
  resident until completion or cancellation.

Writing an image happens between macrosteps, so it may call the binding,
and it does not touch the macrostep invariant
([macrostep.md](macrostep.md)). Core's own memory for the image is the
byte sink's, which is the host's.

## The host's index of unloaded sessions

Core's deadline index covers only envelopes in the pool, which belong to
resident sessions ([envelope.md](envelope.md)). For unloaded sessions, the
host keeps an index of `wake_at`, and posts `h2c.load` when one comes
due.

| Host | Index |
|---|---|
| napi | a `Map` from address to `{image, wake_at}`, plus a min-heap by `wake_at`, plus **one** timer for the earliest. The timer for resident sessions is set by [`c2h.wake`](../../../../tinyactors-core/src/generated/MESSAGES.md#c2hwake). The host may merge the two into one `setTimeout`. |
| SQLite host | a temporary table, `CREATE TEMP TABLE unloaded(address TEXT PRIMARY KEY, image BLOB, wake_at INTEGER)`, with an index on `wake_at`. The next wakeup is `SELECT min(wake_at)`. It is temporary because the images end with the connection's system. |
| PostgreSQL host | the same, as an unlogged table that the host empties when it starts |
| test host | an ordered list. The tests move time explicitly. |

When a wakeup comes due, the host posts the image and then the time:

```
h2c.load {address, image, request_seq: 0}
h2c.time {at: now}          # the delayed send that was due fires as an envelope
```

Today `tah_record_wake` and the wakeup heap in
`tinyactors-host/src/loads.c` do this in a kernel module. Here it is host
policy over two records.

## No directory vtable: not resident means ask

Core keeps the directory of resident sessions and nothing else. It has no
`ta_actor_directory_vtable` and no other way to ask the host whether an
address exists. Mail, a timer or an external event for an address that is
not resident waits on that address's list, and core writes
`c2h.load_needed`. The host answers from its own index: `h2c.load`,
`h2c.spawn` for a family whose first message creates the actor, or
`h2c.load_failed` for an address it does not know
([below](#loading-when-an-envelope-arrives)).

- **One source of truth on each side.** Core knows the resident sessions.
  The host knows the unloaded ones. Nothing keeps a second copy.
- **No call out of a macrostep.** A miss only puts the envelope on a
  waiting list. Core never calls the host, which may do I/O, in the middle
  of a `<send>`.
- **A send to an address that never existed costs a round trip.** It holds
  its envelope until the host answers `h2c.load_failed`, and then becomes
  `error.communication` for its sender, as for any failed load. A host
  that knows an address cannot exist answers at once. Core has no table of
  loadable address families: the host already answers that question, and
  a second copy would need a declaration and could disagree with it.
- **Mail to addresses that do not exist holds pool capacity** until the
  host answers. The pool bounds it, as it bounds any slow load
  ([below](#waiting-envelopes-count-against-the-pool)).

## Loading when an envelope arrives

An envelope for an address that is not resident can be an `h2c.event`,
mail from a local session, or a resident session's timer that fell due. It
does not fail. It **waits in the pool**:

1. Core takes a **waiting entry** for the address, copies the address
   into the name area once, puts the envelope on the entry's waiting list,
   and writes `c2h.load_needed {address, waiting: 1}` from the entry's
   text ([envelope.md](envelope.md#the-waiting-entry-one-copy-of-the-address)).
   The record is written once per address until it is answered. Its
   header `seq` identifies this load request, including when the address
   has been requested before. The entry keeps it as `request_seq`.
2. More envelopes for the same address find the entry and join its list,
   in order. Core copies no address and writes nothing more.
3. The host answers in one of three ways:
   - [`h2c.load`](../../../../tinyactors-core/src/generated/MESSAGES.md#h2cload) with the
     image and `request_seq` equal to the request's `seq`. The waiting
     envelopes become runnable, in order, before anything sent to the
     session later, and each names the session in `target`. Core retires
     the waiting entry and writes `c2h.loaded`.
   - [`h2c.spawn`](../../../../tinyactors-core/src/generated/MESSAGES.md#h2cspawn) at the
     address, for a virtual family where the first message creates the
     actor (napi ADR 0020, the loader's `{ spawn }` result), with the same
     `request_seq`. The waiting envelopes go to the new session, and core
     retires the waiting entry.
   - [`h2c.load_failed`](../../../../tinyactors-core/src/generated/MESSAGES.md#h2cload_failed)
     with a reason and the same `request_seq`, for an address the host
     does not know or an image it cannot read. The waiting envelopes are
     recycled ([below](#recycling-on-h2cload_failed)).

How long the host takes is the host's business: it may read a file, query
a database or call a service. Core does not time out a load. A host that
wants a timeout posts `h2c.load_failed {request_seq, reason: "timeout"}`
itself, as napi's loader timeout does today (napi ADR 0020).

Core checks a nonzero `request_seq` before changing anything. A response
for a retired request, or for an address with no matching active request,
bounces as `stale_request`. It cannot answer a newer request for the
same address. A host that times out a load also retires its asynchronous
operation and discards late results. A proactive load from the
unloaded-session wakeup index uses `request_seq: 0`. When a request for that address is already
active, the host uses its sequence instead. A `load_failed` must answer an
active request and therefore cannot use zero.

`h2c.load_failed` can take several bounded batches of refusal reports
and recycled envelopes. Core retains the command until all of them are
complete, yielding for c2h drains as needed, and then writes
`c2h.load_result` with its `cause_seq` and `request_seq`, and frees the
waiting entry and its copy of the address. Only that record acknowledges
the load failure. Neither the h2c read position nor a single
`c2h.input_refused` acknowledges all waiting inputs. While this recycling
cursor is active, core consumes no later h2c command and runs no session,
so a newly submitted event cannot join the failed attempt's sealed
waiting list.

### Loading is atomic and may refuse

Loading runs between macrosteps. Core validates the image and stages the
actor, decoded values, restored timer envelopes and their name-area text
before publishing any of them. It reserves the complete success or refusal
record, including ring wrap padding, before consuming the command. No
waiting envelope becomes runnable until restoration can commit in full.

On success, core publishes the actor, attaches its timers, moves its
waiting envelopes to the run queue in order, retires the matching load
request and its waiting entry, and writes `c2h.loaded`. Restored timers retain their stored due
times; the following `h2c.time` processes any that are due.

If the pool, name area or allocator cannot satisfy restoration, core
releases all staged resources and writes `c2h.bounced` with reason `pool`,
`names` or `allocation`. An invalid image bounces as invalid. The actor
remains unloaded, its image remains the host's, and the waiting entry,
its list and active request, the directory and the clock remain
unchanged. Refusal does not
implicitly become `load_failed`. The host can unload another eligible
actor and retry, or explicitly fail the active request. A missing c2h
reserve only yields to the host; it does not partially restore the actor.

### Waiting envelopes count against the pool

The waiting list is threaded through envelopes in the pool, as the run
queue is ([envelope.md](envelope.md#the-lists-an-envelope-can-be-on)). Its
head and tail, and the one copy of the address, are in the address's
waiting entry, an item of a small pool whose ceiling is `poolEnvelopes`
([envelope.md](envelope.md#the-waiting-entry-one-copy-of-the-address)).
So a load that takes long holds pool capacity, one entry, and the
address's chunks once, however many envelopes wait. The pool's limits
apply to waiting mail as to any other:

- admission from h2c stops when the pool reaches `admissionWatermark`
  ([BEHAVIORS.md](../BEHAVIORS.md#parameters)), and the host refuses at its edge.
  Already-posted events that cannot be admitted are consumed and bounced;
  they do not prevent a later load answer or clock update from being read;
- a local `<send>` to the waiting address fails as
  `error.execution.capacity.pool` once the pool is full;
- a local `<send>` to an address that has no entry yet fails as
  `error.execution.capacity.pool` when no entry is free, or `.names` when
  the address does not fit in the name area. An `h2c.event` in the same
  case is bounced with reason `pool` or `names`.

The pool is the only bound on how many envelopes wait; no limit applies
per address. The host bounds how long they wait: it answers
`h2c.load_failed` at once for an address it does not know, and times out a
slow load itself. `c2h.load_needed` tells it how many envelopes wait. This
takes the place of napi's per-family `maxWaiting` and its `waiting-full`
dead letters.

Isolation therefore depends on the host answering promptly. Mail to one
slow or unknown address can use the pool's headroom for mail, and other
charts' local sends then fail as `error.execution.capacity.pool`. A cap
per address (`maxWaitingPerAddress`) would add a parameter that refuses
bursts to a popular actor that is slow to load, even when the pool has
room.

### Recycling on `h2c.load_failed`

Each waiting envelope is handled in place, with no new memory:

| Waiting envelope | Becomes |
|---|---|
| mail from a local session | `error.communication` for its sender, in the same envelope ([envelope.md](envelope.md), "Recycling"). `_event.sendid` names the failed send. |
| an `h2c.event` | dropped, and its vref released. `c2h.input_refused` identifies its `source_seq` and the delivery failure; a `tel.trace` of kind `DEAD_LETTER` also describes it. |
| a timer that fell due | a resident sender's timer can target a different, unloaded actor. It becomes `error.communication` for its sender in the same envelope, just like local mail. An unloaded sender's own pending timers remain in its image. |

Recycling needs no address: each error goes to its sender by session.
The waiting entry's cursor keeps recycling's place across c2h drains, and
the entry and its address are freed with `c2h.load_result`.
[`c2h.destroyed`](../../../../tinyactors-core/src/generated/MESSAGES.md#c2hdestroyed) recycles
a destroyed session's waiting envelopes the same way, and frees their
entry.

### Mail for a session that has gone

Mail a chart sends to a session ID (not an address) that is not resident
waits like any other envelope. Its target is the session's text, held
once in its waiting entry
([envelope.md](envelope.md#the-waiting-entry-one-copy-of-the-address)),
and `c2h.load_needed.address` carries it. The host's index maps that text to an image. This is the load keyed by
the session's text that napi ADR 0032 section 2 makes today
(`TAH_LOAD_WHY_SESSION`).

## What this replaces

| Today | In this design |
|---|---|
| `tinyactors-host/include/tinyactors_host/loads.h`, `src/loads.c` (about 530 lines): load records, waiting mailboxes, `max_waiting`, the wakeup min-heap, load outcomes | waiting lists in core's pool; `c2h.load_needed` / `h2c.load` / `h2c.spawn` / `h2c.load_failed`; the wakeup index is the host's (a `Map` and a heap, or a SQL index) |
| `tinyactors-host/include/tinyactors_host/offloads.h`, `src/offloads.c`: the bytes of offloaded actors, kept per address | the host keeps the `blob` from `c2h.unloaded` wherever it likes |
| `tinyactors-host/src/directory.c`: the in-memory directory and its unloaded entries | core's directory of resident sessions. Unloaded addresses are the host's index. There is no `ta_actor_directory_vtable`: "not resident" means "ask with `c2h.load_needed`" ([above](#no-directory-vtable-not-resident-means-ask)) |
| `tinyactors-napi/src/napi_loads.c` (about 550 lines): JavaScript loaders, timeouts, results | the loader stays in JavaScript, and calls `h2c.load`, `h2c.spawn` or `h2c.load_failed` |
| napi's dead letters `waiting-full`, `not-found`, `rejected`, `load-failed` | recycled `error.communication` to senders, or dropped external events reported on `tel` |

See [migration.md](migration.md) for the full list, and
[S07](../scenarios/S07-unloaded-tracker.md) for a walkthrough.
