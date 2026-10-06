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
- **Hosts pass names as text, 2026-10-06.** `ta_actor_deliver_params.name`
  and `ta_invoke_report_params.name` carry the event name as text, at most
  65,535 bytes (one name-area field); `event->name` is not read. Core
  checks the name with the resolver, keeps it in the envelope's text, and
  resolves it when the input is taken, as for mail. Admission is a safe
  point: the name area makes room for a long name there and the run's
  name buffer grows to hold it (it starts at 128 bytes, at system
  creation). `done.invoke.<id>` for a host's DONE report is built as text.
  `ta_event_name_format` reads an input's name kept as text while the input
  lives. The kernel, a host, keeps the names it is given in its own table
  until it is removed (`tinyactors-host/src/events.c`).
  - Still interned in core: names a host gives for delayed sends it
    restores (`ta_delayed_send_info.event`, through the kernel's
    `tah_core_event_name`), until delayed sends are envelopes; and names
    core hands a host (a TAKEN outcome's event, inspection, images, a kept
    event), until those carry text.
- **Hosts read names as text, 2026-10-06.** Core no longer interns a name
  where it hands an event to a host.
  - A TAKEN outcome gives its event's name as text
    (`ta_input_outcome.name`) in a buffer the caller passes in
    `ta_run_params.names`, of at least `TA_RUN_NAMES_MIN_BYTES` (64 KiB,
    the longest name and its NUL); the name is valid until the next
    `ta_run()` with that buffer, and the event's `name` is 0 when core
    keeps the name only as text. A call that has no room left for the next
    name answers `TA_RUN_MORE` and loses nothing.
  - Inspection's `current_event` and an event forwarded to a host invoker
    keep the name as a handle to the text
    (`TA_RUN_NAME_INPUT`), which a host reads with `ta_event_name_format()`
    while the event is valid: during the call, or until the next run or
    inspection. The kernel's JS invoker formats a forwarded name into its
    own table during the call, since it reads the event later.
  - An invoked session's kept event keeps its name's text in storage the
    session owns, its internal queue (`kept_text`), sized when the input
    is taken (`ta_interpreter_keep_room`, between inputs) and filled when
    the macrostep ends, with no allocation there; its handle is
    `TA_RUN_NAME_INPUT | TA_RUN_NAME_KEPT` and the actor's slot, so the
    actor table holds at most 2^30 slots. Changed 2026-10-06 after review:
    the first version kept the input's envelope (`TA_ENVELOPE_KEPT`), which
    counted against `poolEnvelopes` and the admission watermark, so a
    system with many invoked children stopped admitting input, and an
    abandoned macrostep left the kept event naming an envelope already
    given back. A session image copies the name's text into its arena, and
    a session restored from it writes the text back into its queue
    (`ta_interpreter_keep_name`). An event kept from the internal queue
    keeps the envelope that holds its name, as a queued event does.
  - `ta_send_id_intern` is gone, and `ta_system_intern_invoke_id` became
    `ta_system_invoke_id_find`, a lookup: a replayed event's send ID is
    generated (it parses anywhere) or an authored literal of a registered
    definition, and its invocation ID was issued again by the replayed
    parent before the child's events are replayed. Text the system never
    issued answers `TA_ERR_NOT_FOUND`.
  - Still interned: names a host gives for delayed sends it restores
    (`ta_delayed_send_info.event`, through the kernel's
    `tah_core_event_name`), with the public `ta_event_name_intern` and
    `ta_system_intern_atom` kept for that caller until delayed sends are
    envelopes; and names core stores by ID (`ta_event_name_store`): a
    delayed send, a send held for an unpublished session, one for
    `#_internal` or for an I/O processor.
  - Tested in `tests/name_matching_test.c` (claims 9 and 10) and
    `tests/run_test.c` (a names buffer is required for TAKEN outcomes).
- **Internal events and invocation names travel as text, 2026-10-06.**
  - A `<send>` to `#_internal` with an `eventexpr` queues its event with
    the name held as text in an envelope of its own (`TA_ENVELOPE_KEPT`,
    `ta_run_hold_name`), which the queued event owns until it is released;
    an image copies such names into its arena and a restore holds them
    again. With no envelope or no name-area room, or a name longer than
    one name-area field (65,535 bytes), the send fails as
    `error.execution.capacity.names`, as mail does; such a name was
    interned before, and napi's `test/import.js` now expects that error. Holding a name grows
    the run's name buffer to it, so the macrostep that queued the event can
    read it: that is the one allocation a hold may make inside a
    macrostep.
  - A generated invocation ID is no longer interned:
    `TA_GENERATED_INVOKE_ID_BIT` with the state ID's atom and the session's
    serial, formatted `<stateid>.<serial>` (`ta_invoke_id_write`) and
    parsed back by lookup (`ta_invoke_id_parse`). The public
    `ta_system_invoke_id_text`, which lent interned text, became
    `ta_system_invoke_id_format` with the buffer convention of
    `ta_send_id_format`. A `#_<invokeid>` target is looked up, and text
    that names no issued invocation names none (`TA_INVOKE_ID_NONE`), so a
    send to it fails as one to an ended invocation does. An authored ID
    whose text equals a generated one's is the authored one.
  - `done.invoke.<id>` from a local child, and
    `error.communication.invoke.<id>.<reason>` for a host's ERROR report,
    for a lost service and for retirement, are built as text: mail and
    admitted reports carry them in their envelopes; an error queued
    outside a run holds its name; retirement holds each name when it is
    prepared (`ta_invoke_prepare_retire`), so queueing them cannot fail,
    and `ta_invoke_cancel_retire` gives them back when publishing fails
    later. A report taken into the internal queue copies its name into a
    holder of its own, since the report's envelope goes when it
    completes. Error counters read a name kept as text by its prefix.
  - Still interned: the `eventexpr` of a send to an I/O processor, since
    `ta_send.event` carries the name by ID and a send the processor
    refuses is kept as a submitted delayed send, which holds names by ID;
    and the names of delayed sends, until they are envelopes.
  - Tested in `tests/name_matching_test.c` (claims 6, 7 and 11) and the
    invoke-ID checks of `inspection_test`, `send_id_test` and
    `scxml_uri_test`.
- **Targets in `targetexpr`, 2026-10-06.** A `#_<invokeid>` target is
  looked up (since the previous note), and text that is no target fails
  the send with nothing interned (`tests/name_matching_test.c`, claim 12).
  The family of a `#_scxml_<session>` URI is looked up first, and an
  authored family is an atom already. A family no definition of this
  system has is still interned: a host's loader may bring such a session
  in, and a waiting entry names its destination by the family atom.
  Looking it up only made napi's `test/session-loads.js`
  (`anotherIncarnation`) wait forever, since no load was asked for. It
  stops when waiting entries keep the address's bytes (envelope.md), which
  the native records of 5.1 and the images of 5.3 carry. A named address
  in a target is a counted handle (ADR 0065), not an atom.

**Delayed and I/O-processor sends (4.3c).** A delayed `<send eventexpr>`
keeps its name as text in its timer envelope, the text's second field;
inspection and export copy it beside their records
(`ta_delayed_send_info.event_text`), session images and offload bytes
carry it, and an I/O processor reads `ta_send.event_text`. Two run-time
names are still interned, both rare: the eventexpr of a send held for an
unpublished session (held sends keep a name ID until publish), and a
local child's `done.invoke.<id>` kept as a stored send when its parent's
pool is full. `ta_event_name_intern` and `ta_system_intern_atom` remain
for hosts that build authored names' IDs and register types.
