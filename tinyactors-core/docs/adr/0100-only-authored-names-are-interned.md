# ADR 0100: Only authored names are interned

**Status:** Accepted (2026-10-04); implementation pending (ring-proto Phase 3 for matching by lookup, Phase 4.2 for the name area). Refines ADR 0007: authored names stay interned atoms and trie paths; names made at run time are bytes, matched by lookup and never interned.
**Date:** 2026-10-04

## Context

ADR 0007 interns every event name as a path of atom segments in a trie, and
matches on that structure. The atom table (`src/atom.h`) and the trie of
atoms (block D7, `src/trie.h`, which event names use) can only grow: they
have no removal. Core interns names it computes while the system runs:

| Where | What is interned | When |
|---|---|---|
| `ta_send_event_name` (`src/interpreter.c`) | the value of every `eventexpr` | inside a macrostep, for each `<send>` |
| `src/interpreter.c` | the value of every `typeexpr` | inside a macrostep |
| `ta_invoke_error_name` (`src/invoke.c`) | `error.communication.invoke.<id>.<reason>`: the invocation's ID and the host's reason become atoms, and the name trie nodes | each time an invocation fails |
| `src/scxml_uri.c` | evaluated targets | inside a macrostep |
| `ta_event_name_intern`, `ta_send_id_intern`, `ta_system_intern_invoke_id` (public) | whatever text the host passes | whenever a host turns external input into IDs |

That causes two problems ([names.md](../../../docs/proposals/ring-proto/reference/names.md)):

1. **Allocation inside a macrostep.** ADR 0070 lists interning an
   `eventexpr`, `typeexpr` or target among the allocations that can fail
   mid-run. ADR 0102 forbids them.
2. **Memory that grows without bound.** Every distinct run-time name stays
   for the life of the system. An `eventexpr` such as `'order.' + id` adds
   an atom and a trie node per order. A generated invocation ID is unique
   per invocation, so every failed invocation adds names never used again.
   A host that maps client text to event names (an HTTP route, a webhook's
   event type, a WebSocket message) lets **any client** grow the tables:
   posting random event names exhausts the memory of any long-running
   system.

The matcher does not need interned events. `ta_pattern_match_run`
(`src/definition.c`) is ADR 0060's two-pointer wildcard matcher: it walks
an event's segments as atoms against a descriptor's, comparing atoms. Only
the step that produces the event's atoms has to change.

## Decision

### 1. Authored names are interned, once

These are atoms (and trie paths where they are dotted), interned when a
definition is registered, outside any macrostep:

- every event descriptor of a definition's transitions;
- `<send event>`, `<raise event>`, a literal `<send id>`, `<invoke id>` and
  `<send type>`, `<invoke type>`;
- state IDs and the other identities the compiler already interns.

