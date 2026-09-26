# ADR 0066: A session ID outlives residency

**Status:** Accepted and implemented; refines ADR 0038, ADR 0044 and ADR 0052. See the implementation notes. Refined by ADR 0085: restored into another system, a session is a new one.
**Date:** 2026-09-23

## Context

SCXML's `_sessionid` identifies a session, and SCXML (5.10) keeps it constant
for the life of the session. `#_scxml_<sessionid>` is the target URI that
names that session (SCXML C.1, ADR 0044).

In core, `_sessionid` is the actor's runtime ID (`src/datamodel.c`,
`TA_DM_TEXT_SESSION_ID`). `#_scxml_<n>` parses to a runtime reference to
actor `n` (`src/scxml_uri.c`), and the host's router resolves it like any
runtime ID. A runtime ID lasts only while the actor is resident: unloading
frees it, and loading gives the session a new one (ADR 0028, ADR 0052). So a
virtual actor's `_sessionid` changes each time it pages in, a delayed send
to `#_scxml_<its old id>` goes stale, and the manual's statement that
`_sessionid` "comes from the image" is false. ADR 0038 treats a reload as
the same session continuing, which is what SCXML's rule needs.

## Decision

### 1. A session ID is the runtime ID the session was spawned with

- A session gets its ID when it is created: by `ta_actor_spawn()` and
  `ta_actor_prepare_replacement()`. The ID is the new actor's runtime ID.
- It is stored with the actor, carried in the session image, and kept by
  `ta_actor_load()`.
- `_sessionid` and the session URIs core writes (`_event.origin` and
  `_ioprocessors` for an unnamed actor) use the session ID, in decimal as
  before (ADR 0049).
- `ta_actor_inspection.session_id` reports it.

**Why the runtime ID and not a new counter.** Runtime IDs are never issued
twice: a slot's generation only moves on, and it skips 0 (ADR 0059). A
session ID is a runtime ID from the past, so no later spawn can collide with
it, and no counter is needed. A fresh session's ID equals its runtime ID, so
everything that addresses a never-paged session by its runtime ID, in core,
hosts and tests, keeps working unchanged. The two differ only after a
session has been reloaded.

### 2. Load keeps the ID; reconstruct keeps it unless a live session holds it

- `ta_actor_load()` continues the same session (ADR 0038): the directory
  says the named actor is unloaded, so no resident session holds its ID,
  and the loaded actor takes it.
- `ta_actor_reconstruct()` is route-neutral (ADR 0052) and may run while
  the image's session is still resident, for example to build a copy.
  Two live sessions must never share an ID. So reconstruct takes the
  image's ID only when no resident session holds it; otherwise the new
  actor starts a new session, whose ID is its runtime ID. If reconstruct
  runs while the named session is unloaded, the reconstructed copy takes
  the ID, and a later load of the same session starts a new one.
- A replacement (`prepare_replacement`, `activate_replacement`) is a new
  session with a new ID. It bootstraps a different definition revision
  from scratch rather than continuing the incumbent's interpreter state,
  which is what an SCXML session is. The incumbent's session ends when it
  is destroyed.

### 3. Session URIs resolve through a session table when the ID is not a runtime ID

`#_scxml_<n>` still parses to a runtime reference to `n`. When core submits
an SCXML send to the router, a runtime destination that is not a resident
actor's runtime ID is looked up as a session ID:
- a resident session that has been reloaded: the destination becomes that
  actor's current runtime ID;
- an unloaded named session: the destination becomes its named address, so
  the router and the host's directory page it in (ADR 0037), exactly like a
  send to `#_actor_<family>/<name>`;
- no session: the destination is left alone, and the router finds no actor,
  as today (`error.communication`).

This is done once per submission, including retries of submitted sends
(ADR 0054), so it happens when the send is dispatched, not when it is
written. A delayed send to a session that pages out and back in before it
is due still reaches it. A runtime ID the host holds is unchanged by this:
`ta_actor_deliver()` and the other host calls with a stale runtime ID still
answer `TA_ERR_NOT_FOUND` (ADR 0052). Only SCXML sends resolve session IDs.

**The session table** holds only the sessions whose ID is not a resident
actor's runtime ID: reloaded resident sessions and unloaded named sessions.
A never-paged session needs no entry, because the actor table already finds
it (ADR 0063). The table is a linear hash table over fixed pages, like the
address table (ADR 0065): each insert splits at most one bucket, growth
never copies the table, and a failed page allocation only postpones a split.
Its first page is allocated with the system, so inserting never fails.

- A reloaded session's entry is allocated when it is restored, in the
  fallible part of reconstruct, and freed with the actor.
