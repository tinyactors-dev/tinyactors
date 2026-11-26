# ADR 0019: An actor hibernates with its services running

**Status:** Accepted (2026-11-26), by the maintainer; refined the same
day after a refutation (a restored host and a newer blob, an actor that
faults, what ends a sleeping actor's services).
**Date:** 2026-11-26
**Builds on:** [ADR 0018](0018-external-invocations-move-with-their-actor.md)
(an external invocation has an id of its own).

## Whom it serves first, and who pays

Following [docs/roles.md](../roles.md):

- **First: the application developer with a `resident` bound** whose
  actors wait on services they invoked: a shipment that runs for a week,
  a human approval, a long model call. Hibernation refuses an actor with
  a running invocation, so the bindings' resident trim keeps every such
  actor in memory, however many there are, and a bound of 1,000 holds a
  million waiting orders resident.
- **Then: the capacity planner,** who can size memory by the resident
  bound again, not by the number of actors waiting on services.
- **Out of scope:** SCXML invocations (invoked charts): their sessions
  read their parent's instance, so an actor that runs one is still not
  hibernated. A `<send>` to an I/O processor answered later already
  hibernates; nothing changes for it.
- **Who pays:**

  | Cost | What |
  |---|---|
  | per event | nothing |
  | per event from a service | one lookup in an index (a hash and a few probes), instead of a scan of its actor's invoke slots; to a hibernated actor, what any event to it costs: a `WAKE`, a load from the store, a turn, and with a `resident` bound a hibernation and a save again (a service that streams progress moves its actor in and out of the store once per event) |
  | memory | an index of the running external invocations in each segment's region: 4 B per slot, at least twice the request pool's capacity (8 to 16 B per request) |
  | per hibernated actor running services | its request records (49 B each) stay in the segment, as its delayed sends do |
  | API | none; a hibernation blob of an actor with services keeps their slots; the region's layout changes |
  | bindings | none in code; an actor at rest with a running service is hibernated by the resident trim now |

## Context

ADR 0018 gave an external invocation an id that does not change with its
segment, and found it among its actor's invoke slots, which live in the
actor's instance (`src/system/schedule.c`, `external_request`). A
hibernated actor has no instance: its state is in the blob the
application stores. So an event from the service of a hibernated actor
found no invocation and was refused (`TA_ERROR_STALE`), and hibernation
refused an actor with a running invocation (`src/system/hibernate.c`,
`stable_member`).

What else an external invocation needs while its actor is hibernated is
already in the segment: the request record (type, src, params, id), held
by the actor's handle, which hibernation keeps; and the mailbox, which
takes events for a hibernated actor and asks for its wake. An invocation
ends only when its actor leaves the state or takes its `done.invoke`,
which a hibernated actor does not do, and a termination is applied when
the actor wakes (`ta_system_terminate`), so nothing cancels a service
while its actor sleeps.

## Decisions

1. **Each segment keeps an index of its running external invocations,**
   by id: open addressing over the request pool's indices, at least twice
   its capacity, carved in the region like the index of sessions and
   rebuilt when the region grows. An entry is added when an invocation
   starts or is adopted and removed when its request is freed, by moving
   the entries after it back (no tombstones: invocations come and go far
   more often than sessions). An event from a service finds its
   invocation there, whether its actor is resident or hibernated, and is
   refused when the invocation is not the target actor's.
2. **Hibernation keeps an actor's external invocations running.** The
   blob keeps their invoke slots (the request's handle, valid in the
   segment that wakes the blob, or a copy restored from its snapshot);
   the request records stay in the segment. A wake checks that each slot
   names a request of this actor, and refuses the blob otherwise. An
   actor running an SCXML invocation, or with a pending request of any
   other kind, is still refused.
3. **An event from the service of a hibernated actor** is queued and
   wakes it, as any event does; its `done.invoke` ends the invocation
   when the woken actor takes it, and a failure marks the invocation
   ended at once and parks `error.communication` until the wake, as for
   any failure that reaches a hibernated actor.
4. **A host restored from a snapshot reconciles a newer blob.** A wake
   accepts a blob of a hibernation made after the snapshot (the store went
   on), and the actor's services may have changed since: a service the
   copy runs for it that the blob no longer names is stopped (an
   `UNINVOKE`, its request freed), and a slot that names a service the
   copy does not have is cleared and the actor gets
   `error.communication`. Any other blob's slots must each name a service
   of the actor, or the blob is refused. The actor's count of requests is
   counted again from its slots at every wake (while it sleeps, the word
   holds its place in the wake queue).
5. **An actor that faults stops its services** (an `UNINVOKE` each, its
   requests freed), as leaving its states would. Before, its requests
   stayed in the pool for good.

## Consequences

- A population waiting on services hibernates down to the resident
  bound; each sleeper keeps its actor record, its delayed sends and a
  request record per running service in the segment.
- A delayed send of a hibernated actor to its service fires while it
  sleeps, routed by the request as before.
- A snapshot holds the index with the region; a restored host announces
  the services of hibernated actors again, as of resident ones.
- Hibernation blobs of an earlier build are refused, as their layout is.
- What ends a sleeping actor's services is its wake: a termination is
  applied then, and so is a migration. An application whose store loses a
  blob, or that drops a `WAKE` (a binding without a store), leaves that
  actor's services running until the host is freed; before, the same
  actor cost only its record. A segment with a hibernated actor is not
  retired, so its services are never orphaned by a retirement.
- `ta_actor_info` reports no invocations for a hibernated actor: its slots
  are in its blob.
