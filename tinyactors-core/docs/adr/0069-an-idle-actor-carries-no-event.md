# ADR 0069: An idle actor carries no event

**Status:** Superseded in part by ADR 0099 (2026-10-04, implementation pending): event blocks and their cache go; the taken event stays in its envelope, and an invoked session keeps its last event in a slot made when it is created. Accepted and implemented; refines ADR 0038, ADR 0047, ADR 0055, ADR 0064 and ADR 0067 (section 8). See the implementation notes for where the code differs from the text.; refined by ADR 0070 (a failed selection gives its event back; the queue grows per unit of content).
**Date:** 2026-09-24

## Context

Every `ta_actor` holds three `ta_event` copies and a flag for each, whether
the actor is running or not:

| Field | What it is for | When it is set |
|---|---|---|
| `external_event` | The event `ta_actor_deliver` handed over, until the macrostep takes it | From deliver until the event is taken, usually in the same resume |
| `current_event` | What `_event` reads: the external event or an internal one | While a microstep runs, and afterwards until the next event |
| `macrostep_event` | A second retained copy of the external event, only so inspection can report it (ADR 0047) | From deliver until the macrostep ends |

On a 64-bit host that is 192 bytes of the 432 in `ta_actor`, and the scale
test measures about 532 bytes per simple actor. An idle actor uses none of
it, apart from `current_event`, which keeps the last event's data retained
long after anything could read it.

The copies exist because events move. Taking the external event moves it
into `current_event`, and the retry after `TA_ERR_NOMEM` (ADR 0047) relies
on the moved-from bytes still being there. An internal event leaves the
queue when it is taken, so it needs `current_event` to live in.
`current_event` is overwritten, so inspection needs `macrostep_event`.

ADR 0064 already lends a block of memory to an actor while it runs: its
internal queue. The event storage can live there too, so that an idle actor
holds no event at all.

The experiment behind [An idle actor carries no event](../../../docs/proposals/lean-actor-events.md)
(branch `worktree-agent-ac40da141657c3d16`) did that in three steps and
measured each one. It left three questions open, and found one place where
it departed from SCXML.

### What `_event` is between macrosteps

SCXML binds `_event` when the Processor removes an event from a queue
(5.10.1) and never unbinds it. So after its first event, a session's
`_event` is its most recent event, between its macrosteps too.

Chart code of a session normally runs only inside its own macrosteps, so
`_event` between them is invisible. There is one exception. When a parent
leaves the state that invoked a child, core runs the child's
`exitInterpreter` inside the parent's interpreter
(`ta_interpreter_exit_invoked`, SCXML 6.4). The child is between its own
macrosteps then, or has been handed an event it has not taken yet. Its
`<onexit>` handlers may read `_event`, and must see the last event the child
took. The experiment released that event when the macrostep ended, so the
child's `<onexit>` failed with `error.execution` instead. No test covered it.

## Decision

1. **An actor's events live in its event block.** The block ADR 0064 lends
   a running actor (`ta_internal_queue`, now `ta_event_block`) holds:
   - the external event and its state: none, waiting to be taken, or taken;
   - what `_event` names: nothing, the external event, the internal event
     at the head of the queue, or the kept event (point 4);
   - the kept event;
   - the internal queue, as before.

   `ta_actor` loses all three event copies and their flags. Only the block
   pointer stays.

2. **Deliver takes a block.** `ta_actor_deliver` takes the actor's block,
   else a cached one, else a new one with no room for internal events. A
   block with no queue room is a header only. It grows when the first
   internal event is queued, through the reservation before a microstep
   (ADR 0064 point 3). If no block can be had, deliver returns
   `TA_ERR_NOMEM` and nothing changes; the host may retry. This refines
   ADR 0055: deliver is still the admission test, and `TA_ERR_NOMEM` is one
   more answer to it.

3. **Taking an event does not move it.**
   - Taking the external event marks it taken. It stays in its slot until
     the macrostep ends, and inspection reports it from there (ADR 0047).
     `macrostep_event` goes.
   - Taking an internal event leaves it at the head of the queue while it
     is `_event`. It leaves when the next event is taken or the macrostep
     ends. `current_event` goes.
   - The event `_event` named before a take becomes the kept event if it
     would otherwise leave: a taken queue head does. The kept event it
     replaces is released.
   - A microstep that cannot start after an event was taken
     (`TA_ERR_NOMEM`, ADR 0047) undoes the take: the external event is
     waiting again, and `_event` names what it named before. Nothing moves
     back.
   - Transition selection works on a copy of the event. An
     `error.execution` a guard raises can grow the queue, which moves the
     head.

