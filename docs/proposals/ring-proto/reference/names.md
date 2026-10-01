# Names: what is interned, and what stays bytes

**Status:** Sketch (2026-10-01), part of [the ring protocol proposal](../README.md)

Core deals with several kinds of names: event names, send IDs, invocation
IDs, I/O processor types and actor addresses. Today all of them are
interned into tables that only grow. This document splits them in two:

- **Authored names** are interned once, when a definition is compiled.
- **Run-time names** stay bytes. They live in the **name area**, and core
  looks them up but never adds them.

Matching works on both, and gives the same answers as it does today.

## What core does today

Event names are a trie of atom segments (core ADR 0007,
`src/event_name.h`). Atoms are a hash table over an arena
(`src/atom.h`). Neither table has a way to remove an entry: besides
`ta_atom_table_init` and `ta_atom_table_destroy`, `atom.h` declares
`ta_atom_intern`, `ta_atom_text`, `ta_atom_find` and `ta_atom_valid`, and no
removal. Names computed while the system runs are
interned into these tables:

| Where | What is interned | When |
|---|---|---|
| `src/interpreter.c:1850` (`ta_send_event_name`) | the text of every `eventexpr`, through `ta_event_name_intern_dotted` | inside a macrostep, for each `<send>` |
| `src/interpreter.c:1819` | the text of every `typeexpr`, as an atom | inside a macrostep |
| `src/invoke.c:957-973` (`ta_invoke_error_name`) | `error.communication.invoke.<id>.<reason>`. The invocation's own ID text and the host's reason each become atoms, and the name becomes trie nodes. | each time an invocation fails |
| `ta_event_name_intern`, `ta_send_id_intern`, `ta_system_intern_invoke_id` (public, `include/tinyactors.h`) | whatever text the host passes | whenever a host turns external input or a log record into IDs |

This causes two problems.

1. **Allocation inside a macrostep.** ADR 0070 lists "interning an
   `eventexpr`, `typeexpr` or target" among the allocations that can fail
   in the middle of a run. Under this proposal a macrostep allocates
   nothing ([macrostep.md](macrostep.md)), so these must go.
