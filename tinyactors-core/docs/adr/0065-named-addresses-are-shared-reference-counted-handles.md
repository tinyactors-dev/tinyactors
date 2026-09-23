# ADR 0065: Named addresses are shared, reference-counted handles

**Status:** Accepted and implemented; refines ADR 0022, ADR 0040 and ADR 0050. See the implementation notes for where the code differs from the text.
**Date:** 2026-09-23

## Context

A named actor's address is a `ta_actor_address`: a family name and an actor
name, each a borrowed `ta_str`. A `ta_actor_ref` of kind
`TA_ACTOR_REF_NAMED` holds one by value. Core copies the strings when it
must own them:
- the actor's own address, `record.self` (`ta_actor_claim_address()` in
  `src/actor.c`);
- a named destination that a router keeps
  (`ta_external_delivery_clone()` in `src/routing.c`, which allocates one
  block per string).

Everywhere else, core and hosts pass the strings by pointer. That causes two
problems.

**An origin can outlive its strings.** When a named actor sends, the
event's origin is the actor's `record.self`, by value, and its strings point
into memory the sender owns (`src/interpreter.c`, the external send path;
`src/invoke.c`). Several things keep such an event after the sender may be
gone:
- a stored delayed send keeps `origin` in its meta
  (`src/timer.c`). ADR 0054 moves a submitted send to the system-owned list
  when its owner is destroyed, and `ta_actor_free()` then frees
  `record.self`;
- `ta_external_delivery_clone()` copies the destination but not the origin,
  so a router that keeps a delivery holds borrowed origin strings;
- `ta_actor_deliver()` keeps the event as `external_event` and
  `macrostep_event` for the whole macrostep, with the origin still borrowed
  from the caller.

If the sender is destroyed first, and napi ADR 0012 makes destroying
finished actors the default, the receiver's `_event.origin` and any host
that reads the origin read freed memory. This was found by reading the
code. It has not been reproduced yet.

**Copying costs a lot at scale.** At four million named actors, such as one
per websocket connection named by session, every actor-to-actor message
that a host queues copies strings. Every directory operation hashes and
compares them. A host that wants to avoid that must build its own intern
table (napi ADR 0006), which duplicates what core could provide once.

## Decision

1. **Core interns named addresses.** Each system keeps an address table.
   Every distinct `(family, name)` pair that core holds is one record: a
   reference count, a hash, the two strings stored inline, and a pointer
   back to the table. The public handle type is `ta_address` (`TA_HANDLE`).
   Two equal addresses in one system are the same handle, so pointer
   equality is address equality.
2. **A named `ta_actor_ref` holds a handle.** `ta_actor_ref.as.named`
   becomes `const ta_address *`. Every named reference that core stores or
   hands out holds a reference:
   - the actor's `record.self`;
   - event origins, in the internal queue, in the actor's held events and
     in stored delayed sends;
   - resolved destinations of delayed sends;
   - directory entries passed to the host.
3. **Public functions:**

   ```c
   /* Finds or adds the address; *out holds a new reference. */
   TA_API ta_result ta_address_intern(ta_system *system,
                                      TA_IN const ta_actor_address *address,
                                      TA_OUT TA_RELEASE(ta_address_release) ta_address **out);
   TA_API void ta_address_retain(TA_INOUT ta_address *address);
   TA_API void ta_address_release(TA_INOUT TA_OPTIONAL ta_address *address);
   /* The strings, valid while the caller holds a reference. */
   TA_API ta_actor_address ta_address_view(TA_IN const ta_address *address);
   ```

   `ta_actor_address` stays the type for addresses a host supplies:
   spawn, find, unload and the codecs. Core interns them on entry.
   `ta_address_intern` allocates only the first time a pair is seen, and
   can then fail with `TA_ERR_NOMEM`. Retain and release cannot fail.
4. **Deliveries and events are retained, not cloned.**
   - `ta_external_delivery_clone()` is replaced by
     `ta_external_delivery_retain(const ta_external_delivery *, ta_external_delivery *out)`.
     It retains the destination, the origin and the data, never allocates,
     and cannot fail.
   - `ta_external_delivery_release()` releases all three.
   - `ta_event_release()` also releases a named origin, and a public
     `ta_event_retain()` is added beside it.
   - `ta_actor_deliver()` retains the event's origin together with its data.
     The caller may release its own references as soon as the call returns.
5. **The directory speaks handles.** The `ta_actor_directory` callbacks
   (claim, resolve, replace, remove) receive `const ta_address *`. A host
   directory can key its map by the pointer, with no string hashing.
