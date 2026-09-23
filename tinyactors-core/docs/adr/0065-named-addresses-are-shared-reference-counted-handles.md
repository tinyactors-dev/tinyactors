# ADR 0065: Named addresses are shared, reference-counted handles

**Status:** Proposed; refines ADR 0022, ADR 0040 and ADR 0050
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
