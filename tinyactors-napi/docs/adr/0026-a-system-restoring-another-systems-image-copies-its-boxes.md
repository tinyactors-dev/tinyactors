# ADR 0026: A system restoring another system's image copies its boxes

**Status:** Accepted and implemented (2026-09-26). Applies tinyactors-core ADR 0085, on top of core ADR 0074 and napi ADR 0020 (unload() offloads to bytes that only their system loads); refines ADR 0011 point 7 and ADR 0012 point 3 for images restored in another system.
**Date:** 2026-09-26

## Context

Core ADR 0085 lets any system that registers an image's revision restore
it. A debugger forks a production actor into a system of its own, with its
own clock and stub I/O processors (`docs/proposals/debug-adapter.md`, "A
fork needs its own system"). `lib/image.js` refused that: `handleOf`
accepted only an image whose `origin` was the restoring system, and the
generated binding refused a handle of another system.

The refusal protected something real. Value boxes belong to a system
(ADR 0011, ADR 0014): a box is a record from its system's free list, holds a
`napi_ref`, and points back at its system's `tna_values`, whose counters it
updates and to whose free list it returns at its last release. An image
retains the boxes its events and delayed sends carry. Restoring it
retains them again, so an actor restored into system B would hold boxes of
system A. When A closes (ADR 0012 point 3) it frees its free list,
including those records, and B's next release of one of them writes freed
memory. A's close would also count them as leaked, since A's values are
not all released.

The data-model root is not the problem: `reconstruct()` and `load()` bind
it through the calling system, which makes a box of its own for it.

## Decision

**B copies every box it takes from an image of another system into its own
values when it restores it.**

- Core calls a value adopter for each value the restored actor takes from
  the image (core ADR 0085 section 6). napi injects one into `load()` and
  `reconstruct()` for the calling system (`ta_value_adopter` in
  `INJECT_TYPES`, `tna_inject_ta_value_adopter` in `src/runtime_codecs.c`).
- The adopter (`tna_values_adopter` in `src/values.c`) retains a box of the
  calling system and an inline primitive, as restore always did, and makes
  a new box in the calling system's values for a box of another system,
  holding the same JavaScript value. The JavaScript value is shared, not
  cloned (ADR 0011 point 1): a box holds a reference, and both systems live
  in one environment.
- A session image handle may be passed to a call against another system
  (`ANY_SYSTEM_HANDLES` in `bindings.rb`). The image still belongs to the
  system that captured it: that system's close destroys it, and a destroyed
  image is refused as before.
- `handleOf` no longer checks `origin`. `unload()` takes no image since
  core ADR 0074 (it offloads the actor to bytes), so an image of another
  system cannot reach it; core refuses `ta_actor_unload()` with one as
  `stale` (core ADR 0085 section 5).
- Offloaded bytes are not images: `load(address)` reads only the bytes
  this system wrote (core ADR 0074), and their values come back through
  this system's own data model, so they need no adopter.

So after a restore B holds only B's boxes. A and B close in either order,
and ADR 0011 point 7 still holds: no box outlives its system.

## Alternatives

- **Keep A alive while B holds its boxes.** A's values, its free list and
  its allocator lane would outlive A's close, reference-counted by the
  boxes other systems hold. ADR 0012 makes close authoritative and ADR 0014
  checks that a closed system's allocator holds nothing; both would become
  "unless another system still holds a box". A fork would also keep
  production's memory alive.
- **Refuse while A's boxes are held.** A fork of any actor with queued
  event data or a delayed send with data would be refused, which is most
  actors worth forking.
- **Move values into one pool per environment.** Boxes would no longer
  belong to a system at all. That changes ADR 0011 and ADR 0014 for every
  value to serve a debugger's fork, and loses per-system value statistics.
- **Round-trip each value through the data model's `value_encode` and
  `value_decode`** (core ADR 0074, which `unload()` uses with
  `unloadValues`). With `'serialize'` it is a `v8.serialize` copy, which is
  the clone below by another name and fails for a function; with
  `'handles'` it writes a handle to a box of the capturing system, which is
  the problem this ADR solves. It also covers only values the data model
  made, and an image captured mid-macrostep holds event data JavaScript
  posted. The adopter moves the box and keeps the value shared.
- **Clone the JavaScript value.** Copying the box is enough for lifetimes;
  cloning would be a policy (what a fork may share with production) that
  belongs to the debugger. The debugger clones the data root, which is what
  it can change; event data is read-only to a chart.

## Consequences

- A debugger can fork an actor into another system and close either
  system first.
- An image of another system costs one box per boxed value it holds: a
  record from the calling system's free list and a `napi_create_reference`,
  counted in that system's `stats().values`. In a scratch benchmark on
  Node 22 (two runs of 100,000 iterations), reconstructing and destroying
  a copy of an image with eight boxed delayed-send values took 2.55 and
  2.97 µs in another system against 2.04 and 2.21 µs in the capturing one:
  about 60 to 90 ns per copied box, within a noisy measurement. An image
  with no boxes cost the same in both (1.4 to 1.5 µs).
- A restore into the capturing system calls the adopter once per value,
  which retains it: one indirect call per value more than before, and the
  same allocations.
- `SessionImage.origin` still names the system that captured the image,
  and whose close destroys it.

## Testing

`test/persist.js` (`acrossSystems`): an actor captured in one system is
reconstructed in a second system that numbers its event names differently;
the copy holds boxes of the second system, fires its delayed send, with its
boxed data, on the second system's clock alone, and the original does not
move. Then
either the capturing system closes first, with no leaked bytes, and the
second keeps capturing and restoring; or the second closes first, and the
original fires on its own clock. With the adopter replaced by a plain
retain, the first order leaks 32 bytes at the capturing system's close.
