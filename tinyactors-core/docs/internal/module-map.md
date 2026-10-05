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
| `store.h` | empty compatibility boundary; durability is not a core store API |
| `arena.h` | transient allocation |
| `atom.h` | ordinary string interning |
| `event_name.h` | structured dot-token event-name interning/matching |
| `value.h` | host value ownership helpers |
| `event.h` | event envelope and queued-event ownership |
| `fault.h` | plugin fault -> SCXML error event mapping |
| `scxml_uri.h` | the SCXML target URI grammar, parsed and written (ADR 0044) |
| `routing.h` | host scheduling/routing of normalized external actor events |
| `clock.h` | caller-supplied logical time |
| `configuration.h` | active atomic-state set representation |
| `history.h` | shallow/deep history storage layout |
| `continuation.h` | paused macrostep interpreter state |
| `timer.h` | per-owner delayed-send lists and the owner deadline index (ADR 0054) |
| `datamodel.h` | host data-model adapter/binding/context and inspection root |
| `io.h` | built-in SCXML and custom I/O processor dispatch |
| `invoke.h` | invoked-service adapters and active records |
| `definition.h` | immutable compiled statechart revision |
| `builder.h` | mutable construction/compilation front end |
| `export.h` | rebuilds a compiled definition's authoring spec (`ta_definition_export`) |
| `actor.h` | named actor protocol, resident session state, operational inspection |
| `session_image.h` | in-memory interpreter capture/reconstruction |
| `system.h` | shared runtime container/registries |
| `interpreter.h` | SCXML execution engine |
| `internal.h` | implementation umbrella include |

The module boundaries are intended to stay small, but the exact structs are not public ABI.