2. **Memory that grows without bound.** Every distinct run-time name stays
   for the life of the system:
   - an `eventexpr` such as `'order.' + id` adds one trie node and one atom
     per order;
   - a generated invocation ID (`<stateid>.<platformid>`) is unique per
     invocation, so every failed invocation adds atoms and nodes that are
     never used again;
   - a host that maps client-supplied text to event names (an HTTP route, a
     webhook's event type, a WebSocket message) lets **any client** grow the
     tables. Posting random event names is a memory-exhaustion attack on any
     long-running system.

## The split

| Name | Kind | Stored as |
|---|---|---|
| Every event descriptor in a definition's transitions (`order.paid`, `error.*`, `done.invoke.payment`) | authored | atoms, interned when the definition is registered |
| The generated prefixes core itself names (`done.state`, `done.invoke`, `error.execution`, `error.communication`, `error.platform`, and the `invoke` segment of `error.communication.invoke`) | authored by core | atoms interned at system creation. Today's `TA_EVENT_NAME_*` cover all but `invoke`, which `ta_invoke_error_name` (`src/invoke.c`) interns at run time; it would join them. |
| `<send event>`, `<raise event>`, a literal `<send id>`, `<invoke id>`, `<send type>` | authored | atoms, interned at registration |
| The value of `eventexpr`, `typeexpr`, `targetexpr`, `idlocation` text, `sendidexpr` | run time | bytes in the name area |
| Names and IDs in external input ([`h2c.event`](../protocol/generated/MESSAGES.md#h2cevent): `name`, `sendid`, `origin`, `origin_type`) | run time | bytes in the name area |
| A generated send ID `#_send.<n>` | run time | a counter in `ta_send_id`, which was never text (ADR 0048) |
| A generated invocation ID `<stateid>.<platformid>` | run time | a state ID and a counter, rendered to text on demand like `#_send.<n>` |
| An invocation error's reason (ADR 0081) | run time | bytes |
| The address of a resident session | run time, but bounded by residents | a counted handle (ADR 0065), taken at spawn or load, outside any macrostep |
| The address of a session that is not resident | run time | bytes in the name area, on the waiting envelope ([envelope.md](envelope.md)) |

The atom tables therefore grow only when a definition is registered. Their
size is bounded by the definitions an operator registers, not by traffic.

**Open point.** Today atoms are never freed, even when a definition is
(ADR 0080). Counting references per definition would let `DEFINITION_FREED`
release them. That is optional: what can grow without bound is the run-time
names, and they no longer reach the tables.

## Matching without interning

The matcher already compares atoms, not trie IDs. `ta_pattern_match_run`
(`src/definition.c:66-102`) is the two-pointer wildcard matcher of ADR 0060
section 5. It walks an event's segments as `ta_atom`s against a
descriptor's segments, in O(event × descriptor) time and O(1) space. So
only the step that produces the event's atoms changes:

1. **When an envelope is taken,** core splits its name bytes at the dots.
2. **Each segment is looked up** with `ta_atom_find` (`src/atom.h`). This is
   a hash lookup that never inserts. A segment that is an atom gives that
   atom; any other gives `TA_ATOM_NONE`, written here as **UNKNOWN**.
3. **The atoms go into scratch**, which has room for `eventNameDepth`
   segments. A name with more segments is refused when admitted
   ([`c2h.bounced`](../protocol/generated/MESSAGES.md#c2hbounced),
   `invalid`). One produced inside a macrostep is an invalid name, so the
   `<send>` fails as plain `error.execution`, as any other invalid event
   name does.
4. **The matcher runs unchanged.** No descriptor segment is UNKNOWN, so
   UNKNOWN matches only `*`.

Every segment of a descriptor is an atom. So if a descriptor matches an
event's text, every segment it compares against with equality is a known
atom, and gets the same atom from the lookup. The answers are the same as
matching on the interned text.

### Worked examples

The definition's descriptors are `order`, `order.*.paid`,
`error.communication.invoke.payment` (the chart has `<invoke id="payment">`)
and `*`. The definition registers the atoms `order`, `paid`, `error`,
`communication`, `invoke` and `payment`.

| Event (bytes) | Resolved segments | `order` | `order.*.paid` | `error.communication.invoke.payment` | `*` |
|---|---|---|---|---|---|
| `order.123.paid` | order, UNKNOWN, paid | ✓ prefix | ✓ (`*` takes UNKNOWN) | ✗ | ✓ |
| `order.124` | order, UNKNOWN | ✓ prefix | ✗ (no `paid`) | ✗ | ✓ |
| `orders.1` | UNKNOWN, UNKNOWN | ✗ | ✗ | ✗ | ✓ |
| `error.communication.invoke.payment.timeout` | error, communication, invoke, payment, UNKNOWN | ✗ | ✗ | ✓ prefix | ✓ |
| `error.communication.invoke.s3.7.unavailable` (generated ID `s3.7`) | error, communication, invoke, UNKNOWN, UNKNOWN, UNKNOWN | ✗ | ✗ | ✗ | ✓ |
| `x9f2.qq.zz` from an attacker | UNKNOWN ×3 | ✗ | ✗ | ✗ | ✓ |

The last row costs a hash of three short strings and nothing more, and it
leaves no memory behind once its envelope is freed.

## The name area

The name area is a bounded region, `nameAreaBytes`, created with the pool.
It holds the text of every envelope:

- the event name;
- the origin, origintype and invokeid of external input;
- an unresolved target address;
- a send ID that came from outside;
- an invocation error's reason.

**Layout: fixed chunks.** Envelopes are freed out of order (a timer outlives
the mail around it), so a bump arena would fragment. Instead, the area is
an array of 64-byte chunks. Each holds 60 bytes of text and a `u32` index of
the next chunk. A free list links unused chunks. An envelope's `text` field
holds its first chunk and the total length ([envelope.md](envelope.md)).

- Taking and freeing are O(1), with no fragmentation.
- Most envelopes need one chunk: `order.paid`, plus an empty origin, fits
  easily.
- The overhead is 4 bytes in 64, plus the unused end of the last chunk.
  This is an **estimate** to check against real traffic.

**When it is full,** it behaves as a full pool does:

- admission from h2c stops above `admissionWatermark`;
- a `<send>` whose text doesn't fit fails as `error.execution.capacity`.

[envelope.md](envelope.md#running-out) covers both.

**Text leaves through rings as spans,** and is copied into a record's tail
when written: [`c2h.input`](../protocol/generated/MESSAGES.md#c2hinput),
[`c2h.send`](../protocol/generated/MESSAGES.md#c2hsend),
[`tel.trace`](../protocol/generated/MESSAGES.md#teltrace). Images and logs
store text, so ADR 0007's caveat goes away: "persisted data must not assume
process-local event-name IDs remain stable". No ID was persisted.

## IDs, one by one

### Send IDs

- **Generated** (`#_send.<n>`): the sending session's counter, a `u64` in
  `ta_send_id`, as ADR 0048 already defines. Core renders it as text for
  `idlocation`, `_event.sendid` and the `sendid` field of records. No table
  is involved.
- **Literal** (`<send id="t">`): authored, so interned at registration.
- **`<cancel sendidexpr>`:** the text is parsed. The generated form always
  parses. A literal is looked up, and a literal no definition authored can't
  name a pending send of this session, so `<cancel>` does nothing, as SCXML
  says.
- **A send ID from outside** (`_event.sendid` of an external event, or of
  another system's mail): bytes. Core never needs it as an ID; it only hands
  it to the data model as text.

### Invocation IDs

- **Authored** (`<invoke id="payment">`): interned at registration, so
  descriptors such as `done.invoke.payment` match it.
- **Generated:** a state ID and a per-session counter, rendered as
  `<stateid>.<platformid>` when text is needed
  ([`c2h.invoke`](../protocol/generated/MESSAGES.md#c2hinvoke) `invokeid`,
  `_event.invokeid`).
- **`done.invoke.<id>` and `error.communication.invoke.<id>.<reason>`** are
  built as bytes in the envelope's text, not as trie nodes. Matching then
  resolves `done`, `invoke`, `error`, `communication` and authored IDs to
  atoms, and the rest to UNKNOWN.
- **[`h2c.invoke_report`](../protocol/generated/MESSAGES.md#h2cinvoke_report)**
  names the invocation by its text. Core parses the generated form, or
  looks up an authored ID, and doesn't intern either. `ta_system_intern_invoke_id`
  and `ta_send_id_intern` are no longer needed: a replayed
  [`c2h.input`](../protocol/generated/MESSAGES.md#c2hinput) record carries
  text, and the session's own definition knows the literals.

### I/O processor types

`<send type>` literals are authored and interned at registration. A
`typeexpr` value is looked up among the registered I/O processors (core ADR
0068 keeps the registry). A type that isn't registered can't be dispatched,
so it is `error.execution`, as SCXML says. Nothing is interned.

### Addresses

- **A resident session's address** is a counted handle (ADR 0065), created
  at spawn or load, outside any macrostep. A `targetexpr` that names it is
  resolved by lookup. Taking a reference to the handle for the envelope is
  a count, not an allocation.
- **The address of a session that isn't resident** (unloaded, or never
  spawned in a family that has a loader) has no handle. The envelope keeps
  the bytes, the waiting list is keyed by them, and
  [`c2h.load_needed`](../protocol/generated/MESSAGES.md#c2hload_needed)
  carries them. The handle is created at
  [`h2c.load`](../protocol/generated/MESSAGES.md#h2cload), outside a
  macrostep ([unloading.md](unloading.md)).

## What changes for the data model and the host

- **`_event.name` reaches the data model as text** from the envelope, not as
  a `ta_event_name_id` that the data model formats with
  `ta_event_name_format`. The ECMAScript and JSON data models want text
  anyway ([data-model.md](data-model.md)).
- **The host never interns.** [`h2c.event`](../protocol/generated/MESSAGES.md#h2cevent)
  carries the name as bytes. `ta_event_name_intern` stays only for the
  compiler and for tools that build descriptors.
- **Trace records** already carry a `ta_str name`
  ([telemetry.md](telemetry.md)).