The names core itself generates are atoms interned at **system creation**:
`done.state`, `done.invoke`, `error.execution`, `error.communication`,
`error.platform` (today's `TA_EVENT_NAME_*`), the `invoke` segment of
`error.communication.invoke`, which `ta_invoke_error_name` interns at run
time today, and the segments of ADR 0102's capacity errors: `capacity`,
`pool`, `names`, `internal`, `c2h` and `record`.

So the atom table and the trie grow only when a definition is registered.
Their size is bounded by the definitions an operator registers, not by
traffic. Atoms are still never freed (ADR 0080's freed definitions keep
theirs); that is optional to fix, because what can grow without bound no
longer reaches the tables.

### 2. Run-time names are bytes, matched by lookup

| Name | Stored as |
|---|---|
| the value of `eventexpr`, `typeexpr`, `targetexpr`, `sendidexpr`, and the text `idlocation` receives | bytes in the name area |
| names and IDs of external input: name, send ID, origin, origin type, invoke ID | bytes in the name area |
| an invocation error's reason (ADR 0081) | bytes in the name area |
| a generated send ID `#_send.<n>` | the sending session's counter in `ta_send_id` (ADR 0048), rendered as text on demand |
| a generated invocation ID `<stateid>.<platformid>` | a state ID and a per-session counter, rendered on demand the same way |
| `done.invoke.<id>`, `error.communication.invoke.<id>.<reason>` | built as bytes in the envelope's text, never as trie nodes |
| the address of a resident session | its counted handle (ADR 0065), found by lookup; taking a reference is a count, not an allocation |
| the address of a session that is not resident | bytes in the name area, on the waiting envelope (ADR 0099) |

**Matching** uses the name resolver (block D4), when an envelope or an
internal event is **taken**:

1. split the name bytes at the dots;
2. look each segment up with `ta_atom_find`, which never inserts; a
   segment that is not an atom becomes `TA_ATOM_NONE`, "unknown";
3. write the atoms into scratch, which has room for `eventNameDepth`
   segments;
4. run the matcher unchanged. No descriptor segment is unknown, so unknown
   matches only `*`.

Every segment a descriptor compares by equality is an authored atom, so a
lookup gives the event the same atom interning would have: **the answers
are the same as today's.** An attacker's `x9f2.qq.zz` costs a hash of three
short strings and leaves nothing behind once its envelope is freed.

Resolving at take, not at admission, keeps the envelope at 88 bytes (no
array of atoms in it), sees atoms a definition registered while the
envelope waited, and costs nothing for an envelope that is never taken.

**Too deep.** A name with more segments than `eventNameDepth` is invalid:
`ta_actor_deliver()` answers `TA_ERR_INVALID` (later `c2h.bounced`,
`invalid`), and a `<send>` that computes one fails as plain
`error.execution`, as any other invalid event name does.

**Other lookups:**

- **`typeexpr`** is looked up among the registered I/O processor types; a
  type that is not registered is `error.execution`, as SCXML says.
- **`<cancel sendidexpr>`** parses the generated `#_send.<n>` form, or
  looks a literal up as an atom. A literal no definition authored cannot
  name a pending send of this session, so the `<cancel>` does nothing.
- **A send ID or invoke ID from outside** is only handed to the data model
  as text; core never needs it as an ID.

### 3. The name area

The name area holds the text of every envelope. It is bounded by
`nameAreaBytes` (default 262,144, minimum `maxRecordBytes`, maximum 1 GiB)
and fills on demand at safe points, under ADR 0099's admission watermark
and macrostep reserve.

- **Fixed chunks.** It is an index pool (block A5) of 64-byte chunks, each
  60 bytes of text and a `u32` link to the next, read and written as a
  chunk chain (block A6). Envelopes are freed out of order, so a bump arena
  would fragment; chunks never do. Taking and freeing are O(1).
- **An envelope's text** is one chain, named by its first chunk and total
  length. It holds, each prefixed by a `u16` length: its trace parent when
  it has one (ADR 0099), the name, origin, origin type, invoke ID, target
  address, I/O processor type and an external send ID. Most envelopes need
  one chunk: `order.paid` and an empty origin fit easily.
- **When it is full,** a host input is refused (`TA_ERR_FULL`) and a
  `<send>` whose text does not fit fails as
  `error.execution.capacity.names` (ADR 0102).
- **Text leaves as spans,** copied into a record's tail when written
  (`tel.trace` from ADR 0098, `c2h.send` and `c2h.input` later). Images and
  logs store text.

### 4. In two steps

- **Phase 3 (step 3.1): match by lookup.** When an event is taken, resolve
  its name with D4 and run the existing matcher over the result, while
  names are still interned. The generated prefixes, `invoke` among them,
  become atoms at system creation. Both paths must give the same answers,
  so the switch changes no behaviour.
- **Phase 4.2: store run-time names as bytes,** in the name area that
  arrives with the pool, and delete run-time interning: the `eventexpr`
  and `typeexpr` sites, `ta_invoke_error_name`'s atoms, the interning in
  `scxml_uri.c`, and the public `ta_send_id_intern` and
  `ta_system_intern_invoke_id`. `ta_event_name_intern` stays for the
  compiler and for tools that build descriptors.

### 5. What a data model and a host see

- `_event.name` reaches the data model as text from the envelope, not as an
  ID the data model formats. The ECMAScript, JavaScript and jq data models
  want text anyway.
- A host passes event names as bytes (`ta_event`'s name becomes a
  `ta_str`); it never interns.
- Trace records already carry the name as text (ADR 0098).

## Consequences

- **The memory growth is fixed.** Traffic, including hostile traffic, no
  longer grows the atom table or the trie. A core test posts random names
  and checks that the atom count and the trie's node count do not change.
- **ADR 0007's caveat goes:** "persisted data must not assume process-local
  event-name IDs remain stable". Nothing persists a run-time name's ID,
  because it has none.
- **Cost per event:** one hash lookup per segment when the event is taken,
  paid once per event, not per descriptor; admission copies text into the
  name area instead of interning it. A generated name is rendered when
  text is needed.
- **Memory:** run-time names cost name-area chunks only while their
  envelope lives, bounded by `nameAreaBytes`. Per actor: nothing.
- **API:** `ta_send_id_intern` and `ta_system_intern_invoke_id` go;
  `ta_event`'s name is bytes. The kernel's interned-name cache
  (`events.c`, napi ADR 0004) has nothing left to cache; the kernel and
  napi change their call sites in the same commit.
- **Tests:** the worked examples of
  [names.md](../../../docs/proposals/ring-proto/reference/names.md#worked-examples)
  as a core test; the IRP tests in the test host and napi still pass.
