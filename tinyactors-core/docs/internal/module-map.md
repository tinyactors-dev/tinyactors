# Module map

| Header | Responsibility |
|---|---|
| `types.h` | low-level internal scalar/layout helpers |
| `arena.h` | transient allocation |
| `atom.h` | ordinary string interning |
| `event_name.h` | structured dot-token event-name interning/matching |
| `value.h` | host value ownership helpers |
| `event.h` | event envelope and queued-event ownership |
| `fault.h` | plugin fault -> SCXML error event mapping |
| `clock.h` | caller-supplied logical time |
| `configuration.h` | active atomic-state set representation |
| `history.h` | shallow/deep history storage layout |
| `continuation.h` | paused macrostep interpreter state |
| `timer.h` | delayed-send records and deadline index |
| `datamodel.h` | host data-model adapter/context |
| `io.h` | I/O processor registration |
| `invoke.h` | invoked-service adapters and active records |
| `definition.h` | immutable compiled statechart |
| `builder.h` | mutable construction/compilation front end |
| `actor.h` | actor directory/resident session state |
| `snapshot.h` | streaming session serialization |
| `store.h` | optional host persistence backend |
| `system.h` | shared runtime container/registries |
| `interpreter.h` | SCXML execution engine |
| `internal.h` | implementation umbrella include |

The module boundaries are intended to stay small, but the exact structs are not public ABI.
