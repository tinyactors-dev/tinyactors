# Names: what is interned, and how events match

Core names things with atoms (`src/atom.h`) and event names with paths of
atoms in the event-name trie (`src/trie.h`, ADR 0007). Neither table
removes an entry, so every text interned stays for the life of the system.
This document says which names reach those tables and when, and how an
event is matched against a definition's descriptors. Each unit's own
contract is in its header: `atom.h`, `trie.h`, `names.h` (the name
resolver) and `definition.h` (the matcher).

## Authored names

Interned when a definition is compiled, or when the system is created:

- every segment of every event descriptor of a transition (`order.paid`,
  `error.*`, `done.invoke.payment`);
- `<raise event>`, `<send event>`, literal `<send id>` and `<invoke id>`,
  `<send type>` and `<invoke type>`, and state IDs;
- at system creation, the built-in event names (`error`,
  `error.execution`, `error.communication`, `error.platform`, `done`,
  `done.state`, `done.invoke`), the `invoke` segment of
  `error.communication.invoke.<id>.<reason>`, and the SCXML I/O processor
  and invoker types. So every prefix of a name core generates is an atom
  before any chart runs.

## Run-time names

| Name | What happens to it |
|---|---|
| `<send typeexpr>`, `<invoke typeexpr>` | looked up with `ta_atom_find`; text that is no atom names no processor or invoker, and the action fails as `error.execution` |
| `<cancel sendidexpr>` | parsed, or looked up (`ta_send_id_from_text`); never interned |
| `<send eventexpr>` | interned into the trie (`ta_trie_intern_dotted`), inside the macrostep |
| a generated invocation ID `<stateid>.<platformid>` | interned as one atom; `done.invoke.<id>` adds one trie node per dotted part of it |
| `error.communication.invoke.<id>.<reason>` | the ID (one segment, dots and all) and the host's reason are interned, and the name becomes trie nodes |
| `#_<invokeid>` targets, the family of a `#_scxml_` session URI | interned as atoms |
| a named address in a `targetexpr` | a counted handle in the address table (ADR 0065), not an atom |
| external input | the host passes the name as text (`ta_actor_deliver_params.name`), which its envelope keeps; send and invocation IDs are parsed or looked up (`ta_send_id_parse`, `ta_system_invoke_id_find`); a host interns only the names of delayed sends it restores (`ta_event_name_intern`) |

The interned run-time names exist because an event carries its name as a
trie node (`ta_event.name`), and an invocation ID is an atom: the text has
nowhere else to live while the event waits in a queue or a timer.

## Matching

`ta_event_pattern_matches` compares atoms only (ADR 0060 section 5): a
descriptor that runs out first matches by prefix, a `*` segment takes zero
or more event segments, and an event segment that is no atom (the
resolver's UNKNOWN, `TA_ATOM_NONE`) equals no descriptor segment, so only
`*` takes it. When the interpreter selects transitions for a taken event,
it turns the event's name into its segments once, from the event's trie
node, and matches every descriptor against that list.

The name resolver (`ta_names_resolve`) gives the same list from a name's
bytes without interning: it splits them at the dots and looks each segment
up. Every descriptor segment that is not `*` is an atom, so each segment a
descriptor compares with equality resolves to the same atom it would have
been interned as, and lookup answers as the trie does
(`tests/name_matching_test.c` proves it over the worked examples below and
a corpus of names, and that random names add nothing to either table).

One name differs between the two: the trie keeps a generated invocation ID
in `error.communication.invoke.<id>.<reason>` as one segment (`s3.7`),
while the bytes of the same name resolve it as two (`s3`, `7`), as
`done.invoke.<id>` already splits it. Only a descriptor that names the
parts of a dotted invocation ID in an error name sees the difference.

### Worked examples

A definition with the descriptors `order`, `order.*.paid`,
`error.communication.invoke.payment` (the chart has
`<invoke id="payment">`) and `*` registers the atoms `order`, `paid`,
`error`, `communication`, `invoke` and `payment`.

| Event (bytes) | Resolved segments | `order` | `order.*.paid` | `error.communication.invoke.payment` | `*` |
|---|---|---|---|---|---|
| `order.123.paid` | order, UNKNOWN, paid | ✓ prefix | ✓ (`*` takes UNKNOWN) | ✗ | ✓ |
| `order.124` | order, UNKNOWN | ✓ prefix | ✗ (no `paid`) | ✗ | ✓ |
| `orders.1` | UNKNOWN, UNKNOWN | ✗ | ✗ | ✗ | ✓ |
| `error.communication.invoke.payment.timeout` | error, communication, invoke, payment, UNKNOWN | ✗ | ✗ | ✓ prefix | ✓ |
| `error.communication.invoke.s3.7.unavailable` | error, communication, invoke, UNKNOWN, UNKNOWN, UNKNOWN | ✗ | ✗ | ✗ | ✓ |
| `x9f2.qq.zz` | UNKNOWN ×3 | ✗ | ✗ | ✗ | ✓ |
