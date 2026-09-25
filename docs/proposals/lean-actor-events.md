# An idle actor carries no event

**Status:** Adopted → [core ADR 0069](../../tinyactors-core/docs/adr/0069-an-idle-actor-carries-no-event.md) (2026-09-24). The ADR decides the open questions below, and its implementation notes have the measurements on `main`.
**Touches:** tinyactors-core (one ADR, refining ADR 0038, 0047, 0055, 0064 and 0067), tinyactors-napi (the result of deliver)
**Not a plan:** nothing here is decided or scheduled. ADRs and roadmaps win.
**Experiment:** branch `worktree-agent-ac40da141657c3d16` on `origin` (deleted 2026-09-25, after ADR 0069 was implemented),
based on `1ff9d22`. It is not merged and is not meant to be; an ADR and
a fresh implementation on `main` would replace it.

## Goal

Cut per-actor memory by moving event storage off idle actors, where it
does nothing. Today `ta_actor` holds three 64-byte `ta_event` copies
plus their flags, whether the actor is running or not:

| Field | What it is for | When it is set |
|---|---|---|
| `external_event` | The event `ta_actor_deliver` handed over, until the macrostep takes it | From deliver until the event is taken, usually in the same resume |
| `current_event` | What `_event` reads: the external event or an internal one | While a microstep runs, and afterwards until the next event |
| `macrostep_event` | A second retained copy of the external event, only so inspection can report it (ADR 0047) | From deliver until the macrostep ends |

`external_event` and `current_event` are never both in use: taking the
external event moves it, and the retry path after `TA_ERR_NOMEM` relies
on the moved-from bytes still being there. `current_event` exists for
internal events only because the queue head is removed when it is taken.
`macrostep_event` exists only because `current_event` is overwritten.

## The change, in three steps

1. **One external slot for the whole macrostep.** Deliver sets it;
   taking it marks it taken instead of moving it. Inspection reads the
   slot, so `macrostep_event` goes.
2. **`_event` names the external slot or the queue head.** An internal
   event stays at the head of the lent queue (ADR 0064) while it is
   `_event`, and leaves when the next event is taken or the macrostep
   ends. `current_event` goes. Retrying after `TA_ERR_NOMEM` clears where
   the event came from instead of moving events back.
   - Transition selection works on a copy of the event: an
     `error.execution` raised by a guard can grow the queue, which would
     leave a pointer to the head dangling.
3. **The external slot lives in the lent block.** The event, whether it
   was taken, and where `_event` points move into the block core lends a
   running actor, held from deliver until the macrostep ends. An idle
   actor carries no event.
   - Deliver takes a header-only block (80 bytes), which grows when the
     first internal event arrives.
   - The cache of empty blocks holds up to 1024, about 8 KB per system.
     Without it, a host that delivers a batch before resuming (napi
     delivers up to 1024 messages per turn) runs the cache dry and pays
     one allocation and one free per message.

Steps 1 and 2 stand alone: no new failure mode, a simpler retry.
Step 3 is only worth taking together with its header-only block and the
larger cache.

## Measurements

From the experiment, measured after each step on the same machine:

| | base | step 1 | step 2 | step 3 alone | step 3 + small block + cache |
|---|---|---|---|---|---|
| `sizeof(ta_actor)` | 432 | 360 | 288 | 224 | 224 |
| Scale test, bytes per actor (50k to 4M) | 531–532 | 459–460 | 387–388 | 323–324 | 323–324 |
| Scale test, allocator calls per event | 0 | 0 | 0 | 0 | 0 |
| napi, core bytes per idle actor | 464 | 392 | 320 | 256 | 256 |
| napi, extra bytes per actor holding a delivered event | 0 | 0 | 0 | 656 | 80 |
| napi, delivery throughput (M events/s) | ~4.47 | ~4.47 | ~4.47 | ~3.96 | ~4.42 |
| napi at 1M actors, native bytes per actor (plain / data / ws charts) | 480 / 528 / 1225 | | | | 272 / 320 / 1017 |
| napi at 1M actors, RSS bytes per actor (plain) | 518 | | | | 319 |
| napi steady-state core allocator calls per round (plain / ws) | 16384 / 0 | | | 32704 / 16320 | 16384 / 0 |

