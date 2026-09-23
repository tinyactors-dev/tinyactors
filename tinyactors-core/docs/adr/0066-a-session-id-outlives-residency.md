# ADR 0066: A session ID outlives residency

**Status:** Accepted; refines ADR 0038, ADR 0044 and ADR 0052
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