4. **An invoked session keeps its last event.** When the macrostep of a
   session that was invoked ends, the event `_event` names becomes its kept
   event, and the session keeps its block. So a cancelled child's
   `<onexit>` reads its last event as SCXML requires, also when it was
   handed an event it has not taken yet. Every other session releases its
   events when the macrostep ends, and an idle one holds no block.

   For such sessions `_event` is unbound between macrosteps. That departs
   from SCXML 5.10.1, but no chart code can observe it: their chart code
   runs only inside their own macrosteps. A host that reads the data model
   between macrosteps sees no `_event`. The trace of ADR 0067 gives a host
   the last event if it wants it.

5. **The system caches empty blocks, and the host sizes the cache.**
   Because a delivered event now lives in a block until its macrostep ends,
   a host that delivers a batch before resuming holds one block per
   delivered actor. Zero allocator calls per event (ADR 0064 point 8) then
   needs a cache at least as large as the batch.
   - `ta_system_set_event_block_cache(system, blocks)` sets how many empty
     blocks the system keeps. It is 4 when a system is created, as in
     ADR 0064. A smaller setting frees the blocks above it.
   - The cache is a list threaded through the cached blocks, so a larger
     setting costs nothing until blocks are returned to it.
   - Only blocks with room for at most 8 internal events (the first
     capacity) are cached; a block that grew larger is freed when it is
     returned. So the cache holds at most `blocks` times one small block.
     ADR 0064 cached blocks of up to 64 events, which was harmless with 4
     of them and is not with 1024.

   The host knows its batch and core does not, so the host sets it. The
   napi addon sets the cache to its `messageBudget`, the most messages it
   delivers in a turn before resuming.

6. **Session images** (ADR 0038) carry the external event and its state,
   what `_event` names, the kept event, and the internal queue including a
   taken head. `macrostep_event` and `current_event` leave the image.
   Restoring an image that holds any of these gives the actor a block.

7. **Tracing** (ADR 0067 section 8): the trace parent of the current
   macrostep lives in the block, next to the external event it came with.
   An idle actor then carries only the two step counters, 12 bytes, instead
   of the 64 section 8 allows for. A larger public `ta_event` (section 8)
   grows the block, not every actor.

## Consequences

- An idle actor carries no event and retains no event data. With large
  payloads and many idle actors, the host memory that no longer stays
  retained is likely worth more than the bytes in core.
- `ta_actor` shrinks by the three copies. The proposal's measurements
  predict 432 to 224 bytes, and about 532 to 324 bytes per actor in the
  scale test.
- An actor holding a delivered event costs one header-only block: the two
  event slots and the header. A block that grew past 8 events is freed at
  the end of the macrostep, where ADR 0064 kept up to 64 in its cache.
- An invoked session keeps a block between macrosteps: at least the
  header, and more if its last macrostep grew the queue. Top-level and
  named sessions, the bulk of any population, keep none.
- `ta_actor_deliver` can return `TA_ERR_NOMEM`. The napi addon already
  treats it as retryable and backs off (napi ADR 0008).
- Hosts gain one call, `ta_system_set_event_block_cache()`. A host that
  delivers one event and resumes before the next needs no change.
- A retry after `TA_ERR_NOMEM` restores a mark instead of moving events
  back into the queue.
- The image struct changes. Images are in-memory and not a wire format
  (ADR 0038), so no stored image is affected.

## Implementation notes

2026-09-24. Built fresh on `main` in the proposal's three steps, one
commit each, after the scenarios below. The experiment branch was the
guide, not the source.

- **Names.** The block is `ta_event_block` (`src/actor.h`), which was
  `ta_internal_queue`; `ta_actor.event_block` is the one pointer. The
  events live in `ta_event_slots` inside it: the external event and its
  `ta_external_state`, `bound` (a `ta_event_binding`: none, external,
  internal, kept), `bound_before`, and the kept event.
  `ta_actor_bound_event()` is what `ta_dm_host_event()` returns.
- **A take remembers what `_event` was.** Point 3's retry rule is
  `bound_before`: taking an event records what `bound` was, and
  `ta_unbind_event()` puts it back. A taken queue head moves into the kept
  slot when the next event is taken, so the event a retry must bind again
  is still held. The continuation's two restore flags became one,
  `took_event`, since the unbind no longer needs to know where the event
  came from.
