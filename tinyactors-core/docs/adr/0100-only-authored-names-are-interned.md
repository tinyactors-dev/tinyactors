# ADR 0100: Only authored names are interned

**Status:** Accepted (2026-10-05); partly implemented (matching by lookup; immediate eventexpr sends as text). Refines ADR 0007.
**Date:** 2026-10-05

## Context

ADR 0007 interns every event name as a path of atom segments in a trie,
and events carry a `ta_event_name_id`. Neither the atom table
(`src/atom.h`) nor the trie (`src/trie.h`) can remove an entry. Besides
the names a definition authors, core interns names it computes while the
system runs:

- the value of every `<send eventexpr>`, inside a macrostep;
- the value of every `typeexpr`, as an atom;
- `done.invoke.<id>` and `error.communication.invoke.<id>.<reason>`: the
  invocation's ID text and the host's reason become atoms, and the name
  becomes trie nodes, each time an invocation finishes or fails;
- whatever text a host passes to `ta_event_name_intern`,
  `ta_send_id_intern`, `ta_system_intern_invoke_id` and
  `ta_system_intern_atom`.

This causes two problems.

1. **The tables leak.** Every distinct run-time name stays for the life of
   the system. An `eventexpr` such as `'order.' + id` adds an atom and a
   node per order. A generated invocation ID (`<stateid>.<platformid>`) is
   unique per invocation, so every failed invocation adds entries that are
   never used again. A host that maps request fields to event names (an
   HTTP endpoint, a webhook's event type, a WebSocket message) lets any
   client grow the tables. Posting random event names exhausts the memory
   of any long-running system.
2. **Interning allocates inside a macrostep.** ADR 0070 lists interning an
   `eventexpr`, `typeexpr` or target among the allocations that can fail
   in the middle of a run. The ring protocol wants a macrostep that
   allocates nothing.

The matcher already compares atoms, not trie IDs:
`ta_event_pattern_matches` (`src/definition.h`) is ADR 0060's two-pointer
wildcard matcher over an event's segments as a list of `ta_atom`s. Only
the step that produces that list depends on interning.

One generated name is also inconsistent today. `done.invoke.<id>` splits a
dotted invocation ID into one segment per dotted part
(`ta_invoke_done_name`, `src/invoke.c`). `error.communication.invoke.<id>.<reason>`
interns the whole ID text as one atom, dots and all
(`ta_invoke_error_name`). ADR 0049 says a name segment never contains a
dot. Built as bytes and resolved at the dots, the error name splits the
ID, so a descriptor naming the ID's first part would match where it does
not today.

This was worked out in the ring protocol proposal:
[names.md](../../../docs/proposals/ring-proto/reference/names.md), item 5
of [migration.md](../../../docs/proposals/ring-proto/reference/migration.md),
and Phase 3 of the
[plan](../../../docs/proposals/ring-proto/IMPLEMENTATION.md#phase-3-match-names-by-lookup).

## Decision

1. **Authored names are interned at registration.** Every event descriptor
   of a definition's transitions, `<send event>`, `<raise event>`, a
   literal `<send id>`, `<invoke id>` and `<send type>` become atoms when
   the definition is registered.

2. **Core's generated prefixes are interned at system creation.**
   `done.state`, `done.invoke`, `error.execution`, `error.communication`,
   `error.platform`, and the `invoke` segment of
   `error.communication.invoke`. The atom tables therefore grow only at
   system creation and at registration.

3. **Run-time names are bytes.** The value of `eventexpr`, `typeexpr`,
   `targetexpr`, `idlocation` text and `sendidexpr`; the names and IDs of
   external input; an invocation error's reason (ADR 0081); and the
   generated `done.invoke.<id>` and `error.communication.invoke.<id>.<reason>`.
   They live in the name area of the envelope that carries them, and core
   looks them up but never adds them to a table. A generated send ID stays
   a counter (ADR 0048); a generated invocation ID becomes a state ID and a
   counter, rendered as text when text is needed.

4. **Matching resolves bytes into atoms by lookup.** When an envelope is
   taken, core splits its name at the dots and looks each segment up with
   `ta_atom_find`, which never inserts (`ta_names_resolve`,
   `src/names.h`). A segment that is not an atom resolves to UNKNOWN
   (`TA_NAMES_UNKNOWN`, which is `TA_ATOM_NONE`). The atoms go into the
   resolver's scratch of `eventNameDepth` atoms, and the matcher runs on
   them unchanged.
   - **UNKNOWN matches only `*`.** Every descriptor segment that is not a
     wildcard is an atom interned at registration, so none is UNKNOWN.
     Every segment a descriptor compares by equality resolves to the same
     atom it was interned as. Matching by lookup gives the same answers as
     matching interned text.
   - **Resolution happens when the envelope is taken, not when it is
     admitted.** The envelope does not store up to `eventNameDepth` atoms.
     One scratch serves the system, since one macrostep resolves at a
     time. A definition registered while the envelope waits adds atoms the
     resolution then sees. An envelope that never runs costs no lookup.
   - **A name deeper than `eventNameDepth` is invalid.** An external one is
     bounced as `invalid` at admission; one produced in a macrostep fails
     its `<send>` as `error.execution`. An empty name or an empty segment
     is invalid the same way.
   - A `typeexpr` value and a `<cancel sendidexpr>` literal are looked up
     whole (`ta_names_lookup`). A type no I/O processor registered is
     `error.execution`. A literal no definition authored names no pending
     send, so `<cancel>` does nothing.

5. **Atoms are never freed, and their IDs are never reused.** An atom
   lives as long as the system, even after its definition is freed
   (ADR 0080). Nothing that keeps an atom ID (a parked continuation, an
   internal-queue entry, a trace record) can find it naming something
   else. The tables grow with every distinct name ever authored, so growth
   is bounded by what operators register, not by traffic. A host that
   churns definitions without limit creates a new system from time to
   time.

6. **The host never interns.** An external event carries its name as
   bytes. `ta_send_id_intern` and `ta_system_intern_invoke_id` are removed;
   a replayed input carries text, and the session's definition knows the
   literals. Only the compiler and tools that build descriptors intern
   event names.

7. **The invocation error name splits a dotted ID.** In
   `error.communication.invoke.<id>.<reason>`, an invocation ID that
   contains dots becomes one segment per dotted part, as it does in
   `done.invoke.<id>`, and as ADR 0049 requires. The reason is one segment
   already: ADR 0081 refuses a dotted reason. This fix lands as its own
   change before the name area, so that matching by bytes changes nothing
   a chart sees.

## Consequences

- **The leak is closed.** A run-time name costs bytes in the name area
  while its envelope lives, and nothing once it is freed. An attacker's
  `x9f2.qq.zz` costs three hash lookups and leaves no memory behind.
- **ADR 0007 is refined, not reversed.** Authored names are still atom
  segments, and matching is still structural over atoms. Events no longer
  carry a trie ID; generated names are built as bytes, not by appending
  trie nodes. Images and logs store text, so ADR 0007's caveat about
  persisting process-local IDs does not arise.
- **Matching gives the answers it gives today,** apart from the error name
  of decision 7, which is fixed before the change so no chart sees a
  difference when names become bytes.
- **The data model gets `_event.name` as text** from the envelope, not as
  an ID it formats.
- **A system that churns definitions keeps every name it registered.**
  This is accepted: it is bounded by registrations.
- **Costs.**
  - *Before:* each distinct run-time name adds, for each segment not yet
    held, an atom (a 24-byte `ta_atom_entry`, its text in the table's
    arena, and an 8-byte hash slot plus the table's spare room) and a trie
    node (a 12-byte `ta_trie_node` and an 8-byte hash slot plus spare
    room), kept for the life of the system. Growth is unbounded: it
    follows distinct names in traffic, and some of them grow with every
    failed invocation.
  - *After:* the atom table and the trie grow only at system creation and
    registration, bounded by the definitions registered. A run-time name
    costs name-area chunks while its envelope lives: 64-byte chunks of 60
    text bytes each, most names fitting in one (the name area's figures
    are recorded with the ADR that introduces it).
  - *Resolver scratch:* `eventNameDepth` atoms, 256 bytes at the default of
    64, once per system.
  - *Per take:* one hash lookup per segment, paid once per taken event,
    not once per descriptor. Per actor and per event, no bytes change from
    this ADR alone.

## What this ADR does not decide

- The name area's layout, its parameter and its behaviour when full; the
  ring protocol's envelope ADR records those.
- Reference counting of atoms. It was rejected: freeing atoms when a
  definition is freed would cost four bytes per atom, removal from the
  trie, and an audit of every place that keeps an atom ID, to bound a
  growth that only registrations cause.
- A limit on how many atoms a system holds, beyond the table's own
  ceiling.
- How addresses of sessions that are not resident are stored; they are
  bytes in a waiting entry, never interned, and the envelope ADR covers
  them.

## Status of the work

Matching by lookup is done (Phase 3 of the plan). The matcher runs over a
name's atoms, built once per selection; `<send typeexpr>` and
`<invoke typeexpr>` look types up without interning; the `invoke` segment
is an atom from system creation. Events still carry a trie ID, so at take
the atoms come from the event's trie path, which gives the same list
`ta_names_resolve` gives over the name's bytes (`tests/name_matching_test.c`,
which also proves the worked examples of names.md and that random names
leave the atom table and the trie unchanged).

Step 4.2 of the plan completes it: it resolves the envelope's bytes with
`ta_names_resolve`, and stops interning the `eventexpr` value, the
generated invocation ID, `done.invoke.<id>`,
`error.communication.invoke.<id>.<reason>`, `#_<invokeid>` and
session-URI families in `targetexpr`, and the host calls. Decision 7 lands
before it.

## Implementation notes

- **The invocation error name splits a dotted ID (decision 7), 2026-10-05.**
  `ta_invoke_error_name` and `ta_invoke_done_name` share one helper that
  appends a segment per non-empty dotted part of the invocation ID, so
  `error.communication.invoke.<id>.<reason>` and `done.invoke.<id>` have
  the same segments for the same ID. A descriptor naming the ID's first
  part matches both. A name deeper than `eventNameDepth` is refused with
  `TA_ERR_INVALID`, for an error report as already for a done report, and
  the session is unchanged. Tested in `tests/name_matching_test.c`
  (claims 6 and 7); no IRP test or scenario changed.
- **An immediate `<send eventexpr>` to a session travels as text,
  2026-10-06.** The eventexpr's value is checked with the system's
  resolver and kept as text; its envelope keeps the name as a field of its
  text after the record. Inside core the taken event names it with
  `TA_RUN_NAME_INPUT` and the envelope's index (run.h), so `ta_event` keeps
  its 64 bytes (ADR 0068). Where the name is needed it is read into the
  run's name buffer, which grows at the refill before a take: the matcher
  resolves it into atoms, and `_event.name` and trace records read the
  text. 10,000 random
  names sent this way leave the atom table and the trie unchanged
  (`tests/name_matching_test.c`, claim 8).
  - Still interned, because the event is stored by name ID: a delayed send
    (`timer.c` and `ta_delayed_send_info.event` hold IDs; delayed sends
    become envelopes in step 4.3), a send held for an unpublished session,
    one for `#_internal`, one handed to an I/O processor, and mail kept as
    a submitted send when no envelope was free outside a macrostep.
  - Interned where a host reads the event, since hosts read names by ID
    until they pass and read names as text: a TAKEN outcome's event,
    inspection's `current_event`, a session image's events, an event
    forwarded to a host invoker, and an invoked session's kept event.
  - Not yet changed: the generated invocation ID, `done.invoke.<id>`,
    `error.communication.invoke.<id>.<reason>`, `#_<invokeid>` and
    session-URI families in `targetexpr`, and the host calls.
