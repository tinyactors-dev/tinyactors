# ADR 0003: Singular calls are primary; enumeration is paged

**Status:** Accepted
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
`ta_system_visit_actors` only as a callback. Calling into JavaScript once per
actor, or building one array of every actor, costs allocation and GC
proportional to the population. tinyactors-test ran into the same limit: its
one hand-written batch method, `system.actors` (`src/server.c:185`), exists
because "the visitor callback cannot cross RPC". Its scale checks
(`tests/scale_test.c`) call core directly and cross no boundary.

## Decision

1. **The API is singular.** `spawn`, `post`, `send`, `destroy` and
   `inspect` each operate on one actor. They allocate nothing per call
   beyond what the operation itself needs.
2. **Iterating the population is paged.**
   - `system.actors()` returns an iterator that fills a native buffer from
     `ta_system_visit_actors` one fixed-size page at a time, and gives out
     inspections from that page. It never builds the whole population.
   - `system.actorIds()` returns the same pages as `Uint32Array`s of
     interleaved `[slot, generation]` pairs (ADR 0002), with no objects per
     element.
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
- A JavaScript loop that posts to four million actors costs four million
  crossings. That is linear, and ADR 0007's pump keeps the event loop
  responsive while those messages are processed.
- Broadcast-style workloads should fan out inside the actor system (one
  message to a coordinator actor that sends to the others) rather than from
  JavaScript. That keeps the traffic native.
- `ActorInspection` objects exist for single actors and for the entries of a
  page. The fields are as in `index.d.ts`.