- The napi mid-macrostep and throughput figures come from a new bench,
  `tinyactors-napi/test/bench/event_slots.js` on the branch.
- The 16384 calls per round on the plain chart predate this change
  (`tinyactors-napi/docs/scale.md`).
- The core scale test delivers to and resumes one actor at a time, so it
  cannot see the batch-delivery cost that step 3 alone has in napi. A
  batched delivery case belongs in the scale test.

A benefit no figure above shows: an idle actor no longer keeps its last
event's data retained. With large payloads and many idle actors, that host
memory is likely worth more than the bytes in the table.

## `_event` between macrosteps

SCXML binds `_event` when an event is removed from a queue (5.10.1) and
never unbinds it, so after its first event a session's `_event` is its
most recent event, also between macrosteps. The experiment leaves `_event`
unbound between macrosteps.

Where that is invisible:

- Chart code inside a macrostep: guards, `onexit`, transition content,
  `onentry`.
- Invocations started at the end of a macrostep: they are evaluated
  before the macrostep ends.
- Bootstrap: there was never an `_event`, and `ta_dm_host_event()` may
  already return nothing.
- Delayed-send dispatch, unload and load: they evaluate nothing.

Where it is visible:

1. **A cancelled invoked child's `onexit`.** When a parent leaves the
   state that invoked a child, core runs the child's `exitInterpreter`
   inside the parent's interpreter (`ta_interpreter_exit_invoked`), while
   the child is between its own macrosteps. By SCXML, the child's
   `<onexit>` sees the child's last event there; in the experiment,
   `_event.data.x` in that `<onexit>` is `error.execution`. No test covers
   this, so the experiment passed every suite.
2. **Inspection between macrosteps.** A host reading the data model sees
   no `_event`. This changes no chart behaviour; the ADR 0067 trace gives
   a host the last event if it wants it.

**Proposed: keep the last event where cancellation can observe it.** A
session that was invoked, and so can be cancelled from outside its own
macrostep, keeps its last event after the macrostep ends: in its sparse
state, or by keeping its lent block. Top-level and named actors, the bulk
of any population, keep nothing. The cost is one retained event per
invoked child.

The alternative is to document that `_event` is unbound between
macrosteps and pin the cancel case to `error.execution`. It is simpler
and a known departure from SCXML, which the design has so far avoided.

**Test first:** a scenario with a child whose `<onexit>` reads
`_event.data` and sends it to the parent when the parent cancels it, on
jq and ECMAScript. It passes on `main`, fails on the experiment branch,
and must pass after the change.

## What an ADR would change

- **ADR 0064:** a block is also lent while an actor holds a delivered
  event, not only while events are queued or a microstep is paused. Zero
  allocator calls per event holds when the cache is at least the host's
  delivery batch; the ADR should state that rule and the cache size.
- **ADR 0047 and ADR 0055:** `ta_actor_deliver` can answer
  `TA_ERR_NOMEM` with nothing changed, and the host retries. The header
  comment for deliver and napi's mapping of the result need it.
  Inspection still reports the external event a paused or interrupted
  macrostep is processing.
- **ADR 0038:** the image carries the external slot, whether it was
  taken, where `_event` points, and a queue that includes the head in
  use. `macrostep_event` and `current_event` leave the image.
- **ADR 0067 section 8:** the current macrostep's trace parent would also
  live in the lent block, so tracing adds only the step counters (12
  bytes) to an idle actor instead of up to 64.
- **ADR 0061:** unchanged; pausing and resuming inside a microstep pass
  on the branch.

## Open questions

- Should steps 1 and 2 land first under their own ADR, with step 3 and
  the `_event` retention in a second one once the cancel scenario exists?
- Is 1024 the right cache ceiling, or should the host size it through the
  system config, since the host knows its delivery batch?
- The napi `ws` chart costs about 700 bytes per actor more than the
  `data` chart: a pending delayed send, an I/O processor send, and
  per-owner delayed-send arrays that grow by doubling. That is a separate
  proposal, slimming pending sends (`owner`, the second ID, `target`
  beside `destination`, and origin fields used only by submitted sends).