6. **Growth of the table.** The table is keyed by the stored hash. It grows
   without copying the whole table in one step (incremental rehashing, or
   pages as in ADR 0063), so no single operation's cost grows with the
   number of named actors. A record is freed when its last reference is
   released.
7. **Durable formats are unchanged.** Session images and exports carry the
   strings, as ADR 0028 requires. Load and reconstruct intern them.
8. **Tests come first.** A test destroys a named sender while its event is
   still held (in a router's queue, and as a submitted delayed send), then
   delivers the event and reads `_event.origin`. Under AddressSanitizer
   the test must fail on today's code and pass with this change. A second
   test checks that a router that keeps and later releases a delivery makes
   no allocator call after warm-up.

## Consequences

- An origin or destination is valid for as long as someone holds a
  reference, whatever happens to the actor that named it.
- Actor-to-actor traffic between named actors copies no strings. A router
  keeps a delivery by incrementing reference counts.
- Hosts no longer need their own address tables. napi ADR 0006 uses core's
  handles, and its directory map is keyed by pointer.
- Each distinct live address costs one record, the same as the
  `record.self` copy costs today. The strings are no longer copied again
  for each queued delivery.
- This is a breaking change to `ta_actor_ref`, the directory interface and
  the delivery helpers. Every caller is in this repository (root
  `AGENTS.md`): core, the test host, its Ruby bindings and the napi addon
  move with it.

## Implementation notes

- **The use-after-free was real.** `tests/address_test.c` was written first,
  against the old API, and built with AddressSanitizer. Both cases failed
  with a heap-use-after-free: a router that kept a delivery (with
  `ta_external_delivery_clone`) read its origin's name after the sender was
  destroyed, and so did a router offered a submitted send after its sender
  was destroyed. In both, `ta_actor_destroy` had freed the name through
  `ta_actor_free`. The test now also covers an origin a host interned,
  delivered and released at once. It runs under `mise run asan`, which
  `mise run test` runs: ten core lifetime tests under AddressSanitizer and
  UBSan.
- **Handles are `ta_address *`, not `const ta_address *`.** The API
  extractor pairs a handle's output (`ta_address **out`) with a release
  marked `TA_DESTROYS`, and takes no `TA_IN` on handles. So the handle is
  non-const everywhere except `ta_address_view`, and releasing a reference
  is the handle's "destroy". `ta_address_release` accepts null.
- **The table (decision 6) is a linear hash table** (Litwin). It starts at
  16 buckets and splits one bucket per insert while it holds more than one
  record per bucket, so no operation rehashes the table. Buckets live in
  fixed pages of 1,024, found through a page directory, as the actor
  table's slots do (ADR 0063), so growing never copies them. A failed page
  allocation only postpones a split, so the only allocation that can fail
  an intern is the record itself, which stores both strings inline.
- **`ta_external_delivery_release` lost its allocator parameter.** Nothing
  is freed through it any more except by a last release.
- **Parsing a target interns.** `ta_scxml_target_parse` returns a target
  that holds a reference, and `ta_scxml_target_release` is added to give it
  back.
- **Images carry detached addresses.** A session image stores each address
  as a copy in its own arena, whose record has no table: retaining or
  releasing it does nothing. So an image does not depend on the system
  that captured it (ADR 0028). Reconstruct and load intern every address
  the image carries: the actor's self, the held and queued events' origins,
  and the delayed sends' destinations and origins. Capture used to copy
  only the self and the delayed sends' addresses; the held events' origins
  were borrowed from their senders, the same fault as above.
- **Where interning can happen during a run.** Only a `<send>` target is
  interned mid-microstep, when a literal or evaluated target is parsed.
  When the address is already held, which it is for every resident or
  directory-held actor and every address sent to before, this is a lookup
  and allocates nothing. Only an address nobody holds allocates, and a
  failure there returns `TA_ERR_NOMEM` from the send. That is the same
  class as the existing timer reservation in `ta_actor_schedule_send`
  during a send, and this change does not close it. Interning literal
  targets when the definition is compiled would remove the case for
  literal targets.
- **The directory contract.** The handle a callback receives is borrowed;
  a directory that keeps it retains it. `ta_system_destroy` frees every
  record still held, so every reference must be released before it.
- **Callers.** The test host's `ta_actor_ref` codec is hand-written: a
  named ref crosses as `{kind, family, name}` as before. It is interned
  into the request's system, or into the binding's own system for codec
  round trips, and released when the request ends. No scenario changed.
  The scale test's directory keys by the handle pointer, and a new check
  shows that a router keeping and releasing a delivery makes no allocator
  call after warm-up. Before, keeping one copied its destination's
  strings: two allocations.
- **Memory.** A named actor's address is one record where it was two
  string copies, so the scale test measures about 507 bytes per actor,
  down from about 563.
