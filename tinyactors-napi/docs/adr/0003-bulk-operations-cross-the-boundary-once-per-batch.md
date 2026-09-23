# ADR 0003: Bulk operations cross the boundary once per batch

**Status:** Accepted
**Date:** 2026-09-23

## Context

Every Node-API call has a fixed cost: argument unpacking, type checks, and
often allocation. With one call per actor, four million spawns or posts
means four million boundary crossings. Inspection is the same: an API that
returns an array of four million inspection objects allocates them all at
once.

## Decision

1. **Every operation that is applied per actor has a bulk form that takes or
   returns typed arrays:**
   - `spawnMany(definition, count)` returns a `BigUint64Array` of IDs;
   - `postMany(targets: BigUint64Array, event, data?)` posts one event to
     each target;
   - `destroyMany(targets: BigUint64Array)`.

   The singular forms are thin JavaScript calls into the same native paths.
2. **A bulk call is all-or-nothing when it is checked, and per-item when it
   is executed.**
   - Argument and capacity checks (for example queue limits, ADR 0005)
     happen before any change; if one fails, nothing changes.
   - After admission, a failure for one target (for example, it no longer
     exists) is reported per item and does not undo the others. The call
     returns a result array only when the caller asks for one.
3. **Iterating the population is paged.** `system.actors()` returns an
   iterator that fetches inspections from core in fixed-size pages
   (`ta_system_visit_actors` into a native buffer). It never materializes
   the whole population. A compact form returns only IDs as a
   `BigUint64Array` page.
4. **Native code keeps shared building blocks.** Growable vectors, rings,
   hash maps and scratch arenas are written once in the addon (ADR 0009) and
   used by every queue and batch path. No hand-written head/count/capacity
   logic is repeated.

## Consequences

- The cost of a population-wide operation is O(n) native work plus O(1)
  boundary crossings.
- Typed arrays of `bigint` are the bulk ID format. Callers that hold IDs in
  regular arrays pay for the conversion themselves.
- `ActorInspection` objects still exist for single actors and for pages. The
  fields are as in `index.d.ts`.
