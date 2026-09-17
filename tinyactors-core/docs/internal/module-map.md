# Module map

| Header | Responsibility |
|---|---|
| `types.h` | low-level scalar/layout helpers |
| `store.h` | empty compatibility boundary; durability is not a core store API |
| `arena.h` | transient allocation |
| `atom.h` | ordinary string interning |
| `event_name.h` | structured dot-token event-name interning/matching |
| `value.h` | host value ownership helpers |
| `event.h` | event envelope and queued-event ownership |
| `fault.h` | plugin fault -> SCXML error event mapping |
| `routing.h` | host scheduling/routing of normalized external actor events |
| `clock.h` | caller-supplied logical time |
| `configuration.h` | active atomic-state set representation |
| `history.h` | shallow/deep history storage layout |
| `continuation.h` | paused macrostep interpreter state |
| `timer.h` | delayed-send records and deadline index |
| `datamodel.h` | host data-model adapter/binding/context and inspection root |
| `io.h` | built-in SCXML and custom I/O processor dispatch |
| `invoke.h` | invoked-service adapters and active records |
| `definition.h` | immutable compiled statechart revision |
| `builder.h` | mutable construction/compilation front end |
| `actor.h` | named actor protocol, resident session state, operational inspection |
| `session_image.h` | in-memory interpreter capture/reconstruction |
| `system.h` | shared runtime container/registries |
| `interpreter.h` | SCXML execution engine |
| `internal.h` | implementation umbrella include |

The module boundaries are intended to stay small, but the exact structs are not public ABI.
