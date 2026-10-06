# Module map

The public API is `include/tinyactors.h` (ADR 0041). The module headers below
are private and hold layouts and internal functions.

| Header | Responsibility |
|---|---|
| `memory.h` | `ta_mem_alloc`/`_free`/`_copy`/`_zero` over the injected allocator, alignment, `TA_ARRAY_COUNT` |
| `str.h` | `ta_str` equality and construction from a C string |
| `vec.h` | `ta_vec`, the growable array |
| `bitset.h` | `ta_bits_*`, bitsets of 64-bit words |
| `span.h` | `ta_span32`, an offset and length into an array |
| `text_writer.h` | the bounded text writer and `ta_text_render`; decimal and hex formatters |
| `hash.h` | FNV-1a (32- and 64-bit) and the fixed 64-bit mixer |
| `handle.h` | generation handles: a slot and a generation packed in 64 bits, for actor and definition IDs |
| `layout.h` | the layout builder: several regions in one block, with checked offsets and total |
| `arena.h` | transient allocation |
| `index_pool.h` | fixed-size items addressed by `u32` index in pages that never move: take, give, refill up to a ceiling, trim |
| `table.h` | open-addressing hash table of `u32` values, keys hashed and compared by the owner; fixed, or growing only on an explicit reserve |
| `atom.h` | ordinary string interning, on a `table.h` hash table |
| `trie.h` | trie of atoms: paths of atoms interned as nodes keyed by (parent, segment), on a `table.h` hash table; event names are one user |
| `value.h` | host value ownership helpers |
| `event.h` | event envelope and queued-event ownership |
| `scxml_uri.h` | the SCXML target URI grammar, parsed and written (ADR 0044) |
| `routing.h` | actor-reference helpers and external-delivery ownership (see its file comment) |
| `clock.h` | caller-supplied logical time |
| `configuration.h` | the active atomic states, one bit per state ID (see its file comment) |
| `history.h` | shallow/deep history storage layout |
| `continuation.h` | paused macrostep interpreter state |
| `deadline.h` | delayed sends as envelopes: the deadline index and owner lists (ADR 0101) |
| `datamodel.h` | the data model's view of its actor (see its file comment) |
| `io.h` | the I/O processor registry entry and origin rendering (see its file comment) |
| `invoke.h` | invocation bookkeeping, retirement and restore (see its file comment) |
| `definition.h` | immutable compiled statechart revision, and the event-descriptor matcher (see its file comment) |
| `builder.h` | mutable construction/compilation front end |
| `export.h` | rebuilds a compiled definition's authoring spec (`ta_definition_export`) |
| `actor.h` | named actor protocol, resident session state, operational inspection |
| `session_image.h` | in-memory interpreter capture/reconstruction |
| `system.h` | the shared runtime container: tables, registries, run buffers (see its file comment) |
| `interpreter.h` | SCXML execution engine |
| `internal.h` | the umbrella include, in dependency order (see its file comment) |

The module boundaries are intended to stay small, but the exact structs are not public ABI.