- An unloaded session's entry holds a reference to the actor's address. It
  is allocated before unload changes anything, so unload still fails with
  `TA_ERR_NOMEM` and nothing changed. It is freed when a load of that
  address commits, when `ta_actor_remove_unloaded()` releases the name, or
  with the system.
- Per actor, core stores the session ID and a pointer to its entry.

### 4. Sends to self use the session ID

A `<send>` with no target names the sending session by its session ID, not
its current runtime ID. Resolution at submission (section 3) then finds the
session wherever it is: resident, reloaded, or unloaded. That replaces
`ta_timer_readdress_self()` (c669f6c), which rewrote a submitted self-send
to the owner's address at unload: the session table now does that for every
session URI, so the special case goes.

`ta_image_rebind_self()` no longer touches destinations. A destination that
names the session is its session ID, which a reload does not change, and
rewriting it to the new runtime ID would make it stale at the next unload.
It still rewrites origins: an unnamed actor's origin is its runtime
reference, which a reconstruct changes (section 5).

### 5. Invocations and origins

`#_parent` and `#_<invokeid>` resolve through the invocation table, not
session IDs, and a restored image loses its invocations anyway (ADR 0039).
An event's origin stays the sender's `record.self`: its address for a named
actor, its runtime reference for an unnamed one. An unnamed actor cannot be
unloaded, so its runtime reference lives as long as its session, and the
text core writes for it is its session ID.

## Consequences

- `_sessionid` is constant for the life of a session, as SCXML requires,
  across any number of unloads and loads.
- A delayed send to `#_scxml_<id>` reaches the session after it has paged
  out and back in, from the session itself or from another actor. While
  the session is unloaded, it routes by name, so the host pages it in.
- A never-paged session costs nothing extra, and every existing use of a
  runtime ID as a session URI keeps working.
- Per-actor memory grows by 16 bytes: the session ID and the entry pointer.
- `docs/manual/virtual-actors.md` is now correct: `_sessionid` does come
  from the image.

## Implementation notes

- **Where it lives.** `src/session.{h,c}` holds the table
  (`ta_session_table`, `ta_session_node`), `ta_session_resident()`,
  `ta_session_id_of()` and `ta_session_route()`. An actor has
  `session_id` and `session_node`. An address record points at its unloaded
  session (`ta_address.unloaded`), so `ta_actor_remove_unloaded()` and a
  committing load find the parked entry without a search.
- **Resolution happens at submission**, in `ta_submit_external()`, for every
  SCXML send, including retries of submitted sends. `#_scxml_<n>` still
  parses to a runtime reference to `n`, so `ta_scxml_target_parse()` and
  `ta_scxml_target_format()` are unchanged, except that formatting a
  resident actor's runtime reference writes its session ID.
- **A resident runtime ID still wins.** A runtime ID that names a resident
  actor is delivered to that actor as before, even when it is not the
  actor's session ID (a reloaded actor's current runtime ID). Runtime IDs
  are never reissued, so a value can't be both one actor's current runtime
  ID and another session's ID.
- **Load removes the parked entry only once the directory swap commits.**
  If the swap fails, the reconstructed actor is freed with its resident
  entry, and the parked entry still routes sends by address.
- **`ta_timer_readdress_self()` is removed**, as section 4 says, and
  `ta_image_rebind_self()` now rewrites origins only.
- **Changed tests.**
  - `delayed_send_test.c`: a refused send to self retried after its owner
    reloaded goes to the loaded actor's runtime ID, and one retried while
    the owner is unloaded goes by address (new test).
  - The paging scenarios: "sends to self" now assert the ADR 0066 contract.
    `_sessionid` is constant across reloads, on jq and ECMAScript. Pending
    sends to self name the session. A send to another session by
    `#_scxml_` reaches it after it reloads. A send to an unloaded session
    is submitted by address and reaches it after the host pages it in. A
    send to a session that ended is `error.communication`. The case "a send
    to another actor by runtime ID goes stale", from b85b7b2, is replaced:
    that target was a session URI, which now reaches the reloaded session.
- **Scale.** Per-actor memory went from about 508 to about 532 bytes: the
  session ID and the entry pointer, plus padding. Resolution flatness is
  tested structurally, not by timing, in `session_id_test.c`: 200,000 entries
  are inserted, found and removed, and no allocation is larger than one page
  of buckets, so the table never copies itself. A timing check in
  `scale_test.c` would need a population-sized number of unloaded or
  reloaded sessions at each size, which means capturing and unloading
  hundreds of thousands of actors. It is left for later.