- **Invoked sessions.** "Invoked" is `sparse->has_parent`. Such a session
  keeps its whole block between macrosteps, which is 152 bytes when its
  last macrostep queued nothing and 728 when the queue grew to its first
  capacity. The point 4 scenarios check the kept event on both data
  models.
- **Deviation: a delivered block rarely stays a header.** ADR 0064's
  reservation makes room for one internal event before every microstep,
  so a header-only block grows to the first capacity as soon as its actor
  runs, even if the microstep raises nothing. A delivery with an empty
  cache therefore costs two allocator calls (the header, then the
  growth), and the blocks in a warm cache are mostly 728 bytes. The
  header-only block still bounds what an actor costs while it waits to be
  resumed with a cold cache: 152 bytes instead of 728.
- **Cache.** `system->block_cache` is a list through
  `ta_event_block.next_cached`, with `block_cache_count` and
  `block_cache_limit`. `ta_interpreter_block_cache_trim()` frees down to
  the limit; teardown sets the limit to 0 and trims.
- **napi.** `pump.c` calls `ta_system_set_event_block_cache()` with
  `messageBudget` after parsing the scheduling options, and `bindings.rb`
  lists the function as plumbing-owned. At the default budget of 1,024 the
  cache keeps at most about 745 KB. A host that raises `messageBudget`
  also raises what the cache may keep after a burst: the bench below,
  with a budget of 200,000, keeps 728 bytes per actor.
- **`ta_actor_deliver`** takes the block after the origin checks, so
  `TA_ERR_NOMEM` leaves the event and the actor untouched. The addon
  already backs off and retries on it (napi ADR 0008).
- **Tests.**
  - `tinyactors-test/ruby/scenarios/event_binding.rb`: a cancelled child's
    `<onexit>` sees its last external event, its last internal event, and
    not an event it was handed but did not take, on jq and ECMAScript.
    They passed before the change, and failed when the experiment's rule
    (release at the end of every macrostep) was put back.
  - The same file fails each allocation of the reservation before a
    microstep in turn, with paused actors holding every cached block, and
    checks that the resumed macrostep binds the same `_event` values.
  - `io_origin_test.c` now expects a top-level actor to give its last
    event's origin back when the macrostep ends, and warms the block cache
    before counting allocations.
  - `scale_test.c`, `check_batched_delivery_allocations`: 256 actors each
    delivered an event before any is resumed.
- **Measurements**, macOS on Apple silicon, before (`0a37619`) and after:

  | | Before | After |
  |---|---|---|
  | `sizeof(ta_actor)` | 432 | 224 |
  | Scale test, bytes per actor (50k to 4M) | 532 | 324 |
  | Scale test, allocator calls per event, one actor at a time | 0 | 0 |
  | Scale test, allocator calls per event, batches of 256, default cache | not measured | 1.97 |
  | Scale test, allocator calls per event, batches of 256, cache of 256 | not measured | 0 |
  | Event block header (`offsetof(ta_event_block, events)`) | 8 | 152 |
  | Cached block with room for 8 events | 584 | 728 |
  | napi bench, core bytes per idle actor (200k) | 464 | 256 |
  | napi bench, extra core bytes per actor holding a delivered event | 0 | 152 |
  | napi bench, delivery throughput, Node / Bun (M events/s) | 2.9 / 3.4 | 2.9 / 3.4 |
  | napi at 1M actors, native bytes per actor (plain / data / ws) | 480 / 528 / 1225 | 272 / 320 / 1017 |
  | napi at 1M actors, RSS bytes per actor (plain / data / ws) | 518 / 1411 / 2135 | 319 / 700 / 1456 |
  | napi steady-state core allocator calls per round (plain / ws) | 16384 / 0 | 16384 / 0 |

  The napi bench is `tinyactors-napi/test/bench/event_blocks.js`; the 1M
  figures come from `TAT_SCALE_MAX_POPULATION=1000000 node test/scale.js`.
  The 16,384 calls per round on the plain chart predate this ADR (napi
  `docs/scale.md`).
- **Open points.**
  - A retry after `TA_ERR_NOMEM` in transition selection itself, as
    opposed to the microstep it chose, does not give the taken event back;
    that was true before this ADR too, and is unchanged.
  - An invoked session keeps a block that may have grown. Swapping it for
    a cached header-only block when the macrostep ends would bound it at
    152 bytes, but the cache holds blocks of mixed sizes, so this is left
    until invoked populations are measured.
