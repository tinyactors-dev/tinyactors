# ADR 0003: Singular calls are primary; enumeration is paged

**Status:** Accepted; paging uses tinyactors-core ADR 0062
**Date:** 2026-09-23

## Context

An earlier version of this ADR required a bulk form of every per-actor
operation (`spawnMany`, `postMany`, `destroyMany`). The argument was that
four million calls means four million boundary crossings. That argument
mixed together two costs:

- **Per-actor JavaScript objects and `bigint` IDs.** These were the real
  scaling problem in the first addon: V8 global handles and GC pressure that
  grow with the population. ADR 0002 removes them. An actor is two small
  integers, and nothing is allocated per actor.
- **The fixed cost of a Node-API call**, about 100 ns plus argument
  conversion. With ADR 0002 and ADR 0004 (interned event names), a singular
  `post` is that crossing plus a hash lookup and a mailbox push, about the
  same as the core work it triggers. The cost is linear and measurable, but
  it is not a scaling problem.

At four million actors, most traffic should go from actor to actor through
the native router (ADR 0006) and never touch JavaScript. JavaScript supplies
stimulus from outside, not the steady-state traffic.

One case really does grow with the population: **enumeration**. Core offers
`ta_system_visit_actors` only as a callback. Before tinyactors-core ADR
0062, the walk always started at slot 0 and its order was not part of the
contract, so it had no cursor. Page *k* could only be built by walking again
and skipping the first *k* pages, and every skipped actor cost a full
inspection. At four million actors and pages of 1,024, one full iteration
made about 8×10⁹ inspections, and it could not be split across turns of the
event loop. Calling into JavaScript once per
actor, or building one array of every actor, costs allocation and GC
proportional to the population. tinyactors-test ran into the same limit: its
one hand-written batch method, `system.actors` (`src/server.c:185`), exists
because "the visitor callback cannot cross RPC". Its scale checks
(`tests/scale_test.c`) call core directly and cross no boundary.

## Decision

1. **The API is singular.** `spawn`, `post`, `send`, `destroy` and
   `inspect` each operate on one actor. They allocate nothing per call
   beyond what the operation itself needs.
2. **Iterating the population is paged, with a cursor.**
   - Core's `ta_system_visit_actors` takes a `ta_actor_visit_params`
     (tinyactors-core ADR 0062): a start position `from`, a `limit`, and
     `next`, where the following page starts. The walk is in increasing
     position order, and the position is opaque to the addon. A visit
     that fails leaves `next` at that actor, so the page can be retried.
   - Each page costs O(page size + empty slots passed): the walk starts at
     `from` and stops at `limit`.
   - `system.actors()` returns an iterator that fills a page in the call
     arena (ADR 0014) and gives out inspections from that page. It never
     builds the whole population. Inspection pointers are borrowed only for
     the visit, so the page copies what it keeps: state lists, formatted
     event names (ADR 0004), and value boxes for delayed-send targets and
     data (ADR 0011).
   - `system.actorIds()` returns the same pages as `Uint32Array`s of
     interleaved `[slot, generation]` pairs (ADR 0002), with no objects per
     element. It copies only the IDs.
   - A page is a snapshot. Actors spawned or destroyed during iteration may
     or may not appear.
3. **Bulk mutation is deferred until it is measured.** `spawnMany`,
   `postMany` and similar calls are added only when a benchmark of a
   JavaScript-driven fan-out shows that boundary crossings dominate. Such an
   addition needs its own ADR, which states the measurement.
4. **Native code keeps shared building blocks.** Growable vectors, rings,
   hash maps and scratch arenas are written once in the addon (ADR 0009) and
   used by every queue and by enumeration. No hand-written head/count/capacity
   logic is repeated.

## Consequences

- The API stays small, and every call has one native path.
- Iterating the whole population is O(population), with work per call
  bounded by the page size.
- A JavaScript loop that posts to four million actors costs four million
  crossings. That is linear, and ADR 0007's pump keeps the event loop
  responsive while those messages are processed.
- Broadcast-style workloads should fan out inside the actor system (one
  message to a coordinator actor that sends to the others) rather than from
  JavaScript. That keeps the traffic native.
- `ActorInspection` objects exist for single actors and for the entries of a
  page. The fields are as in `index.d.ts`.
